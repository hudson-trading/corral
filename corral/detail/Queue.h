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

#pragma once
#include <stddef.h>

#include <iterator>
#include <memory>
#include <ranges>
#include <span>
#include <type_traits>
#include <utility>

#include "../config.h"

namespace corral::detail {

/// A makeshift queue.
/// Uses a circular buffer under the hood, so effectively zero-malloc (after
/// the initial allocation), provided elements are dequeued with a decent pace
/// to keep number of elements in the queue small.
template <class T>
    requires(std::is_nothrow_destructible_v<T>)
class Queue {
    T* buffer_;
    size_t capacity_;
    T* head_;
    T* tail_;
    size_t size_;

  public:
    explicit Queue(size_t capacity)
      : buffer_(static_cast<T*>(malloc(capacity * sizeof(T)))),
        capacity_(capacity),
        head_(buffer_),
        tail_(buffer_),
        size_(0) {}

    Queue(Queue<T>&& rhs) noexcept
      : buffer_(std::exchange(rhs.buffer_, nullptr)),
        capacity_(std::exchange(rhs.capacity_, 0)),
        head_(std::exchange(rhs.head_, nullptr)),
        tail_(std::exchange(rhs.tail_, nullptr)),
        size_(std::exchange(rhs.size_, 0)) {}

    Queue& operator=(Queue<T> rhs) noexcept {
        std::swap(buffer_, rhs.buffer_);
        std::swap(capacity_, rhs.capacity_);
        std::swap(head_, rhs.head_);
        std::swap(tail_, rhs.tail_);
        std::swap(size_, rhs.size_);
        return *this;
    }

    ~Queue() {
        auto destroyRange = [](std::span<T> range) {
            std::destroy(range.begin(), range.end());
        };
        destroyRange(first_range());
        destroyRange(second_range());
        free(buffer_);
    }

    size_t capacity() const { return capacity_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    T& front() { return *head_; }
    void pop_front() {
        head_->~T();
        if (++head_ == buffer_ + capacity_) {
            head_ = buffer_;
        }
        --size_;
    }

    template <class U> void push_back(U&& u) {
        if (size_ == capacity_) {
            grow();
        }
        new (tail_) T(std::forward<U>(u));
        if (++tail_ == buffer_ + capacity_) {
            tail_ = buffer_;
        }
        ++size_;
    }

    template <class... Args> void emplace_back(Args&&... args) {
        if (size_ == capacity_) {
            grow();
        }
        new (tail_) T(std::forward<Args>(args)...);
        if (++tail_ == buffer_ + capacity_) {
            tail_ = buffer_;
        }
        ++size_;
    }

    template <std::invocable<T&> Fn> void for_each_item(Fn&& fn) {
        for (T& elem : first_range()) {
            std::forward<Fn>(fn)(elem);
        }
        for (T& elem : second_range()) {
            std::forward<Fn>(fn)(elem);
        }
    }

    template <std::forward_iterator It>
        requires(std::constructible_from<T, std::iter_reference_t<It>>)
    void append(It begin, It end) {
        size_t sz = std::distance(begin, end);
        if (size_ + sz > capacity_) {
            grow(size_ + sz);
        }

        if (tail_ + sz <= buffer_ + capacity_) {
            // Free space unfragmented
            tail_ = moveRange(begin, end, tail_);
        } else {
            auto bound = begin;
            std::advance(bound, std::distance(tail_, buffer_ + capacity_));
            moveRange(begin, bound, tail_);
            tail_ = moveRange(bound, end, buffer_);
        }
        size_ += sz;
        if (tail_ == buffer_ + capacity_) {
            tail_ = buffer_;
        }
    }

    template <std::output_iterator<T> It>
    std::pair<It, size_t /*elem count*/> pop_front_to(size_t n, It dst) {
        CORRAL_ASSERT(n <= size_);
        size_t n1 = std::min<size_t>(
                n, static_cast<size_t>(buffer_ + capacity_ - head_));
        dst = std::move(head_, head_ + n1, dst);
        std::destroy(head_, head_ + n1);
        head_ += n1;
        if (head_ == buffer_ + capacity_) {
            head_ = buffer_;
        }
        size_ -= n1;

        n = std::min<size_t>(n - n1, static_cast<size_t>(tail_ - buffer_));
        if (n) {
            dst = std::move(buffer_, buffer_ + n, dst);
            std::destroy(buffer_, buffer_ + n);
            head_ = buffer_ + n;
            size_ -= n;
        }

        return {dst, n + n1};
    }

  private:
    std::span<T> first_range() {
        if (empty()) {
            return {};
        } else if (head_ < tail_) {
            return {head_, tail_};
        } else {
            return {head_, buffer_ + capacity_};
        }
    }

    std::span<T> second_range() {
        if (empty() || head_ < tail_) {
            return {};
        } else {
            return {buffer_, tail_};
        }
    }

    void grow(size_t newCapacity = 0) {
        Queue q(std::max<size_t>(std::max<size_t>(newCapacity, 8),
                                 capacity_ * 2));
        auto dst = moveRange(first_range(), q.tail_);
        q.tail_ = moveRange(second_range(), dst);
        q.size_ = size_;
        *this = std::move(q);
    }

    static auto moveRange(auto it, auto ie, auto dst) {
        if constexpr (std::is_nothrow_move_constructible_v<T>) {
            return std::uninitialized_move(it, ie, dst);
        } else {
            return std::uninitialized_copy(it, ie, dst);
        }
    }

    static auto moveRange(auto&& range, auto dst) {
        return moveRange(range.begin(), range.end(), dst);
    }
};

} // namespace corral::detail
