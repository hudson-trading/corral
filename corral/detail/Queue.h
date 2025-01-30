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

#include <memory>
#include <span>
#include <type_traits>
#include <utility>

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

    void grow() {
        Queue q(std::max<size_t>(8, capacity_ * 2));
        auto moveRange = [&](std::span<T> range) {
            if constexpr (std::is_nothrow_move_constructible_v<T>) {
                q.tail_ = std::uninitialized_move(range.begin(), range.end(),
                                                  q.tail_);
            } else {
                q.tail_ = std::uninitialized_copy(range.begin(), range.end(),
                                                  q.tail_);
            }
            q.size_ += range.size();
        };
        moveRange(first_range());
        moveRange(second_range());
        *this = std::move(q);
    }
};

} // namespace corral::detail
