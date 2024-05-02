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
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
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

#include <iostream>
#include <optional>
#include <ranges>

#include "../corral/asio.h"
#include "../corral/corral.h"

using tcp = boost::asio::ip::tcp;
namespace posix = boost::asio::posix;

/// An example sketch implementation of the Happy Eyeballs connection algorithm
/// (RFC 8305) (less endpoint reordering, irrelevant for a concurrency example).
///
/// Essentially tries to connect to each endpoint in `endpoints` concurrently,
/// with staggered startup; Nth endpoint is attempted after (N-1)th endpoint
/// fails, or after `delay` milliseconds since it started, whichever comes
/// first.
corral::Task<std::optional<tcp::socket>> happy_eyeballs_connect(
        boost::asio::io_service& io_service,
        std::ranges::range auto endpoints,
        boost::posix_time::milliseconds delay) {
    std::optional<tcp::socket> result;
    corral::Value<size_t> current{0};  // A barrier for individual connection tasks.
                                       // If K is stored, connections to [0,K] may be attempted.
    size_t remaining = 0;

    CORRAL_WITH_NURSERY(nursery) {
        auto try_connect = [&](size_t idx,
                               tcp::endpoint addr) -> corral::Task<void> {
            auto kickOffNext = [&] {
                current = std::max<size_t>(current, idx + 1);
            };

            // Wait until allowed to start by the previous attempt.
            co_await current.untilMatches([idx](auto c) { return c >= idx; });

            // Start the connection attempt in background
            // (as we don't want it cancelled after `delay`).
            nursery.start([&, addr]() -> corral::Task<void> {
                tcp::socket sock(io_service);
                std::cout << "trying " << addr << "...\n";
                auto ec = co_await sock.async_connect(
                        addr, corral::asio_nothrow_awaitable);

                if (!ec) {
                    std::cout << "connected to " << addr << "\n";
                    result.emplace(std::move(sock));
                    nursery.cancel();
                } else if (remaining--) {
                    kickOffNext(); // If next endpoint is still pending, start it.
                                   // Note this may execute out of order, so `max()`
                                   // call is necessary in `kickOffNext()`.
                } else {
                    nursery.cancel(); // no more endpoints to try
                }
            });

            // Kick off next endpoint after `delay` milliseconds.
            // As kickOffNext() is idempotent, we can do it unconditionally
            // upon timeout expiration, not caring about whether the lambda above
            // has already done so.
            co_await corral::sleepFor(io_service, delay);
            kickOffNext();
        };

        for (const auto& endpoint : endpoints) {
            nursery.start(try_connect, remaining++, endpoint);
        }

        CORRAL_SUSPEND_FOREVER(); // keep `try_connect` lambda in scope
    };

    co_return result;
}


int main(int argc, char** argv) {
    if (argc != 4) {
        throw std::runtime_error(
                "usage: asio_happy_eyeballs <host> <port> <delay_ms>");
    }
    boost::asio::io_service io_service(1);
    tcp::resolver resolver(io_service);
    auto endpoints = corral::run(
            io_service,
            resolver.async_resolve(tcp::resolver::query{argv[1], argv[2]},
                                   corral::asio_awaitable));
    auto delay = boost::posix_time::milliseconds(std::stoi(argv[3]));
    auto sock = corral::run(
            io_service, happy_eyeballs_connect(io_service, endpoints, delay));
    return sock ? 0 : 1;
}
