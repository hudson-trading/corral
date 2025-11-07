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

#include <functional>
#include <string>

#include "../corral/corral.h"
#include "TestEventLoop.h"
#include "helpers.h"

using namespace corral;
using namespace corral::testing;
using namespace std::chrono_literals;
using corral::detail::ScopeGuard;
using std::chrono::milliseconds;

#define CORRAL_TEST_CASE(...)                                                  \
    CORRAL_TEST_CASE_IMPL(TestEventLoop, t, __VA_ARGS__)

namespace {


//
// Nursery
//

CORRAL_TEST_CASE("nursery-smoke") {
    size_t count = 0;
    auto incrementAfter = [&](milliseconds delay) -> Task<void> {
        co_await t.sleep(delay);
        ++count;
    };
    CORRAL_WITH_NURSERY(n) {
        n.start(incrementAfter, 2ms);
        n.start(incrementAfter, 3ms);
        n.start(incrementAfter, 5ms);

        co_await t.sleep(4ms);
        CATCH_CHECK(count == 2);

        co_await t.sleep(2ms);
        CATCH_CHECK(count == 3);

        co_return join;
    };
}

CORRAL_TEST_CASE("nursery-extended-lifetime") {
    auto func = [&](const std::string& s) -> Task<void> {
        co_await t.sleep(1ms);
        CATCH_CHECK(s == "hello world! I am a long(ish) string.");
    };

    const std::string ext = "hello world! I am a long(ish) string.";

    CORRAL_WITH_NURSERY(n) {
        CATCH_SECTION("implicit-construction") {
            n.start(func, "hello world! I am a long(ish) string.");
        }

        CATCH_SECTION("existing-object") {
            const std::string str = "hello world! I am a long(ish) string.";
            n.start(func, str);
        }

        CATCH_SECTION("by-reference") {
            // Passing a string defined outside the nursery block by reference
            n.start(func, std::cref(ext));
        }

        co_return join;
    };
}

CORRAL_TEST_CASE("nursery-member-function") {
    struct Test {
        int x = 42;
        Task<void> func(TestEventLoop& tt, int expected) {
            co_await tt.sleep(1ms);
            CATCH_CHECK(x == expected);
        }
    };

    Test obj;
    CORRAL_WITH_NURSERY(n) {
        CATCH_SECTION("ptr") {
            n.start(&Test::func, &obj, std::ref(t), 43);
            obj.x = 43;
        }
        CATCH_SECTION("ref") {
            n.start(&Test::func, std::ref(obj), std::ref(t), 43);
            obj.x = 43;
        }
        CATCH_SECTION("val") {
            // `this` passed by value, so subsequent changes to `obj`
            // should not be visible by the spawned task
            n.start(&Test::func, obj, std::ref(t), 42);
            obj.x = 43;
        }
        co_return join;
    };
    CATCH_CHECK(t.now() == 1ms);
}

CORRAL_TEST_CASE("nursery-member-function-lifetime") {
    struct Test {
        Task<void> byValue(LifetimeTracked t) {
            t.assertValid();
            co_return;
        }
        Task<void> byConstRef(const LifetimeTracked& t) {
            t.assertValid();
            co_return;
        }
        Task<void> byRef(LifetimeTracked& t) {
            t.assertValid();
            co_return;
        }
        Task<void> byRRef(LifetimeTracked&& t) {
            t.assertValid();
            co_return;
        }

        Task<void> moveOnly(std::unique_ptr<int> mm) {
            CATCH_CHECK((*mm == 42));
            co_return;
        }

        Task<void> moveOnlyByConstRef(const std::unique_ptr<int>& mm) {
            CATCH_CHECK((*mm == 42));
            co_return;
        }
    };

    LifetimeTracked lt;
    Test obj;
    auto mm = std::make_unique<int>(42);

    CORRAL_WITH_NURSERY(n) {
        n.start(&Test::byValue, &obj, LifetimeTracked{});
        n.start(&Test::byValue, &obj, LifetimeTracked::Maker{});

        n.start(&Test::byConstRef, &obj, LifetimeTracked{});
        n.start(&Test::byConstRef, &obj, LifetimeTracked::Maker{});
        n.start(&Test::byConstRef, &obj, std::ref(lt));
        n.start(&Test::byConstRef, &obj, std::cref(lt));

        n.start(&Test::byRef, &obj, std::ref(lt));

        n.start(&Test::byRRef, &obj, LifetimeTracked{});
        n.start(&Test::byRRef, &obj, LifetimeTracked::Maker{});

        n.start(&Test::moveOnly, &obj, std::make_unique<int>(42));

        n.start(&Test::moveOnlyByConstRef, &obj, std::make_unique<int>(42));
        n.start(&Test::moveOnlyByConstRef, &obj, std::cref(mm));

        co_return join;
    };
}

CORRAL_TEST_CASE("nursery-imm-ready") {
    CORRAL_WITH_NURSERY(n) {
        n.start([] { return noop(); });
        return just(join);
    };
}

CORRAL_TEST_CASE("nursery-ret-policy") {
    auto sleep = [&](milliseconds delay) -> Task<void> {
        co_await t.sleep(delay);
    };

    CATCH_SECTION("join") {
        CORRAL_WITH_NURSERY(n) {
            n.start(sleep, 5ms);
            co_return join;
        };
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("cancel") {
        CORRAL_WITH_NURSERY(n) {
            n.start(sleep, 5ms);
            co_return cancel;
        };
        CATCH_CHECK(t.now() == 0ms);
    }

    CATCH_SECTION("suspend_forever") {
        co_await anyOf(sleep(5ms), [&]() -> Task<void> {
            CORRAL_WITH_NURSERY(n) {
                co_await SuspendForever{};
                co_return join;
            };
        });
        CATCH_CHECK(t.now() == 5ms);
    }
}

CORRAL_TEST_CASE("nursery-cancel-enqueue") {
    bool started = false;
    CORRAL_WITH_NURSERY(nursery) {
        ScopeGuard guard([&] {
            nursery.start([&]() -> Task<> {
                started = true;
                co_await corral::yield;
                CATCH_CHECK(!"should never reach here");
                co_return;
            });
        });
        co_return cancel;
    };
    CATCH_CHECK(started);
}

CORRAL_TEST_CASE("nursery-sync-cancel") {
    bool cancelled = false;
    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> {
            ScopeGuard guard([&] { cancelled = true; });
            co_await t.sleep(5ms);
        });
        co_await t.sleep(1ms);
        CATCH_CHECK(!cancelled);
        n.cancel();
        CATCH_CHECK(!cancelled);
        co_await yield;
        CATCH_CHECK(!"should not reach here");
        co_return cancel;
    };
    CATCH_CHECK(cancelled);
}

CORRAL_TEST_CASE("nursery-multi-cancel") {
    auto task = [&](Nursery& n) -> Task<> {
        co_await t.sleep(1ms, uninterruptible);
        n.cancel();
    };
    CORRAL_WITH_NURSERY(n) {
        n.start(task, std::ref(n));
        n.start(task, std::ref(n));
        n.start(task, std::ref(n));
        co_return join;
    };
}

CORRAL_TEST_CASE("nursery-cancel-from-outside") {
    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> { co_await t.sleep(10ms); });
        t.schedule(1ms, [&] { n.cancel(); });
        co_return join;
    };

    CATCH_CHECK(t.now() == 1ms);
}

#if __cpp_exceptions
CORRAL_TEST_CASE("nursery-multi-exception") {
    auto task = [&](Nursery& n) -> Task<> {
        co_await t.sleep(1ms, uninterruptible);
        throw std::runtime_error("boo!");
    };
    try {
        CORRAL_WITH_NURSERY(n) {
            n.start(task, std::ref(n));
            n.start(task, std::ref(n));
            n.start(task, std::ref(n));
            co_return join;
        };
    } catch (std::runtime_error&) {}
}

CORRAL_TEST_CASE("nursery-cancel-exception") {
    CATCH_CHECK_THROWS_WITH(
            CORRAL_WITH_NURSERY(n) {
                n.start([&]() -> Task<> {
                    co_await t.sleep(2ms, uninterruptible);
                    throw std::runtime_error("boo!");
                });
                n.start([&]() -> Task<> {
                    co_await t.sleep(3ms, uninterruptible);
                });
                co_await t.sleep(1ms);
                co_return cancel;
            },
            Catch::Equals("boo!"));
    CATCH_CHECK(t.now() == 3ms);
}

CORRAL_TEST_CASE("nursery-exception") {
    auto t1 = [&]() -> Task<> { co_await t.sleep(2ms); };
    auto t2 = [&]() -> Task<> {
        co_await std::suspend_never();
        throw std::runtime_error("boo!");
    };

    CATCH_CHECK_THROWS_WITH(
            CORRAL_WITH_NURSERY(n) {
                n.start(t1);
                n.start(t2);
                co_return join;
            },
            Catch::Equals("boo!"));
    CATCH_CHECK(t.now() == 0ms);
}

CORRAL_TEST_CASE("nursery-imm-fail-sibling") {
    StageChecker stage;
    try {
        CORRAL_WITH_NURSERY(n) {
            // Note: nursery block itself is not a coroutine

            n.start([&]() -> Task<void> {
                stage.require(1);
                auto _ = stage.requireOnExit(2);
                co_await SuspendForever{};
            });

            n.start([&]() -> Task<void> {
                throw std::runtime_error("boo!");
                return noop();
            });

            stage.require(0);
            return just(join);
        };
    } catch (std::runtime_error& e) {
        stage.require(3);
        CATCH_CHECK(e.what() == std::string_view("boo!"));
    }
}

CORRAL_TEST_CASE("nursery-imm-fail-nursery-block") {
    StageChecker stage;
    try {
        CORRAL_WITH_NURSERY(n) {
            n.start([&]() -> Task<void> {
                stage.require(1);
                auto _ = stage.requireOnExit(2);
                co_await SuspendForever{};
            });

            stage.require(0);
            throw std::runtime_error("boo!");
        };
    } catch (std::runtime_error& e) {
        stage.require(3);
        CATCH_CHECK(e.what() == std::string_view("boo!"));
    }
}
#endif


//
// TaskStarted handling
//

CORRAL_TEST_CASE("nursery-task-started-await") {
    CORRAL_WITH_NURSERY(n) {
        co_await n.start(
                [&](milliseconds delay, TaskStarted<> started) -> Task<> {
                    co_await t.sleep(delay);
                    started();
                    co_await t.sleep(5ms);
                },
                2ms);
        CATCH_CHECK(t.now() == 2ms);
        co_return join;
    };
    CATCH_CHECK(t.now() == 7ms);
}

CORRAL_TEST_CASE("nursery-task-started-no-await") {
    CORRAL_WITH_NURSERY(n) {
        n.start(
                [&](milliseconds delay, TaskStarted<> started) -> Task<> {
                    co_await t.sleep(delay);
                    started();
                },
                2ms);
        co_return join;
    };
    CATCH_CHECK(t.now() == 2ms);
}

CORRAL_TEST_CASE("nursery-task-started-optional-arg-await") {
    CORRAL_WITH_NURSERY(n) {
        co_await n.start(
                [&](milliseconds delay, TaskStarted<> started = {}) -> Task<> {
                    co_await t.sleep(delay);
                    started();
                    co_await t.sleep(5ms);
                },
                2ms);
        CATCH_CHECK(t.now() == 2ms);
        co_return join;
    };
    CATCH_CHECK(t.now() == 7ms);
}

CORRAL_TEST_CASE("nursery-task-started-optional-arg-no-await") {
    CORRAL_WITH_NURSERY(n) {
        n.start(
                [&](milliseconds delay, TaskStarted<> started = {}) -> Task<> {
                    co_await t.sleep(delay);
                    started();
                },
                2ms);
        co_return join;
    };
    CATCH_CHECK(t.now() == 2ms);
}

CORRAL_TEST_CASE("nursery-task-started-combiners") {
    auto task = [&](milliseconds delay, TaskStarted<> started) -> Task<> {
        co_await t.sleep(delay);
        started();
        co_await t.sleep(delay);
    };
    CORRAL_WITH_NURSERY(n) {
        co_await allOf(n.start(task, 2ms), n.start(task, 3ms));
        CATCH_CHECK(t.now() == 3ms);
        co_return join;
    };
    CATCH_CHECK(t.now() == 6ms);
}

CORRAL_TEST_CASE("nursery-task-started-retval") {
    CORRAL_WITH_NURSERY(n) {
        int ret = co_await n.start([](TaskStarted<int> started) -> Task<> {
            co_await yield; // make this a coroutine
            started(42);
        });
        CATCH_CHECK(ret == 42);
        co_return join;
    };
}

CORRAL_TEST_CASE("nursery-task-started-template-retval") {
    CORRAL_WITH_NURSERY(n) {
        int ret = co_await n.start<int>(
                [](auto arg, TaskStarted<int> started) -> Task<> {
                    co_await yield; // make this a coroutine
                    started(arg);
                },
                42);
        CATCH_CHECK(ret == 42);
        co_return join;
    };
}

#if __cpp_exceptions
CORRAL_TEST_CASE("nursery-task-started-exception") {
    CORRAL_WITH_NURSERY(n) {
        try {
            co_await n.start([](TaskStarted<> started) -> Task<> {
                co_await yield; // make this a coroutine
                throw std::runtime_error("boo!");
            });
            CATCH_CHECK(!"should never reach here");
        } catch (const std::runtime_error& e) {
            CATCH_CHECK(e.what() == std::string_view("boo!"));
        }
        co_return join;
    };
}
#endif

CORRAL_TEST_CASE("nursery-task-started-cancel") {
    CORRAL_WITH_NURSERY(n) {
        auto [done, timedOut] =
                co_await anyOf(n.start([&](TaskStarted<> started) -> Task<> {
                    co_await t.sleep(5ms);
                    CATCH_CHECK(!"should never reach here");
                }),
                               t.sleep(2ms));
        CATCH_CHECK(!done);
        CATCH_CHECK(t.now() == 2ms);
        co_return join;
    };
}

CORRAL_TEST_CASE("nursery-task-started-cancel-reject") {
    CORRAL_WITH_NURSERY(n) {
        auto [done, timedOut] =
                co_await anyOf(n.start([&](TaskStarted<> started) -> Task<> {
                    co_await corral::noncancellable(t.sleep(5ms));
                    started();
                }),
                               t.sleep(2ms));
        CATCH_CHECK(!done);
        CATCH_CHECK(t.now() == 5ms);
        co_return join;
    };
}

CORRAL_TEST_CASE("start-cancel_nursery-confirm") {
    Nursery* inner = nullptr;
    Event cancelInner;
    CORRAL_WITH_NURSERY(outer) {
        co_await outer.start([&](TaskStarted<> started) -> Task<> {
            co_await anyOf(openNursery(std::ref(inner), std::move(started)),
                           cancelInner);
        });

        CATCH_REQUIRE(inner);

        outer.start([&]() -> Task<> {
            co_await inner->start([&](TaskStarted<> started) -> Task<> {
                co_await t.sleep(5ms);
                started();
                co_await t.sleep(1ms);
            });
        });

        co_await t.sleep(1ms);
        cancelInner.trigger();
        co_await t.sleep(1ms);
        CATCH_CHECK(inner);
        co_await t.sleep(5ms);
        CATCH_CHECK(!inner);

        co_return join;
    };
}

CORRAL_TEST_CASE("nursery-task-started-imm-ready") {
    CORRAL_WITH_NURSERY(n) {
        co_await n.start([](TaskStarted<> started) -> Task<> {
            started();
            return noop();
        });
        co_return join;
    };
}

CORRAL_TEST_CASE("nursery-task-started-cancel-before-handoff") {
    // If TaskStarted<> is invoked in a cancelled context, handing
    // off a coroutine pending cancellation to a cancelled nursery,
    // this should not result in double cancellation.
    co_await anyOf(t.sleep(1ms), [&]() -> Task<> {
        CORRAL_WITH_NURSERY(n) {
            co_await n.start([&](TaskStarted<> started) -> Task<> {
                co_await t.sleep(5ms, uninterruptible);
                started();

                co_await t.sleep(1ms, uninterruptible);

                // The cancellation should happen, though
                co_await yield;
                CATCH_CHECK(!"should never reach here");
            });

            // The task in the middle of its cancellation should not
            // be reparented, and the above `n.start()` should not
            // complete early.
            CATCH_CHECK(!"should never reach here");

            co_return join;
        };
    });
}

CORRAL_TEST_CASE("nursery-task-started-cancel-befor-handoff-2") {
    // Cancelling `n.start()` before handoff and *then* cancelling
    // the nursery should not result in double cancellation either.
    CORRAL_WITH_NURSERY(n) {
        co_await anyOf(t.sleep(1ms),
                       n.start([&](TaskStarted<> started) -> Task<> {
                           co_await t.sleep(2ms, uninterruptible);
                           started();
                           co_await t.sleep(2ms, uninterruptible);
                       }));
        co_return cancel;
    };
}

CORRAL_TEST_CASE("nursery-task-started-early-cancel") {
    // `co_await n.start()` in cancelled context should still start
    // the child coroutine, pending cancellation.
    bool started = false;
    co_await anyOf(t.sleep(1ms), [&]() -> Task<> {
        CORRAL_WITH_NURSERY(n) {
            co_await t.sleep(2ms, uninterruptible); // now we have pending
                                                    // cancellation
            co_await n.start([&](TaskStarted<>) -> Task<> {
                started = true;
                co_await yield;
                CATCH_CHECK(!"should never reach here");
            });

            CATCH_CHECK(!"should never reach here"); // see above
            co_return join;
        };
    });
    CATCH_CHECK(started);
}


//
// openNursery() tests
//

CORRAL_TEST_CASE("open-nursery") {
    Nursery* inner = nullptr;
    CORRAL_WITH_NURSERY(outer) {
        co_await outer.start(openNursery, std::ref(inner));
        inner->start([&]() -> Task<> { co_return; });
        co_return cancel;
    };
}

CORRAL_TEST_CASE("open-nursery-cancel") {
    Nursery* n = nullptr;
    CORRAL_WITH_NURSERY(n2) {
        co_await n2.start(openNursery, std::ref(n));
        n->start([&]() -> Task<> {
            co_await t.sleep(1ms, uninterruptible);
            n->start([&]() -> Task<> { co_return; });
        });
        co_return cancel;
    };
}


//
// UnsafeNursery
//

CORRAL_TEST_CASE("unsafe-nursery-async-close") {
    auto n = std::make_unique<UnsafeNursery>(t);

    CATCH_SECTION("outside") {
        n->start([&]() -> Task<> { co_await t.sleep(5ms, uninterruptible); });
        co_await t.sleep(1ms);

        Event e;
        n->asyncClose([&]() noexcept {
            n.reset();
            e.trigger();
        });
        co_await e;
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("inside") {
        n->start([&]() -> Task<> {
            co_await t.sleep(1ms);
            n->asyncClose([&]() noexcept { n.reset(); });
        });
        co_await t.sleep(2ms);
    }

    CATCH_CHECK(!n);
}

CORRAL_TEST_CASE("unsafe-nursery-sync-close") {
    auto n = std::make_unique<UnsafeNursery>(t);
    Event evt;
    bool done = false;
    n->start([&]() -> Task<> {
        co_await evt;
        done = true;
    });
    co_await t.sleep(1ms);
    CATCH_CHECK(!n->empty());
    CATCH_CHECK(!done);
    evt.trigger();
    CATCH_CHECK(!n->empty());
    CATCH_CHECK(!done);
    n.reset();
    CATCH_CHECK(done);
}

} // namespace
