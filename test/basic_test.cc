// This file is part of corral, a lightweight C++20 coroutine library.
//
// Copyright (c) 2024-2025 Hudson River Trading LLC
// <opensource@hudson-trading.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.
//
// SPDX-License-Identifier: MIT

#define CATCH_CONFIG_MAIN

#include <cstdint>
#include <optional>
#include <ranges>
#include <vector>

#include "config.h"

// Windows headers #define infamous min(a,b) and max(a,b) macros,
// which break `std::max()` and `std::numeric_limits<T>::max()`,
// unless parenthesized (`(std::max)(a,b)`) or qualified
// (`std::max<size_t>(a,b)`).
//
// #define these here to catch any possible uses in the code below.
#define min(a, b) __corral_dont_use_min_max(a, b)
#define max(a, b) __corral_dont_use_min_max(a, b)

#include "../corral/corral.h"
#include "TestEventLoop.h"
#include "helpers.h"

using namespace corral;
using namespace corral::testing;
using namespace std::chrono_literals;
using corral::detail::ScopeGuard;

namespace {
#define CORRAL_TEST_CASE(...)                                                  \
    CORRAL_TEST_CASE_IMPL(TestEventLoop, t, __VA_ARGS__)


CORRAL_TEST_CASE("smoke") {
    auto one = [&]() -> Task<int> { co_return 1; };
    auto two = [&]() -> Task<int> { co_return 2; };
    auto three = [&]() -> Task<int> { co_return 3; };

    auto six = [&]() -> Task<int> {
        int x = co_await one;
        int y = co_await two;
        int z = co_await three;
        co_return x + y + z;
    };

    int ret = co_await six;
    CATCH_CHECK(ret == 6);
}

CORRAL_TEST_CASE("task-awaitable-types") {
    CATCH_SECTION("task-object") {
        auto taskLambda = []() -> Task<int> { co_return 42; };
        Task<int> task = taskLambda();
        int x = co_await task;
        CATCH_CHECK(x == 42);
    }

    CATCH_SECTION("task-callable") {
        int x = co_await []() -> Task<int> { co_return 43; };
        CATCH_CHECK(x == 43);
    }
}

CORRAL_TEST_CASE("interleave") {
    std::vector<int> v;
    auto one = [&]() -> Task<> {
        co_await t.sleep(1ms);
        v.push_back(1);
        co_await t.sleep(2ms);
        v.push_back(3);
        co_await t.sleep(2ms);
        v.push_back(5);
    };

    auto two = [&]() -> Task<> {
        co_await t.sleep(2ms);
        v.push_back(2);
        co_await t.sleep(2ms);
        v.push_back(4);
        co_await t.sleep(2ms);
        v.push_back(6);
    };

    co_await allOf(one, two);
    CATCH_CHECK(v == (std::vector<int>{1, 2, 3, 4, 5, 6}));
}

CORRAL_TEST_CASE("nonmoveable") {
    int v = co_await NonmoveableAwaiter{};
    CATCH_CHECK(v == 42);
    v = co_await NonmoveableAwaitable{};
    CATCH_CHECK(v == 42);
}

CORRAL_TEST_CASE("return-types") {
    int x = 42;
    const int cx = 43;

    auto refTask = [&]() -> Task<int&> { co_return x; };
    int& ref = co_await refTask();
    CATCH_CHECK(&ref == &x);

    auto crefTask = [&]() -> Task<const int&> { co_return cx; };
    const int& cref = co_await crefTask();
    CATCH_CHECK(&cref == &cx);

    auto rrefTask = [&]() -> Task<int&&> { co_return std::move(x); };
    int&& rref = co_await rrefTask();
    CATCH_CHECK(&rref == &x);
}


//
// Handling exceptions
//

#if __cpp_exceptions
CORRAL_TEST_CASE("exceptions") {
    auto bad = [&]() -> Task<> {
        co_await std::suspend_never();
        throw std::runtime_error("boo!");
    };
    CATCH_CHECK_THROWS_WITH(co_await bad, Catch::Equals("boo!"));
}

CORRAL_TEST_CASE("throwing-awaitable") {
    CATCH_SECTION("immediate") {
        CATCH_CHECK_THROWS(co_await ThrowingAwaitable{});
    }

    CATCH_SECTION("combiner-first") {
        CATCH_CHECK_THROWS(co_await anyOf(ThrowingAwaitable(), t.sleep(5ms)));
        CATCH_CHECK(t.now() == 0ms);
    }

    CATCH_SECTION("combiner-last") {
        CATCH_CHECK_THROWS(co_await anyOf(t.sleep(5ms), ThrowingAwaitable{}));
        CATCH_CHECK(t.now() == 0ms);
    }

    CATCH_SECTION("combiner-noncancellable") {
        CATCH_CHECK_THROWS(co_await anyOf(t.sleep(5ms, uninterruptible),
                                          ThrowingAwaitable{}));
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("shared") {
        Shared aw{ThrowingAwaitable{}};
        CATCH_CHECK_THROWS(co_await aw);
        CATCH_CHECK_THROWS(co_await aw);
    }
}
#endif


//
// Cancellation
//

CORRAL_TEST_CASE("basic-cancel") {
    bool started = false;
    auto task = [&]() -> Task<> {
        co_await t.sleep(1ms);
        started = true;
        co_await t.sleep(2ms);
        CATCH_CHECK(false);
    };

    co_await anyOf(task, t.sleep(2ms));
    CATCH_CHECK(started);

    co_await t.sleep(5ms);
}

CORRAL_TEST_CASE("self-cancel") {
    bool started = false;
    Event cancelEvt;
    auto outer = [&]() -> Task<> {
        auto work = [&]() -> Task<> {
            auto interrupt = [&]() -> Task<> {
                started = true;
                co_await t.sleep(1ms);
                cancelEvt.trigger();
            };
            co_await interrupt;
            co_await yield;
            CATCH_CHECK(false);
        };
        co_await anyOf(work, cancelEvt);
    };
    co_await outer;
    CATCH_CHECK(started);
}

CORRAL_TEST_CASE("no-cancel") {
    bool resumed = false;
    auto task = [&]() -> Task<> {
        co_await t.sleep(5ms, uninterruptible);

        CATCH_SECTION("next-cancellable") {
            resumed = true;
            co_await yield;
            CATCH_CHECK(!"should not reach here");
        }

        CATCH_SECTION("next-noncancellable") {
            co_await t.sleep(0ms, uninterruptible);
            resumed = true;
        }

        CATCH_SECTION("next-task") {
            co_await []() -> Task<> { co_return; };
            resumed = true;
        }

        CATCH_SECTION("next-nontrivial") {
            co_await anyOf(
                    [&]() -> Task<> {
                        co_await yield;
                        CATCH_CHECK(!"should not reach here");
                        co_return;
                    },
                    untilCancelledAnd([&]() -> Task<> {
                        resumed = true;
                        co_return;
                    }));
        }
    };
    auto task2 = [&]() -> Task<> {
        co_await anyOf(task, t.sleep(2ms));
        CATCH_CHECK(t.now() == 5ms);
    };

    co_await task2;
    CATCH_CHECK(t.now() == 5ms);
    CATCH_CHECK(resumed);
}

#if __cpp_exceptions
CORRAL_TEST_CASE("no-cancel-exc") {
    CATCH_SECTION("nested-task") {
        CATCH_CHECK_THROWS(co_await [&]() -> Task<> {
            co_await anyOf(t.sleep(1ms), [&]() -> Task<> {
                co_await t.sleep(2ms, uninterruptible);
                co_await [&]() -> Task<> {
                    co_await t.sleep(1ms, uninterruptible);
                    throw std::runtime_error("boo!");
                };
            });
        });
    }

    CATCH_SECTION("all-of") {
        CATCH_CHECK_THROWS(co_await [&]() -> Task<> {
            co_await anyOf(t.sleep(1ms), [&]() -> Task<> {
                co_await t.sleep(2ms, uninterruptible);
                co_await allOf(t.sleep(1ms), [&]() -> Task<> {
                    co_await t.sleep(1ms, uninterruptible);
                    throw std::runtime_error("boo!");
                });
            });
        });
    }
}
#endif


//
// Executors
//

CORRAL_TEST_CASE("executor") {
    bool innerInvoked = false;
    Event evt;
    auto inner = [&]() -> Task<> {
        CORRAL_TRACE("entering inner(), waiting on event");
        co_await evt;
        CORRAL_TRACE("inner() resumed by event");
        innerInvoked = true;
        co_return;
    };

    CORRAL_WITH_NURSERY(n) {
        n.start(inner);
        co_await t.sleep(1ms);

        evt.trigger();
        CATCH_CHECK(!innerInvoked);

        co_await yield;
        CATCH_CHECK(innerInvoked);

        co_return join;
    };
}

CORRAL_TEST_CASE("chained-executors") {
    Event evt, evt2;
    UnsafeNursery n(t);

    CATCH_SECTION("timeout") {
        n.start([&]() -> Task<> {
            co_await t.sleep(1ms);
            evt.trigger();
        });
        co_await evt;
    }

    CATCH_SECTION("event") {
        n.start([&]() -> Task<> {
            co_await evt2;
            evt.trigger();
        });
        evt2.trigger();
        co_await evt;
    }
}

CORRAL_TEST_CASE("multiverse") {
    co_await t.sleep(2ms);

    TestEventLoop t2;

    CATCH_SECTION("run") {
        run(t2, [&t2]() -> Task<void> { co_await t2.sleep(3ms); });
    }
    CATCH_SECTION("unsafe-nursery") {
        UnsafeNursery n(t2);
        n.start([&t2]() -> Task<void> {
            co_await t2.sleep(3ms);
            t2.stop();
        });
        t2.run();
    }

    co_await t.sleep(4ms);

    CATCH_CHECK(t.now() == 6ms);
    CATCH_CHECK(t2.now() == 3ms);
}


} // namespace
