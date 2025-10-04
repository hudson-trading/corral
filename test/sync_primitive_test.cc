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

#define CORRAL_TEST_CASE(...)                                                  \
    CORRAL_TEST_CASE_IMPL(TestEventLoop, t, __VA_ARGS__)

namespace {

CORRAL_TEST_CASE("event") {
    Event evt;
    auto task = [&]() -> Task<> {
        co_await evt;
        CATCH_CHECK(t.now() == 2ms);
        co_await evt;
        CATCH_CHECK(t.now() == 2ms);
    };
    auto trigger = [&]() -> Task<> {
        co_await t.sleep(2ms);
        evt.trigger();
    };
    co_await allOf(task, trigger);
}

CORRAL_TEST_CASE("parking-lot") {
    ParkingLot pl;
    size_t running = 0;

    auto task = [&]() -> Task<> {
        co_await t.sleep(1ms);
        co_await pl.park();
        ++running;
    };

    CORRAL_WITH_NURSERY(nursery) {
        for (int i = 0; i != 5; ++i) {
            nursery.start(task);
        }
        CATCH_CHECK(pl.empty());
        co_await t.sleep(2ms);
        CATCH_CHECK(!pl.empty());
        CATCH_CHECK(running == 0);

        pl.unparkOne();
        co_await t.sleep(1ms);
        CATCH_CHECK(running == 1);

        pl.unparkOne();
        pl.unparkOne();
        co_await t.sleep(1ms);
        CATCH_CHECK(running == 3);

        pl.unparkAll();
        co_await t.sleep(1ms);
        CATCH_CHECK(running == 5);

        co_return join;
    };
}

CORRAL_TEST_CASE("semaphores") {
    CATCH_SECTION("smoke") {
        Semaphore sem(5);
        int concurrency = 0;
        auto worker = [&]() -> Task<> {
            auto lk = co_await sem.lock();
            ++concurrency;
            CATCH_CHECK(concurrency <= 5);
            co_await t.sleep(1ms);
            --concurrency;
        };
        CORRAL_WITH_NURSERY(nursery) {
            for (int i = 0; i != 20; ++i) {
                nursery.start(worker);
            }
            co_return join;
        };
        CATCH_CHECK(t.now() == 4ms);
        CATCH_CHECK(sem.value() == 5);
    }

    CATCH_SECTION("acquire-race") {
        Semaphore sem(1);
        // Both awaitables see the semaphore as available, but only
        // one can acquire it.
        co_await anyOf(t.sleep(2ms), allOf(sem.acquire(), sem.acquire()));
        CATCH_CHECK(t.now() == 2ms);
        CATCH_CHECK(sem.value() == 0);
    }

    CATCH_SECTION("acquire-release-race") {
        Semaphore sem(0);
        // Semaphore awaitable's await_ready() initially returns false,
        // but by the time its await_suspend() runs, the semaphore gets
        // a free resource, so acquire() should complete immediately.
        co_await allOf(
                [&]() -> Task<> {
                    sem.release();
                    return noop();
                },
                sem.acquire());
    }

    CATCH_SECTION("not-awaited-acquire") {
        Semaphore sem(1);
        // acquire() is short-circuited by an immediately-ready awaitable
        // and is not awaited, so semaphore's resource should not leak.
        auto [_, acquired] = co_await anyOf(Ready{}, sem.acquire());
        CATCH_CHECK(!acquired);
        CATCH_CHECK(sem.value() == 1);
    }
}

CORRAL_TEST_CASE("value") {
    Value<int> v{0};

    co_await allOf(
            [&]() -> Task<> {
                co_await until(v <= 0);
                CATCH_CHECK(t.now() == 0ms);

                auto [from, to] = co_await v.untilChanged();
                CATCH_CHECK(t.now() == 1ms);
                CATCH_CHECK(from == 0);
                CATCH_CHECK(to == 3);

                std::tie(from, to) = co_await v.untilChanged();
                // Should skip the 3->3 change
                CATCH_CHECK(t.now() == 3ms);
                CATCH_CHECK(from == 3);
                CATCH_CHECK(to == 4);

                to = co_await v.untilEquals(5);
                CATCH_CHECK(t.now() == 4ms);
                CATCH_CHECK(to == 5);
                CATCH_CHECK(v == 7);
            },
            [&]() -> Task<> {
                co_await t.sleep(1ms);
                v = 3;

                co_await t.sleep(1ms);
                v = 3;
                co_await t.sleep(1ms);
                ++v;

                co_await t.sleep(1ms);
                v = 7;
                v -= 2;
                v += 2;
            });

    CATCH_CHECK(t.now() == 4ms);

    Value<std::function<void()>> vf{};
    co_await allOf(
            [&]() -> Task<> {
                co_await t.sleep(1ms);
                vf = [] {};
                co_await t.sleep(2ms);
                vf = nullptr;
            },
            [&]() -> Task<> {
                co_await until(!!vf);
                CATCH_CHECK(t.now() == 5ms);

                co_await until(vf.isTrue()); // alternate syntax
                CATCH_CHECK(t.now() == 5ms);

                CATCH_CHECK(vf);
                CATCH_CHECK(vf.isTrue());

                co_await until(!vf);
                CATCH_CHECK(t.now() == 7ms);

                CATCH_CHECK(!vf);
                CATCH_CHECK(!vf.isTrue());
            });
}

} // namespace
