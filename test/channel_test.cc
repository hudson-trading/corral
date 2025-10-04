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

#include <array>
#include <forward_list>
#include <ranges>
#include <sstream>
#include <string>
#include <vector>

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

//
// Bounded channels
//

CORRAL_TEST_CASE("bounded-channel-smoke") {
    Channel<int> channel{3};

    std::vector<std::optional<int>> results;

    co_await channel.send(1);
    co_await channel.send(2);
    co_await channel.send(3);

    CATCH_CHECK(channel.full());
    results.push_back(co_await channel.receive());
    results.push_back(co_await channel.receive());
    results.push_back(co_await channel.receive());
    CATCH_CHECK(channel.empty());
    CATCH_CHECK(results == std::vector<std::optional<int>>{1, 2, 3});

    channel.close();

    auto last = co_await channel.receive();
    CATCH_CHECK(last == std::nullopt);
}

CORRAL_TEST_CASE("bounded-channel-blocking") {
    Channel<int> channel{3};
    bool ranLast = false;

    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> { co_await channel.send(1); });
        n.start([&]() -> Task<> { co_await channel.send(2); });
        n.start([&]() -> Task<> { co_await channel.send(3); });
        n.start([&]() -> Task<> {
            co_await channel.send(4);
            ranLast = true;
        });

        co_await t.sleep(5ms);

        CATCH_CHECK(channel.size() == 3);
        CATCH_CHECK(channel.full());
        CATCH_CHECK(ranLast == false);

        std::array<std::optional<int>, 3> values;
        values[0] = co_await channel.receive();
        values[1] = co_await channel.receive();
        values[2] = co_await channel.receive();
        CATCH_CHECK(values == std::array<std::optional<int>, 3>{1, 2, 3});

        co_await yield;

        CATCH_CHECK(ranLast == true);
        CATCH_CHECK(channel.size() == 1);

        int value = *co_await channel.receive();
        CATCH_CHECK(value == 4);
        CATCH_CHECK(channel.size() == 0);

        co_return join;
    };
}

CORRAL_TEST_CASE("bounded-channel-alternating") {
    Channel<int> channel{3};
    std::vector<int> results;

    co_await allOf(
            [&]() -> Task<> {
                for (int i = 0; i < 10; i++) {
                    co_await channel.send(i);
                }
                channel.close();
            },
            [&]() -> Task<> {
                while (std::optional<int> v = co_await channel.receive()) {
                    if (*v % 2 == 0) {
                        continue;
                    }
                    results.push_back(*v);
                }
            });

    CATCH_CHECK(results == std::vector<int>{1, 3, 5, 7, 9});
}

CORRAL_TEST_CASE("bounded-channel-try") {
    Channel<int> channel{3};
    // False because channel empty
    CATCH_CHECK(!channel.tryReceive());

    // True because free space
    CATCH_CHECK(channel.trySend(1));
    CATCH_CHECK(channel.trySend(2));
    CATCH_CHECK(channel.trySend(3));

    // False because full
    CATCH_CHECK(!channel.trySend(4));

    // True because not empty
    CATCH_CHECK(channel.tryReceive());
    CATCH_CHECK(channel.tryReceive());

    channel.close();

    // We can still read remainign data while closed
    CATCH_CHECK(channel.tryReceive());

    // But we cannot write new data while closed
    CATCH_CHECK(channel.empty());
    CATCH_CHECK(channel.closed());
    CATCH_CHECK(!channel.trySend(5));

    co_return;
}

CORRAL_TEST_CASE("bounded-channel-close") {
    Channel<int> channel{3};
    std::vector<std::optional<int>> results;

    CORRAL_WITH_NURSERY(nursery) {
        co_await channel.send(1);
        co_await channel.send(2);
        co_await channel.send(3);

        // Discard result
        auto send = [&](int value) -> Task<> { co_await channel.send(value); };

        // These should remain blocked
        nursery.start(send, 4);
        nursery.start(send, 5);

        co_await yield;

        CATCH_CHECK(channel.full());
        CATCH_CHECK(!channel.closed());
        channel.close();
        CATCH_CHECK(channel.full());
        CATCH_CHECK(channel.closed());

        while (auto item = co_await channel.receive()) {
            results.push_back(*item);
        }

        co_return cancel;
    };


    CATCH_CHECK(channel.empty());
    CATCH_CHECK(results == std::vector<std::optional<int>>{1, 2, 3});

    // More reads will return nullopt
    std::optional<int> item = co_await channel.receive();
    CATCH_CHECK(item == std::nullopt);

    bool sent = co_await channel.send(6);
    CATCH_CHECK(sent == false);
}


//
// Unbounded channels
//

CORRAL_TEST_CASE("unbounded-channel-smoke") {
    Channel<int> channel;

    for (int i = 0; i < 10'000; i++) {
        CATCH_CHECK(channel.size() == i);
        co_await channel.send(i);
    }

    CATCH_CHECK(channel.size() == 10'000);

    for (int i = 0; i < 10'000; i++) {
        std::optional<int> v = co_await channel.receive();
        CATCH_REQUIRE(v.has_value());
        CATCH_CHECK(*v == i);
    }

    CATCH_CHECK(channel.size() == 0);
    CATCH_CHECK(channel.empty());
}

CORRAL_TEST_CASE("unbounded-channel-close") {
    Channel<int> channel;
    std::vector<std::optional<int>> results;

    bool sent = co_await channel.send(1);
    CATCH_CHECK(sent);
    channel.close();

    results.push_back(co_await channel.receive());
    CATCH_CHECK(channel.empty());
    results.push_back(co_await channel.receive());
    CATCH_CHECK(channel.empty());

    CATCH_CHECK(results == std::vector<std::optional<int>>{1, std::nullopt});

    // More writes will fail
    sent = co_await channel.send(2);
    CATCH_CHECK(!sent);
}


//
// Bulk operations on channels
//

CORRAL_TEST_CASE("channel-bulk-smoke") {
    Channel<char> channel(16);
    size_t wr = co_await channel.send(std::string_view("0123456789ab"));
    CATCH_CHECK(wr == 12);
    CATCH_CHECK(channel.size() == 12);
    CATCH_CHECK(channel.space() == 4);

    std::string_view sv("ABCDEFGH");
    auto it = co_await channel.send(sv.begin(), sv.end());
    CATCH_CHECK(it == sv.begin() + 4);
    CATCH_CHECK(channel.size() == 16);
    CATCH_CHECK(channel.space() == 0);

    std::string buf(32, '\0');
    auto o = co_await channel.receive(buf.begin(), 4);
    CATCH_CHECK(o == buf.begin() + 4);
    CATCH_CHECK(buf.substr(0, 4) == "0123");

    wr = co_await channel.send(std::string_view("98765432"));
    CATCH_CHECK(wr == 4);

    size_t rd = co_await channel.receive(buf);
    CATCH_CHECK(rd == 16);
}

CORRAL_TEST_CASE("channel-bulk-iterator-categories") {
    Channel<int> channel;

    // send(forward_interator)
    std::forward_list<int> list{1, 2, 3, 4, 5};
    size_t wr = co_await channel.send(list);
    CATCH_CHECK(wr == 5);
    list.clear();

    // send(input_iterator)
    std::istringstream iss("6 7 8");
    co_await channel.send(std::istream_iterator<int>(iss),
                          std::istream_iterator<int>());

    // receive(output_iterator)
    std::vector<int> vec;
    co_await channel.receive(std::back_inserter(vec));
    CATCH_CHECK(vec == std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8});
}

CORRAL_TEST_CASE("channel-bulk-read-multi-wakeup") {
    Channel<int> channel;

    std::vector<int> v;
    v.resize(6);
    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> { co_await channel.receive(v.begin(), 1); });
        n.start([&]() -> Task<> {
            co_await channel.receive(v.begin() + 1, 2);
        });
        n.start([&]() -> Task<> {
            co_await channel.receive(v.begin() + 3, 3);
        });

        co_await t.sleep(1ms);
        co_await channel.send(std::array{1, 2, 3, 4, 5, 6});
        co_return join;
    };
    CATCH_CHECK(v == std::vector<int>{1, 2, 3, 4, 5, 6});
}

CORRAL_TEST_CASE("channel-bulk-write-multi-wakeup") {
    Channel<int> channel(6);
    co_await channel.send(std::array{10, 11, 12, 13, 14, 15});

    std::vector<int> v;
    v.resize(6);
    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> { co_await channel.send(1); });
        n.start([&]() -> Task<> { co_await channel.send(std::array{2, 3}); });
        n.start([&]() -> Task<> {
            co_await channel.send(std::array{4, 5, 6});
        });

        co_await t.sleep(1ms);
        co_await channel.receive(v.begin(), 6);
        co_return join;
    };

    CATCH_CHECK(v == std::vector<int>{10, 11, 12, 13, 14, 15});
    co_await channel.receive(v.begin(), 6);
    CATCH_CHECK(v == std::vector<int>{1, 2, 3, 4, 5, 6});
}

CORRAL_TEST_CASE("channel-bulk-race") {
    Channel<int> channel;
    co_await channel.send(1);

    co_await allOf(
            [&]() -> Task<> {
                auto [a, b] =
                        co_await allOf(channel.receive(), channel.receive());
                CATCH_CHECK(t.now() == 2ms);

                if (*a == 1) {
                    CATCH_CHECK(*b == 2);
                } else {
                    CATCH_CHECK(*a == 2);
                    CATCH_CHECK(*b == 1);
                }
            },
            [&]() -> Task<> {
                co_await t.sleep(2ms);
                co_await channel.send(2);
            });
}

CORRAL_TEST_CASE("channel-bulk-concurrent-read-oversubscribe") {
    Channel<int> channel;
    co_await channel.send(std::array{1, 2, 3, 4, 5, 6});

    std::vector<int> a, b, c;
    co_await allOf(channel.receive(std::back_inserter(a), 6),
                   channel.receive(std::back_inserter(b), 6),
                   channel.receive(std::back_inserter(c), 6));

    CATCH_CHECK(a == std::vector<int>{1, 2, 3, 4});
    CATCH_CHECK(b == std::vector<int>{5});
    CATCH_CHECK(c == std::vector<int>{6});
}

CORRAL_TEST_CASE("channel-bulk-imm-cancel") {
    Channel<int> channel;
    co_await channel.send(std::array{1, 2, 3, 4});

    auto [_, r] = co_await anyOf(std::suspend_never{}, channel.receive());
    CATCH_CHECK(!r);
    CATCH_CHECK(channel.size() == 4);
}

CORRAL_TEST_CASE("channel-bulk-pending-op") {
    Channel<int> channel;
    co_await channel.send(1);

    CATCH_SECTION("create-await") {
        auto awaitable = channel.receive();
        auto awaiter = std::move(awaitable).operator co_await();
        CATCH_CHECK(channel.empty());
        CATCH_CHECK(channel.tryReceive() == std::nullopt);

        auto v = co_await std::move(awaiter);
        CATCH_CHECK(*v == 1);
    }

    CATCH_SECTION("create-destroy") {
        {
            auto awaitable = channel.receive();
            auto awaiter = std::move(awaitable).operator co_await();
            CATCH_CHECK(channel.empty());
            CATCH_CHECK(channel.tryReceive() == std::nullopt);
        }
        CATCH_CHECK(channel.size() == 1);
        auto v = co_await channel.receive();
        CATCH_CHECK(v == 1);
    }
}

CORRAL_TEST_CASE("channel-bulk-zero-sized-read") {
    Channel<int> channel;
    int a;
    std::optional<int> b;
    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> { co_await channel.receive(&a, 0); });
        n.start([&]() -> Task<> { b = co_await channel.receive(); });

        co_await t.sleep(1ms);
        co_await channel.send(1);
        co_return join;
    };
    CATCH_CHECK(b == 1);
}

} // namespace
