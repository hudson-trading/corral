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

#ifdef _MSC_VER
#define _USE_MATH_DEFINES 1
#endif

#include <cmath>
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
// No-op awaitables
//

CORRAL_TEST_CASE("noop") {
    auto mkNoop = []() -> Task<> { return noop(); };
    co_await noop();
    co_await mkNoop();
    co_await mkNoop;
    co_await anyOf(mkNoop(), mkNoop());
    CORRAL_WITH_NURSERY(n) {
        n.start(mkNoop);
        co_return join;
    };
}

CORRAL_TEST_CASE("just-imm") {
    auto x = co_await just(42);
    CATCH_CHECK(x == 42);

    auto [y, z] = co_await allOf(just(1), just(2));
    CATCH_CHECK(y == 1);
    CATCH_CHECK(z == 2);

    int i;
    int& ri = co_await just<int&>(i);
    CATCH_CHECK(&ri == &i);

    auto p = std::make_unique<int>(42);
    auto q = co_await just(std::move(p));
    CATCH_CHECK(p == nullptr);
    CATCH_CHECK(*q == 42);

    auto& rq = co_await just<std::unique_ptr<int>&>(q);
    CATCH_CHECK(*q == 42);
    CATCH_CHECK(*rq == 42);
}


template <class T>
constexpr const bool FitsInline = corral::detail::ValueOrPromise<T>::FitsInline;

template <class T> Task<T> mkAsync(T v) {
    return just<T>(std::forward<T>(v));
}

CORRAL_TEST_CASE("just-async") {
    // Same as `just-imm`, but all just() are casted to Task<T>.
    static_assert(FitsInline<int>);
    static_assert(FitsInline<int*>);
    static_assert(FitsInline<uintptr_t>);
    static_assert(FitsInline<std::monostate>);
    static_assert(FitsInline<void>);

    static_assert(!FitsInline<std::vector<int>>);
    static_assert(!FitsInline<std::unique_ptr<int>>);
    static_assert(FitsInline<const std::vector<int>&>);

    auto x = co_await mkAsync(42);
    CATCH_CHECK(x == 42);

    auto [y, z] = co_await allOf(mkAsync(1), mkAsync(2));
    CATCH_CHECK(y == 1);
    CATCH_CHECK(z == 2);

    int i;
    int& ri = co_await mkAsync<int&>(i);
    CATCH_CHECK(&ri == &i);
    int&& rri = co_await mkAsync<int&&>(std::move(i));
    CATCH_CHECK(&rri == &i);

    auto p = std::make_unique<int>(42);
    auto q = co_await mkAsync(std::move(p));
    CATCH_CHECK(p == nullptr);
    CATCH_CHECK(*q == 42);

    auto& rq = co_await mkAsync<std::unique_ptr<int>&>(q);
    CATCH_CHECK(*q == 42);
    CATCH_CHECK(*rq == 42);

    float f = co_await Task<float>(justApx<float>(M_PI));
    CATCH_CHECK(fabs(f - float(M_PI)) < 8e-7);

    double df = co_await Task<double>(justApx<double>(M_PI));
    CATCH_CHECK(fabs(df - M_PI) < 1e-15);
}

//
// noncancellable() & disposable()
//

CORRAL_TEST_CASE("noncancellable-task-return") {
    auto t1 = [&]() -> Task<> { co_await t.sleep(1ms); };
    auto t2 = [&] { return corral::noncancellable(t1()); };
    co_await t2();
}

CORRAL_TEST_CASE("disposable") {
    auto [_, done] = co_await anyOf(t.sleep(3ms), [&]() -> Task<> {
        co_await disposable(t.sleep(5ms, uninterruptible));
        CATCH_CHECK(!"should not reach here");
    });
    CATCH_CHECK(t.now() == 5ms);
    CATCH_CHECK(!done);
}

CORRAL_TEST_CASE("complex-noncancellable-disposable-structure") {
    StageChecker stage;

    Event evt;
    auto [_, done] = co_await anyOf(t.sleep(3ms), [&]() -> Task<> {
        auto g2 = stage.requireOnExit(2);
        co_await disposable(
                anyOf(corral::noncancellable(
                              anyOf(evt,
                                    [&]() -> Task<> {
                                        auto g1 = stage.requireOnExit(1);
                                        co_await t.sleep(5ms);
                                        CATCH_CHECK(!"should not reach here");
                                    })),

                      untilCancelledAnd([&]() -> Task<> {
                          stage.require(0);
                          evt.trigger();
                          co_return;
                      })));
        CATCH_CHECK(!"should not reach here");
    });

    stage.require(3);
    CATCH_CHECK(!done);
    CATCH_CHECK(t.now() == 3ms);
}


//
// Shared
//

CORRAL_TEST_CASE("shared") {
    auto shared = Shared([&]() -> Task<int> {
        co_await t.sleep(5ms);
        co_return 42;
    });

    auto use = [&](milliseconds delay = 0ms) -> Task<int> {
        if (delay != 0ms) {
            co_await t.sleep(delay);
        }
        int ret = co_await shared;
        CATCH_CHECK(t.now() == 5ms);
        CATCH_CHECK(ret == 42);
        co_return std::move(ret);
    };

    CATCH_SECTION("smoke") {
        auto [x, y] = co_await allOf(use(), use(1ms));
        CATCH_CHECK(x == 42);
        CATCH_CHECK(y == 42);
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("cancellation") {
        co_await anyOf(allOf(use(), use(1ms)), t.sleep(3ms));
        CATCH_CHECK(t.now() == 3ms);
    }
}

CORRAL_TEST_CASE("shared-no-cancel") {
    auto shared = Shared([&]() -> Task<> {
        ScopeGuard check([&] { CATCH_CHECK(t.now() == 5ms); });
        co_await t.sleep(10ms);
    });

    auto first = [&]() -> Task<> {
        co_await anyOf(shared, t.sleep(2ms));
        CATCH_CHECK(t.now() == 2ms);
    };

    auto second = [&]() -> Task<> {
        co_await t.sleep(1ms);
        co_await anyOf(shared, t.sleep(4ms));
        CATCH_CHECK(t.now() == 5ms);
    };

    co_await allOf(first, second);
}

CORRAL_TEST_CASE("shared-cancel-wait") {
    CATCH_SECTION("int") {
        auto shared = Shared([&]() -> Task<int> {
            co_await t.sleep(5ms, uninterruptible);
            co_await t.sleep(5ms);
            co_return 42;
        });

        co_await allOf(anyOf(shared, t.sleep(3ms)), [&]() -> Task<> {
            co_await t.sleep(4ms);
            auto res = co_await shared.asOptional();
            CATCH_CHECK(!res);
        });
    }

    CATCH_SECTION("void") {
        auto shared = Shared([&]() -> Task<> {
            co_await t.sleep(5ms, uninterruptible);
            co_await t.sleep(5ms);
        });

        co_await allOf(anyOf(shared, t.sleep(3ms)), [&]() -> Task<> {
            co_await t.sleep(4ms);
            auto res = co_await shared.asOptional();
            CATCH_CHECK(!res);
        });
    }
}

CORRAL_TEST_CASE("shared-cancel-self") {
    auto shared = Shared([&]() -> Task<> { co_await t.sleep(5ms); });
    auto parent = [&]() -> Task<void> { co_await shared; };
    co_await anyOf(parent(), parent());
}


//
// Try/catch blocks
//

#if __cpp_exceptions
CORRAL_TEST_CASE("try-smoke") {
    StageChecker stage;

    auto g3 = stage.requireOnExit(3);
    CORRAL_TRY {
        stage.require(0);
        auto g1 = stage.requireOnExit(1);
        co_await yield;
    }
    CORRAL_CATCH(std::exception & e) {
        CATCH_CHECK(!"should not reach here");
        co_await yield;
    }
    CORRAL_FINALLY {
        stage.require(2);
        co_await yield;
    };
}

CORRAL_TEST_CASE("try-no-macros") {
    StageChecker stage;
    auto g3 = stage.requireOnExit(3);
    // clang-format off
        co_await try_([&]() -> Task<> {
            stage.require(0);
            auto g1 = stage.requireOnExit(1);
            co_await yield;
        }).catch_([&](std::exception& e) -> Task<> {
            CATCH_CHECK(!"should not reach here");
            co_await yield;
        }).finally([&]() -> Task<> {
            stage.require(2);
            co_await yield;
        });
    // clang-format on
}

CORRAL_TEST_CASE("try-caught-exception") {
    StageChecker stage;
    auto g4 = stage.requireOnExit(4);
    CORRAL_TRY {
        stage.require(0);
        auto g1 = stage.requireOnExit(1);
        co_await t.sleep(1ms);
        throw std::runtime_error("test");
    }
    CORRAL_CATCH(std::exception & e) {
        stage.require(2);
        CATCH_CHECK(std::string(e.what()) == "test");
        co_await yield;
    }
    CORRAL_FINALLY {
        stage.require(3);
        co_await yield;
    };
}

CORRAL_TEST_CASE("try-multiple-catch-blocks") {
    StageChecker stage;
    auto g3 = stage.requireOnExit(3);
    CORRAL_TRY {
        stage.require(0);
        co_await t.sleep(1ms);
        throw std::runtime_error("test");
    }
    CORRAL_CATCH(std::logic_error & e) {
        CATCH_CHECK(!"should never reach here");
        co_await yield;
    }
    CORRAL_CATCH(std::runtime_error & e) {
        stage.require(1);
        CATCH_CHECK(std::string(e.what()) == "test");
        co_await yield;
    }
    CORRAL_CATCH(std::exception & e) {
        CATCH_CHECK(!"should never reach here");
        co_await yield;
    }
    CORRAL_FINALLY {
        stage.require(2);
        co_await yield;
    };
}

CORRAL_TEST_CASE("try-catch-all") {
    StageChecker stage;
    auto g2 = stage.requireOnExit(2);
    CORRAL_TRY {
        stage.require(0);
        co_await t.sleep(1ms);
        throw 42;
    }
    CORRAL_CATCH(std::exception & e) {
        CATCH_CHECK(!"should never reach here");
        co_await yield;
    }
    CORRAL_CATCH(Ellipsis) {
        stage.require(1);
        co_await yield;
    };
}

CORRAL_TEST_CASE("try-rethrow") {
    StageChecker stage;
    auto g4 = stage.requireOnExit(4);
    CORRAL_TRY {
        stage.require(0);
        co_await yield;
        throw std::runtime_error("test");
    }
    CORRAL_CATCH(Ellipsis) {
        try {
            stage.require(1);
            co_await rethrow;
        } catch (std::runtime_error& e) {
            stage.require(2);
            CATCH_CHECK(std::string(e.what()) == "test");
        }
        stage.require(3);
    };
}

CORRAL_TEST_CASE("try-rethrow-indirect-nursery") {
    StageChecker stage;
    auto g3 = stage.requireOnExit(3);
    try {
        CORRAL_TRY {
            stage.require(0);
            co_await yield;
            throw std::runtime_error("test");
        }
        CORRAL_CATCH(Ellipsis) {
            CORRAL_WITH_NURSERY(n) {
                n.start([&]() -> Task<> {
                    stage.require(1);
                    co_await rethrow;
                });
                co_return join;
            };
        };
    } catch (std::runtime_error& e) {
        stage.require(2);
        CATCH_CHECK(std::string(e.what()) == "test");
    }
}

CORRAL_TEST_CASE("try-rethrow-nested") {
    StageChecker stage;
    auto g4 = stage.requireOnExit(4);
    CORRAL_TRY {
        stage.require(0);
        co_await yield;
        throw std::runtime_error("test");
    }
    CORRAL_CATCH(Ellipsis) {
        stage.require(1);
        CORRAL_TRY {
            stage.require(2);
            co_await rethrow;
        }
        CORRAL_CATCH(std::runtime_error & e) {
            stage.require(3);
            CATCH_CHECK(std::string(e.what()) == "test");
            co_await yield;
        };
    };
}

CORRAL_TEST_CASE("try-no-finally-block") {
    StageChecker stage;
    auto g2 = stage.requireOnExit(2);
    CORRAL_TRY {
        stage.require(0);
        co_await t.sleep(1ms);
        throw std::runtime_error("test");
    }
    CORRAL_CATCH(std::exception & e) {
        stage.require(1);
        CATCH_CHECK(std::string(e.what()) == "test");
        co_await yield;
    };
}

CORRAL_TEST_CASE("try-no-finally-no-macros") {
    StageChecker stage;
    auto g2 = stage.requireOnExit(2);
    // clang-format off
        co_await try_([&]() -> Task<> {
            stage.require(0);
            co_await t.sleep(1ms);
            throw std::runtime_error("test");
        }).catch_([&](std::exception& e) -> Task<> {
            stage.require(1);
            CATCH_CHECK(std::string(e.what()) == "test");
            co_await yield;
        });
    // clang-format on
}

CORRAL_TEST_CASE("try-uncaught-exception") {
    StageChecker stage;
    auto g4 = stage.requireOnExit(4);
    try {
        CORRAL_TRY {
            stage.require(0);
            auto g1 = stage.requireOnExit(1);
            co_await t.sleep(1ms);
            throw std::runtime_error("test");
        }
        CORRAL_FINALLY {
            stage.require(2);
            co_await yield;
        };
    } catch (std::exception&) { stage.require(3); }
}

CORRAL_TEST_CASE("try-reraised") {
    StageChecker stage;
    auto g4 = stage.requireOnExit(4);
    std::exception* ex = nullptr;
    try {
        CORRAL_TRY {
            stage.require(0);
            co_await t.sleep(1ms);
            throw std::runtime_error("test");
        }
        CORRAL_CATCH(std::exception & e) {
            stage.require(1);
            ex = &e;
            co_await rethrow;
        }
        CORRAL_FINALLY {
            stage.require(2);
            co_await yield;
        };
    } catch (std::runtime_error& e) {
        stage.require(3);
        CATCH_CHECK(ex == &e);
        CATCH_CHECK(std::string(e.what()) == "test");
    }
}

CORRAL_TEST_CASE("try-raised-different") {
    StageChecker stage;
    auto g4 = stage.requireOnExit(4);
    try {
        CORRAL_TRY {
            stage.require(0);
            co_await t.sleep(1ms);
            throw std::runtime_error("test1");
        }
        CORRAL_CATCH(std::exception & e) {
            stage.require(1);
            co_await t.sleep(1ms);
            throw std::logic_error("test2");
        }
        CORRAL_FINALLY {
            stage.require(2);
            co_await yield;
        };
    } catch (std::logic_error& e) {
        stage.require(3);
        CATCH_CHECK(std::string(e.what()) == "test2");
    }
}

CORRAL_TEST_CASE("try-sync-exception") {
    StageChecker stage;
    // Make sure finally block is executed even if try-block
    // is not a coroutine
    auto g4 = stage.requireOnExit(4);
    try {
        CORRAL_TRY {
            stage.require(0);
            auto g1 = stage.requireOnExit(1);
            throw std::runtime_error("test");
            // no co_return or co_await here
        }
        CORRAL_FINALLY {
            stage.require(2);
            co_await yield;
        };
    } catch (std::exception&) { stage.require(3); }
}

CORRAL_TEST_CASE("try-exc-catch-cancel") {
    // If the coroutine is cancelled in the catch clause,
    // the exception should not be rethrown out of the whole try-catch block.
    StageChecker stage;
    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> {
            co_await t.sleep(3ms);
            stage.require(2);
            n.cancel();
        });

        CATCH_SECTION("without-finally") {
            auto g4 = stage.requireOnExit(4);
            CORRAL_TRY {
                co_await t.sleep(1ms);
                stage.require(0);
                throw std::runtime_error("test");
            }
            CORRAL_CATCH(const std::exception& e) {
                stage.require(1);
                auto g3 = stage.requireOnExit(3);

                co_await t.sleep(10ms); // cancellation happens here
                CORRAL_ASSERT_UNREACHABLE();
            };
        }

        CATCH_SECTION("with-finally") {
            auto g6 = stage.requireOnExit(6);
            CORRAL_TRY {
                co_await t.sleep(1ms);
                stage.require(0);
                throw std::runtime_error("test");
            }
            CORRAL_CATCH(const std::exception& e) {
                stage.require(1);
                auto g3 = stage.requireOnExit(3);

                co_await t.sleep(10ms); // cancellation happens here
                CORRAL_ASSERT_UNREACHABLE();
            }
            CORRAL_FINALLY {
                stage.require(4);
                co_await t.sleep(3ms);
                stage.require(5);
            };
        }

        CORRAL_ASSERT_UNREACHABLE();
    };
}

CORRAL_TEST_CASE("try-exc-finally-cancel") {
    // If the coroutine is cancelled in the finally-clause with an unhandled
    // exception, the finally-clause should run to completion,
    // and exception should be rethrown out of the try-block.
    StageChecker stage;
    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> {
            co_await t.sleep(3ms);
            stage.require(2);
            n.cancel();
        });

        try {
            CATCH_SECTION("without-catch") {
                auto g4 = stage.requireOnExit(4);
                CORRAL_TRY {
                    co_await t.sleep(1ms);
                    stage.require(0);
                    throw std::runtime_error("test");
                }
                CORRAL_FINALLY {
                    stage.require(1);
                    co_await t.sleep(10ms); // cancellation happens here
                    stage.require(3);
                };
                CATCH_CHECK(!"should not reach here");
            }

            CATCH_SECTION("with-throwing-catch") {
                auto g4 = stage.requireOnExit(4);
                CORRAL_TRY {
                    co_await t.sleep(1ms);
                    stage.require(0);
                    throw std::runtime_error("test2");
                }
                CORRAL_CATCH(const std::exception& e) {
                    co_await t.sleep(1ms);
                    throw std::runtime_error("test");
                }
                CORRAL_FINALLY {
                    stage.require(1);
                    co_await t.sleep(10ms); // cancellation happens here
                    stage.require(3);
                };
                CATCH_CHECK(!"should not reach here");
            }

        } catch (const std::exception& e) {
            stage.require(5);
            CATCH_CHECK(std::string(e.what()) == "test");
        }

        co_return cancel;
    };
}
#endif

CORRAL_TEST_CASE("try-cancel") {
    StageChecker stage;
    auto g5 = stage.requireOnExit(5);
    auto [_, done] = co_await anyOf(t.sleep(2ms), [&]() -> Task<> {
        CORRAL_TRY {
            stage.require(0);
            auto g1 = stage.requireOnExit(2);
            co_await untilCancelledAnd([&]() -> Task<> {
                stage.require(1);
                co_await t.sleep(1ms);
            });
        }
        CORRAL_FINALLY {
            stage.require(3);
            co_await t.sleep(1ms);
        };
    });
    CATCH_CHECK(!done);
    stage.require(4);
    CATCH_CHECK(t.now() == 4ms);
}

CORRAL_TEST_CASE("try-early-cancel") {
    StageChecker stage;
    auto g2 = stage.requireOnExit(2);
    CORRAL_WITH_NURSERY(n) {
        n.cancel();

        CORRAL_TRY {
            stage.require(0);
            co_await t.sleep(1ms);
        }
        CORRAL_FINALLY {
            stage.require(1);
            co_await t.sleep(1ms);
        };
        co_return join;
    };
    CATCH_CHECK(t.now() == 1ms);
}


//
// Sequence
//

CORRAL_TEST_CASE("seq-smoke") {
    co_await (t.sleep(2ms) | then([&] { return t.sleep(3ms); }));
    CATCH_CHECK(t.now() == 5ms);
}

CORRAL_TEST_CASE("seq-pass-value") {
    auto r = co_await (just(42) | then([](int v) { return just(v + 1); }));
    CATCH_CHECK(r == 43);
}

CORRAL_TEST_CASE("seq-pass-reference") {
    int arr[2] = {1, 2};
    auto r = co_await (just<int&>(arr[0]) |
                       then([](int& v) { return just(&v + 1); }));
    CATCH_CHECK(r == &arr[1]);
}

CORRAL_TEST_CASE("seq-pass-void") {
    co_await (noop() | then([] { return noop(); }));
}

CORRAL_TEST_CASE("seq-task") {
    auto r = co_await ([]() -> Task<int> {
        co_return 42;
    } | then([](int v) -> Task<int> { co_return v + 1; }));
    CATCH_CHECK(r == 43);
}

#if __cpp_exceptions
CORRAL_TEST_CASE("seq-exception") {
    CATCH_SECTION("first") {
        CATCH_CHECK_THROWS(co_await ([]() -> Task<> {
            co_await yield;
            throw std::runtime_error("test");
        } | then([] { return just(42); })));
    }

    CATCH_SECTION("second-construction") {
        CATCH_CHECK_THROWS(co_await (just(42) | then([](int) -> Task<void> {
                                         throw std::runtime_error("test");
                                     })));
    }

    CATCH_SECTION("second-execution") {
        CATCH_CHECK_THROWS(co_await (just(42) | then([](int) -> Task<int> {
                                         co_await yield;
                                         throw std::runtime_error("test");
                                     })));
    }
}
#endif

CORRAL_TEST_CASE("seq-cancellation") {
    CATCH_SECTION("first") {
        auto [r, _] = co_await anyOf(t.sleep(3ms) | then([] {
                                         CATCH_CHECK(!"should not reach here");
                                         return noop();
                                     }),
                                     t.sleep(1ms));
        CATCH_CHECK(t.now() == 1ms);
        CATCH_CHECK(!r);
    }

    CATCH_SECTION("second") {
        auto [r, _] = co_await anyOf(
                t.sleep(1ms) | then([&t] { return t.sleep(3ms); }) | then([] {
                    CATCH_CHECK(!"should not reach here");
                    return noop();
                }),
                t.sleep(2ms));
        CATCH_CHECK(t.now() == 2ms);
        CATCH_CHECK(!r);
    }

    CATCH_SECTION("from-within") {
        Event evt;
        co_await anyOf(evt, t.sleep(1ms) | then([&] {
                                evt.trigger();
                                return t.sleep(3ms);
                            }));
    }
}

CORRAL_TEST_CASE("seq-noncancellable") {
    CATCH_SECTION("first") {
        auto [r, _] = co_await anyOf(t.sleep(2ms, uninterruptible) |
                                             then([] { return just(42); }),
                                     t.sleep(1ms));
        CATCH_CHECK(t.now() == 2ms);
        CATCH_CHECK(*r == 42);
    }

    CATCH_SECTION("second") {
        auto [r, _] = co_await anyOf(t.sleep(1ms) | then([&t] {
                                         return t.sleep(3ms, uninterruptible);
                                     }),
                                     t.sleep(2ms));
        CATCH_CHECK(t.now() == 4ms);
        CATCH_CHECK(r);
    }

    CATCH_SECTION("early") {
        // The second awaitable is noncancellable, so should complete,
        // and the lambda should be invoked. However, as the awaitable
        // returned by the lambda is early-cancellable, it should not be
        // suspended on.
        bool started = false;
        auto [r, _] = co_await anyOf(t.sleep(2ms, uninterruptible) | then([&] {
                                         started = true;
                                         return t.sleep(2ms);
                                     }),
                                     t.sleep(1ms));
        CATCH_CHECK(t.now() == 2ms);
        CATCH_CHECK(started);
        CATCH_CHECK(!r);
    }
}

CORRAL_TEST_CASE("seq-lifetime") {
    Semaphore sem{1};
    co_await allOf(sem.lock() | then([&t] { return t.sleep(5ms); }),
                   [&]() -> Task<> {
                       co_await t.sleep(1ms);
                       auto lk = co_await sem.lock();
                       CATCH_CHECK(t.now() == 5ms);
                   });
}

CORRAL_TEST_CASE("seq-chaining") {
    CATCH_SECTION("lassoc") {
        auto r = co_await (just(42) | then([](int v) { return just(v + 1); }) |
                           then([](int v) { return just(v + 1); }));
        CATCH_CHECK(r == 44);
    }

    CATCH_SECTION("rassoc") {
        auto r =
                co_await (just(42) | (then([](int v) { return just(v + 1); }) |
                                      then([](int v) { return just(v + 1); })));
        CATCH_CHECK(r == 44);
    }
}

CORRAL_TEST_CASE("seq-qualifications") {
    LValueQualifiedImm lvqi;
    LValueQualified lvq;
    int r = co_await (noop() | then([&]() -> auto& { return lvqi; }));
    CATCH_CHECK(r == 42);
    r = co_await (noop() | then([] { return RValueQualifiedImm{}; }));
    CATCH_CHECK(r == 42);
    r = co_await (noop() | then([&]() -> auto& { return lvq; }));
    CATCH_CHECK(r == 42);
    r = co_await (noop() | then([] { return RValueQualified{}; }));
    CATCH_CHECK(r == 42);
}


//
// Miscellanea
//

CORRAL_TEST_CASE("run-on-cancel") {
    co_await anyOf(t.sleep(2ms), untilCancelledAnd(t.sleep(1ms)));
    CATCH_CHECK(t.now() == 3ms);
}

CORRAL_TEST_CASE("make-awaitable") {
    auto sleep = [&](std::chrono::milliseconds delay) {
        return makeAwaitable<TestEventLoop::Sleep</*Cancelable = */ true>>(
                std::ref(t), delay);
    };

    co_await anyOf(sleep(10ms), sleep(20ms));
    CATCH_CHECK(t.now() == 10ms);
}

} // namespace
