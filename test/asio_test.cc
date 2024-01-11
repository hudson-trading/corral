// This file is part of corral, a lightweight C++20 coroutine library.
//
// Copyright (c) 2024 Hudson River Trading LLC <opensource@hudson-trading.com>
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

#include "../corral/asio.h"

#include <chrono>

#include <boost/asio.hpp>

#include "../corral/corral.h"
#include "config.h"
#include "helpers.h"

#define CORRAL_TEST_CASE(...)                                                  \
    CORRAL_TEST_CASE_IMPL(boost::asio::io_service, io, __VA_ARGS__)

namespace {

using Clock = std::chrono::high_resolution_clock;
using namespace std::chrono_literals;

CORRAL_TEST_CASE("asio-smoke", "[asio]") {
    boost::asio::deadline_timer t(io);
    t.expires_from_now(boost::posix_time::millisec(100));
    auto from = Clock::now();
    co_await t.async_wait(corral::asio_awaitable);
    CATCH_CHECK(Clock::now() - from >= 90ms);
}

CORRAL_TEST_CASE("asio-anyof", "[asio]") {
    boost::asio::deadline_timer t1(io), t2(io);
    t1.expires_from_now(boost::posix_time::millisec(100));
    t2.expires_from_now(boost::posix_time::millisec(500));
    auto from = Clock::now();
    auto [s1, s2] =
            co_await corral::anyOf(t1.async_wait(corral::asio_awaitable),
                                   t2.async_wait(corral::asio_awaitable));

    auto d = Clock::now() - from;
    CATCH_CHECK(d >= 90ms);
    CATCH_CHECK(d <= 150ms);

    CATCH_CHECK(s1);
    CATCH_CHECK(!s2);
}

} // namespace
