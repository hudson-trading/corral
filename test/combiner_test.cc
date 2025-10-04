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

#include "../corral/corral.h"
#include "TestEventLoop.h"
#include "helpers.h"

using namespace corral;
using namespace corral::testing;
using namespace std::chrono_literals;
using corral::detail::ScopeGuard;

#define CORRAL_TEST_CASE(...)                                                  \
    CORRAL_TEST_CASE_IMPL(TestEventLoop, t, __VA_ARGS__)

namespace {

CORRAL_TEST_CASE("anyof") {
    CATCH_SECTION("smoke") {
        auto [a, b, c] =
                co_await anyOf(t.sleep(2ms), t.sleep(3ms),
                               [&]() -> Task<> { co_await t.sleep(5ms); });
        CATCH_CHECK(a);
        CATCH_CHECK(!b);
        CATCH_CHECK(!c);
        CATCH_CHECK(t.now() == 2ms);
    }

    CATCH_SECTION("immediate-front") {
        co_await anyOf([&]() -> Task<> { co_return; },
                       [&]() -> Task<> { co_await t.sleep(1ms); });
        CATCH_CHECK(t.now() == 0ms);

        auto [a, b] = co_await anyOf(ReadyCancellable{}, t.sleep(1ms));
        CATCH_CHECK(t.now() == 0ms);
        CATCH_CHECK(a);
        CATCH_CHECK(!b);
    }

    CATCH_SECTION("immediate-back") {
        co_await anyOf([&]() -> Task<> { co_await t.sleep(1ms); },
                       [&]() -> Task<> { co_return; });
        CATCH_CHECK(t.now() == 0ms);

        auto [a, b] = co_await anyOf(t.sleep(1ms), ReadyCancellable{});
        CATCH_CHECK(t.now() == 0ms);
        CATCH_CHECK(!a);
        CATCH_CHECK(b);
    }

    CATCH_SECTION("immediate-both") {
        auto [a, b] = co_await anyOf(Ready{}, Ready{});
        CATCH_CHECK(t.now() == 0ms);
        CATCH_CHECK(a);
        CATCH_CHECK(b);

        std::tie(a, b) = co_await anyOf(ReadyCancellable{}, ReadyCancellable{});
        CATCH_CHECK(t.now() == 0ms);
        CATCH_CHECK(a);
        CATCH_CHECK(!b);
    }

    CATCH_SECTION("empty") {
        auto r = co_await anyOf();
        static_assert(std::tuple_size_v<decltype(r)> == 0);
    }

    CATCH_SECTION("non-cancellable") {
        auto [a, b, c] = co_await anyOf(
                t.sleep(2ms), t.sleep(3ms, uninterruptible), t.sleep(5ms));
        CATCH_CHECK(t.now() == 3ms);
        CATCH_CHECK(a);
        CATCH_CHECK(b);
        CATCH_CHECK(!c);
    }

    CATCH_SECTION("return-lref") {
        int x = 42;
        auto [lx, s1] = co_await anyOf([&]() -> Task<int&> { co_return x; },
                                       t.sleep(2ms));
        CATCH_CHECK(&*lx == &x);
        CATCH_CHECK(!s1);
    }

    CATCH_SECTION("return-rref") {
        int x = 42;
        auto [rx, s2] = co_await anyOf(
                [&]() -> Task<int&&> { co_return std::move(x); }, t.sleep(2ms));
        CATCH_CHECK(&*rx == &x);
        CATCH_CHECK(!s2);
    }

    CATCH_SECTION("immediate-lambda") {
        co_await anyOf(Ready{}, [&]() -> Task<> { co_await t.sleep(1ms); });
        CATCH_CHECK(t.now() == 0ms);
    }

    CATCH_SECTION("take-lref") {
        LValueQualifiedImm a;
        LValueQualified b;
        auto [ra, rb] = co_await allOf(a, b);
        CATCH_CHECK(ra == 42);
        CATCH_CHECK(rb == 42);
    }
}

CORRAL_TEST_CASE("mostof") {
    CATCH_SECTION("smoke") {
        co_await mostOf(t.sleep(2ms), t.sleep(3ms),
                        [&]() -> Task<> { co_await t.sleep(5ms); });
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("empty") {
        auto r = co_await mostOf();
        static_assert(std::tuple_size_v<decltype(r)> == 0);
    }

    CATCH_SECTION("retval") {
        auto [a, b] = co_await mostOf([]() -> Task<int> { co_return 42; },
                                      []() -> Task<int> { co_return 43; });
        CATCH_CHECK(*a == 42);
        CATCH_CHECK(*b == 43);
    }

    CATCH_SECTION("noncancellable") {
        bool resumed = false;
        auto sub = [&]() -> Task<> {
            auto [a, b, c] = co_await mostOf(
                    [&]() -> Task<int> { co_return 42; },
                    t.sleep(3ms, uninterruptible), t.sleep(5ms));
            CATCH_CHECK(a);
            CATCH_CHECK(*a == 42);
            CATCH_CHECK(b);
            CATCH_CHECK(!c);
            resumed = true;
        };
        co_await anyOf(sub(), t.sleep(1ms));
        CATCH_CHECK(t.now() == 3ms);
        CATCH_CHECK(resumed);
    }

#if __cpp_exceptions
    CATCH_SECTION("throws") {
        bool cancelled = false;
        CATCH_CHECK_THROWS(co_await mostOf(
                [&]() -> Task<> {
                    ScopeGuard guard([&] { cancelled = true; });
                    co_await SuspendForever{};
                },
                [&]() -> Task<> {
                    co_await t.sleep(1ms);
                    throw std::runtime_error("boo!");
                }));
        CATCH_CHECK(cancelled);
    }
#endif
}

CORRAL_TEST_CASE("allof") {
    CATCH_SECTION("smoke") {
        co_await allOf(t.sleep(2ms), t.sleep(3ms),
                       [&]() -> Task<> { co_await t.sleep(5ms); });
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("immediate-front") {
        co_await allOf(Ready{}, [&t]() -> Task<> { co_await t.sleep(1ms); });
        CATCH_CHECK(t.now() == 1ms);
    }

    CATCH_SECTION("immediate-back") {
        co_await allOf([&t]() -> Task<> { co_await t.sleep(1ms); }, Ready{});
        CATCH_CHECK(t.now() == 1ms);
    }

    CATCH_SECTION("empty") {
        auto r = co_await allOf();
        static_assert(std::tuple_size_v<decltype(r)> == 0);
    }

    CATCH_SECTION("retval") {
        auto [a, b] = co_await allOf([]() -> Task<int> { co_return 42; },
                                     []() -> Task<int> { co_return 43; });
        CATCH_CHECK(a == 42);
        CATCH_CHECK(b == 43);
    }

#if __cpp_exceptions
    CATCH_SECTION("throws") {
        bool cancelled = false;
        CATCH_CHECK_THROWS(co_await allOf(
                [&]() -> Task<> {
                    ScopeGuard guard([&] { cancelled = true; });
                    co_await SuspendForever{};
                },
                [&]() -> Task<> {
                    co_await t.sleep(1ms);
                    throw std::runtime_error("boo!");
                }));
        CATCH_CHECK(cancelled);
    }
#endif
}

CORRAL_TEST_CASE("anyof-allof-mix") {
    auto five = [&]() -> Task<> { co_await t.sleep(5ms); };
    co_await allOf(
            t.sleep(2ms), [&]() -> Task<> { co_await t.sleep(3ms); },
            anyOf(five, t.sleep(6ms)));
    CATCH_CHECK(t.now() == 5ms);
}

CORRAL_TEST_CASE("mux-range") {
    auto task = [&t](int x) -> Task<int> {
        co_await t.sleep(std::chrono::milliseconds(x));
        co_return x * 100;
    };

    std::vector<Task<int>> v;
    v.push_back(task(3));
    v.push_back(task(2));
    v.push_back(task(5));

    CATCH_SECTION("anyof") {
        auto ret = co_await anyOf(v);
        CATCH_CHECK(t.now() == 2ms);
        CATCH_CHECK(ret.size() == 3);
        CATCH_CHECK(!ret[0]);
        CATCH_CHECK(*ret[1] == 200);
        CATCH_CHECK(!ret[2]);
    }

    CATCH_SECTION("allof") {
        auto ret = co_await allOf(v);
        CATCH_CHECK(t.now() == 5ms);
        CATCH_CHECK(ret[0] == 300);
        CATCH_CHECK(ret[1] == 200);
        CATCH_CHECK(ret[2] == 500);
    }

    CATCH_SECTION("anyof-immediate") {
        auto imm = []() -> Task<int> { co_return 0; };
        v.push_back(imm());
        co_await anyOf(v);
    }

    CATCH_SECTION("anyof-empty") {
        v.clear();
        co_await anyOf(v);
    }

    CATCH_SECTION("allof-empty") {
        v.clear();
        co_await allOf(v);
    }

#if !defined(__clang__) || __clang_major__ >= 16
    // clang-15 does not compile this
    CATCH_SECTION("moveable-range-1") {
        co_await allOf(v |
                       std::views::transform([&](Task<int>& x) -> Task<int>&& {
                           return std::move(x);
                       }));
    }

    CATCH_SECTION("moveable-range-2") {
        co_await allOf(v |
                       std::views::transform([&](Task<int>& x) -> Task<int> {
                           return std::move(x);
                       }));
    }

    CATCH_SECTION("rvalue-range") {
        // Same as above, but construct awaitables on the fly
        std::vector<int> ints{3, 2, 5};
        co_await allOf(ints |
                       std::views::transform([&](int x) { return task(x); }));
    }
#endif
}

CORRAL_TEST_CASE("mux-cancel") {
    auto task = [&]() -> Task<> { co_await t.sleep(5ms); };
    std::vector<Task<>> tv;
    tv.push_back(task());
    tv.push_back(task());

    CATCH_SECTION("any-of") {
        co_await anyOf([&]() -> Task<> { co_await anyOf(task(), task()); },
                       t.sleep(1ms));
    }
    CATCH_SECTION("all-of") {
        co_await anyOf([&]() -> Task<> { co_await allOf(task(), task()); },
                       t.sleep(1ms));
    }
    CATCH_SECTION("any-of-range") {
        co_await anyOf([&]() -> Task<> { co_await anyOf(tv); }, t.sleep(1ms));
    }
    CATCH_SECTION("all-of-range") {
        co_await anyOf([&]() -> Task<> { co_await allOf(tv); }, t.sleep(1ms));
    }
}

CORRAL_TEST_CASE("mux-cancel-done") {
    auto noop = []() -> Task<> { co_return; };
    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> {
            co_await allOf(SuspendForever{}, SuspendForever{}, noop());
        });
        co_await t.sleep(1ms);
        co_return cancel;
    };
}

} // namespace
