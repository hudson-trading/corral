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
#include <cstddef>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "config.h"
#include "../corral/detail/Queue.h"

#if __cpp_exceptions

namespace {

template <bool Copyable> struct TrackedQueueValue {
    static inline int alive = 0;
    static inline int transfers = 0;
    static inline int failOnTransfer = -1;

    int value;

    explicit TrackedQueueValue(int v) : value(v) { ++alive; }

    TrackedQueueValue(const TrackedQueueValue& other)
        requires(Copyable)
      : value(other.value) {
        transferred();
    }

    TrackedQueueValue(const TrackedQueueValue&)
        requires(!Copyable) = delete;

    TrackedQueueValue(TrackedQueueValue&& other) noexcept(false)
      : value(other.value) {
        transferred();
        other.value = -1;
    }

    TrackedQueueValue& operator=(const TrackedQueueValue&) = delete;
    TrackedQueueValue& operator=(TrackedQueueValue&&) = delete;

    ~TrackedQueueValue() noexcept { --alive; }

    static void reset() {
        alive = 0;
        transfers = 0;
        failOnTransfer = -1;
    }

    static void transferred() {
        if (++transfers == failOnTransfer) {
            throw std::runtime_error("injected queue transfer failure");
        }
        ++alive;
    }
};

// Forward iterator that supplies rvalue references without a range adaptor.
template <class T> class MovingIterator {
    T* ptr_ = nullptr;

  public:
    using iterator_concept = std::forward_iterator_tag;
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;

    MovingIterator() = default;
    explicit MovingIterator(T* ptr) : ptr_(ptr) {}

    T&& operator*() const { return std::move(*ptr_); }
    MovingIterator& operator++() {
        ++ptr_;
        return *this;
    }
    MovingIterator operator++(int) {
        auto prior = *this;
        ++*this;
        return prior;
    }
    bool operator==(const MovingIterator&) const = default;
};

template <class T>
void appendPair(corral::detail::Queue<T>& queue, std::array<T, 2>& values) {
    if constexpr (std::is_copy_constructible_v<T>) {
        queue.append(values.begin(), values.end());
    } else {
        queue.append(MovingIterator<T>{values.data()},
                     MovingIterator<T>{values.data() + values.size()});
    }
}

template <bool Copyable> void checkWrappedAppendFailure() {
    using T = TrackedQueueValue<Copyable>;
    T::reset();
    {
        std::array<T, 2> values{T{10}, T{20}};
        corral::detail::Queue<T> queue{4};
        queue.emplace_back(1);
        queue.emplace_back(2);
        queue.emplace_back(3);
        queue.pop_front();
        queue.pop_front();

        CATCH_REQUIRE(queue.size() == 1);
        CATCH_REQUIRE(T::alive == 3); // Two source values, one queued value.

        T::transfers = 0;
        T::failOnTransfer = 2; // First segment succeeds; second throws.
        bool caught = false;
        try {
            appendPair(queue, values);
        } catch (const std::runtime_error&) {
            caught = true;
        }
        T::failOnTransfer = -1;
        CATCH_REQUIRE(caught);
        CATCH_REQUIRE(queue.size() == 1);
        CATCH_REQUIRE(queue.front().value == 3);
        // A failure must destroy the completed first-segment construction.
        CATCH_REQUIRE(T::alive == 3);

        // The vacant slot must be safe to reuse after the exception.
        queue.emplace_back(99);
        CATCH_CHECK(queue.size() == 2);
        CATCH_CHECK(T::alive == 4);
        queue.pop_front();
        CATCH_REQUIRE(queue.front().value == 99);
        queue.pop_front();
        CATCH_CHECK(queue.size() == 0);
        CATCH_CHECK(T::alive == 2);
    }
    CATCH_CHECK(T::alive == 0);
}

template <bool Copyable> void checkWrappedGrowthFailure() {
    using T = TrackedQueueValue<Copyable>;
    T::reset();
    {
        corral::detail::Queue<T> queue{4};
        queue.emplace_back(1);
        queue.emplace_back(2);
        queue.emplace_back(3);
        queue.emplace_back(4);
        queue.pop_front();
        queue.pop_front();
        queue.emplace_back(5);
        queue.emplace_back(6);

        CATCH_REQUIRE(queue.size() == 4);
        CATCH_REQUIRE(T::alive == 4);

        T::transfers = 0;
        T::failOnTransfer = 3; // Two in first segment; third throws.
        bool caught = false;
        try {
            queue.emplace_back(7); // Forces growth of wrapped buffer.
        } catch (const std::runtime_error&) {
            caught = true;
        }
        T::failOnTransfer = -1;
        CATCH_REQUIRE(caught);
        CATCH_REQUIRE(queue.size() == 4);
        // Temporary queue must destroy its completed first segment.
        CATCH_REQUIRE(T::alive == 4);

        if constexpr (Copyable) {
            std::vector<int> observed;
            queue.for_each_item(
                    [&](const T& v) { observed.push_back(v.value); });
            const std::vector<int> expected{3, 4, 5, 6};
            CATCH_CHECK(observed == expected);
        }

        // Both copied and move-only queues must remain structurally reusable.
        queue.emplace_back(8);
        CATCH_CHECK(queue.size() == 5);
        CATCH_CHECK(T::alive == 5);
        for (int i = 0; i < 5; ++i) {
            queue.pop_front();
        }
        CATCH_CHECK(queue.size() == 0);
        CATCH_CHECK(T::alive == 0);
    }
    CATCH_CHECK(T::alive == 0);
}

} // namespace

CATCH_TEST_CASE("queue-wrapped-append-copy-failure-cleans-objects") {
    checkWrappedAppendFailure<true>();
}

CATCH_TEST_CASE("queue-wrapped-append-move-failure-cleans-objects") {
    checkWrappedAppendFailure<false>();
}

CATCH_TEST_CASE("queue-growth-copy-failure-cleans-temporary") {
    checkWrappedGrowthFailure<true>();
}

CATCH_TEST_CASE("queue-growth-move-failure-cleans-temporary") {
    checkWrappedGrowthFailure<false>();
}

#endif // __cpp_exceptions
