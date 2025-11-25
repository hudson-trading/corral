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

#include <thread>

#include "../corral/DegenerateThreadPool.h"
#include "../corral/corral.h"
#include "TestEventLoop.h"
#include "helpers.h"

using namespace corral;
using namespace corral::testing;

#define CORRAL_TEST_CASE(...)                                                  \
    CORRAL_TEST_CASE_IMPL(TestEventLoop, t, __VA_ARGS__)

namespace {

CORRAL_TEST_CASE("degenerate-thread-pool") {
    DegenerateThreadPool tp(t, 0);
    CATCH_CHECK(tp.isDegenerate());

    CATCH_SECTION("smoke-void") {
        bool called = false;
        co_await tp.run([&] { called = true; });
        CATCH_CHECK(called);
    }

    CATCH_SECTION("smoke-value") {
        auto tid = co_await tp.run([] { return std::this_thread::get_id(); });
        CATCH_CHECK(tid == std::this_thread::get_id());
    }

    CATCH_SECTION("reference") {
        int x = 0;
        int& y = co_await tp.run([&x]() -> int& { return x; });
        CATCH_CHECK(&x == &y);

        int&& ry = co_await tp.run([&x]() -> int&& { return std::move(x); });
        CATCH_CHECK(&x == &ry);
    }

    CATCH_SECTION("exception") {
        CATCH_CHECK_THROWS_WITH(
                co_await tp.run([] { throw std::runtime_error("boo!"); }),
                Catch::Equals("boo!"));
    }

    CATCH_SECTION("cancel-token") {
        // DegenerateThreadPool runs tasks inline and does not support
        // cancellation, but the passed in function still can take
        // a token for API compatibility.
        co_await tp.run([&](DegenerateThreadPool::CancelToken token) {
            CATCH_CHECK(!token);
        });
    }

    CATCH_SECTION("early-cancellation") {
        std::atomic<bool> called{false};
        Event evt;
        evt.trigger();
        co_await anyOf(evt, tp.run([&] { called = true; }));
        CATCH_CHECK(!called.load());
    }
}

} // namespace
