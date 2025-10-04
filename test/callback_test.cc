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

#include <cstring>
#include <functional>
#include <string>

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

// A makeshift lightweight function adapter which fits in two words.
template <class T> class MyLightweightFn;
template <class... Args> class MyLightweightFn<void(Args...)> {
    void (*fn_)(size_t, Args...);
    size_t obj_;

  public:
    template <class Fn> MyLightweightFn(Fn fn) {
        static_assert(std::is_trivially_copyable_v<Fn>);
        static_assert(std::is_trivially_destructible_v<Fn>);
        static_assert(sizeof(Fn) == sizeof(obj_));
        memcpy(&obj_, &fn, sizeof(Fn));
        fn_ = +[](size_t obj, Args... args) {
            (*reinterpret_cast<Fn*>(&obj))(args...);
        };
    }

    void operator()(Args... args) { return fn_(obj_, args...); }
};

//
// Tests
//

CORRAL_TEST_CASE("cb") {
    auto schedule = [&](std::function<void(int, int)> cb) {
        t.schedule(1ms, [cb] { cb(1, 2); });
        t.schedule(2ms, [cb] { cb(3, 4); });
        t.schedule(3ms, [cb] { cb(5, 6); });
    };

    CBPortal<int, int> aw;
    int msgs = 0;
    auto consumer = [&]() -> Task<> {
        auto [x, y] = co_await untilCBCalled(schedule, aw);
        ++msgs;
        CATCH_CHECK(x == 1);
        CATCH_CHECK(y == 2);

        std::tie(x, y) = co_await aw;
        ++msgs;
        CATCH_CHECK(x == 3);
        CATCH_CHECK(y == 4);

        std::tie(x, y) = co_await aw;
        ++msgs;
        CATCH_CHECK(x == 5);
        CATCH_CHECK(y == 6);

        std::tie(x, y) = co_await aw;
        CATCH_CHECK(false);
    };

    co_await anyOf(consumer, t.sleep(5ms));
    CATCH_CHECK(msgs == 3);
}

CORRAL_TEST_CASE("cb-simplified") {
    CATCH_SECTION("immediate") {
        auto [i, s] = co_await untilCBCalled(
                [](std::function<void(int, std::string)> cb) {
                    cb(42, "forty-two");
                });
        CATCH_CHECK(i == 42);
        CATCH_CHECK(s == "forty-two");
    }

    CATCH_SECTION("delayed") {
        CORRAL_WITH_NURSERY(nursery) {
            int x = co_await untilCBCalled([&](std::function<void(int)> cb) {
                nursery.start([&, cb]() -> Task<> {
                    co_await t.sleep(2ms);
                    cb(42);
                });
            });
            CATCH_CHECK(t.now() == 2ms);
            CATCH_CHECK(x == 42);
            co_return cancel;
        };
    }

    CATCH_SECTION("multiple") {
        auto [i, s] = co_await untilCBCalled(
                [](std::function<void(int)> cbi,
                   std::function<void(std::string)> cbs) { cbs("hello"); });
        CATCH_CHECK(!i);
        CATCH_CHECK(*s == "hello");
    }
}

CORRAL_TEST_CASE("cb-cstyle") {
    CORRAL_WITH_NURSERY(nursery) {
        CBPortal<int> cbp;
        int x = co_await untilCBCalled(
                [&]<class CB>(CB& cb) {
                    void (*ccb)(void*, int) = +[](void* obj, int xx) {
                        (*reinterpret_cast<CB*>(obj))(xx);
                    };
                    void* ccbArg = &cb;

                    nursery.start([&, ccb, ccbArg]() -> Task<> {
                        co_await t.sleep(2ms);
                        (*ccb)(ccbArg, 42);
                    });
                },
                cbp);
        CATCH_CHECK(t.now() == 2ms);
        CATCH_CHECK(x == 42);
        co_return cancel;
    };
};

CORRAL_TEST_CASE("cb-anyof") {
    auto schedule = [&](std::function<void()> voidcb,
                        std::function<void(std::string)> strcb) {
        t.schedule(1ms, [voidcb] { voidcb(); });
        t.schedule(2ms, [strcb] { strcb("one"); });
        t.schedule(3ms, [voidcb] { voidcb(); });
        t.schedule(4ms, [strcb] { strcb("two"); });
    };

    CBPortal<> aw1;
    CBPortal<std::string> aw2;

    auto [v, s] = co_await untilCBCalled(schedule, aw1, aw2);
    CATCH_CHECK(v);
    CATCH_CHECK(!s);

    std::tie(v, s) = co_await anyOf(aw1, aw2);
    CATCH_CHECK(!v);
    CATCH_CHECK(*s == "one");

    std::tie(v, s) = co_await anyOf(aw1, aw2);
    CATCH_CHECK(v);
    CATCH_CHECK(!s);

    std::tie(v, s) = co_await anyOf(aw1, aw2);
    CATCH_CHECK(!v);
    CATCH_CHECK(*s == "two");
}

CORRAL_TEST_CASE("cb-immediate") {
    CBPortal<int> aw;
    auto func = [](std::function<void(int)> cb) {
        cb(1);
        cb(2);
        cb(3);
    };

    int x = co_await untilCBCalled(func, aw);
    CATCH_CHECK(x == 1);

    x = co_await aw;
    CATCH_CHECK(x == 2);

    x = co_await aw;
    CATCH_CHECK(x == 3);
}

CORRAL_TEST_CASE("cb-return-type") {
    auto val = [](std::function<void(int)> cb) { cb(42); };
    CBPortal<int> valCB;
    int x = co_await untilCBCalled(val, valCB);
    CATCH_CHECK(x == 42);

    auto lref = [&x](std::function<void(int&)> cb) { cb(x); };
    CBPortal<int&> lrefCB;
    int& lr = co_await untilCBCalled(lref, lrefCB);
    CATCH_CHECK(&lr == &x);

    auto cref = [&x](std::function<void(const int&)> cb) { cb(x); };
    CBPortal<const int&> crefCB;
    const int& cr = co_await untilCBCalled(cref, crefCB);
    CATCH_CHECK(&cr == &x);

    auto rref = [&x](std::function<void(int&&)> cb) { cb(std::move(x)); };
    CBPortal<int&&> rrefCB;
    int&& rr = co_await untilCBCalled(rref, rrefCB);
    CATCH_CHECK(&rr == &x);
}

CORRAL_TEST_CASE("cb-reflike") {
    std::function<void(const std::string&)> cb;
    co_await allOf(
            [&]() -> Task<> {
                auto portal = std::make_unique<CBPortal<const std::string&>>();
                std::string s = co_await untilCBCalled(
                        [&](std::function<void(const std::string&)> c) {
                            cb = c;
                        },
                        *portal);
                CATCH_CHECK(s == "hello");
                portal.reset();
            },
            [&]() -> Task<> {
                co_await t.sleep(1ms);
                cb("hello");
            });
}

CORRAL_TEST_CASE("cb-lightweight") {
    CATCH_SECTION("simplified") {
        auto [i, s] = co_await untilCBCalled(
                [](MyLightweightFn<void(int, std::string)> cb) {
                    cb(42, "forty-two");
                });
        CATCH_CHECK(i == 42);
        CATCH_CHECK(s == "forty-two");
    }

    CATCH_SECTION("explicit") {
        CBPortal<int, std::string> cbp;
        auto [i, s] = co_await untilCBCalled(
                [](MyLightweightFn<void(int, std::string)> cb) {
                    cb(42, "forty-two");
                },
                cbp);
        CATCH_CHECK(i == 42);
        CATCH_CHECK(s == "forty-two");
    }
}

} // namespace
