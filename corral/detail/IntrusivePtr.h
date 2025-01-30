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

#include <cstddef>
#include <type_traits>

namespace corral::detail {

template <class T> class IntrusivePtr;

/// A mixin for reference-counted objects.
template <class Self> class RefCounted {
    size_t refCount_ = 0;
    friend IntrusivePtr<Self>;

    RefCounted(const RefCounted&) = delete;
    RefCounted& operator=(const RefCounted&) = delete;

  public:
    RefCounted() = default;
};

/// A lightweight replacement for std::shared_ptr
/// which does not have the overhead of atomic operations.
template <class T> class IntrusivePtr {
    T* ptr_;

    void ref() {
        if (ptr_) {
            ptr_->refCount_++;
        }
    }
    void deref() {
        if (ptr_ && --ptr_->refCount_ == 0) {
            delete ptr_;
        }
    }

  public:
    IntrusivePtr() : ptr_(nullptr) {}
    explicit(false) IntrusivePtr(std::nullptr_t) : ptr_(nullptr) {}
    explicit IntrusivePtr(T* ptr) : ptr_(ptr) { ref(); }
    IntrusivePtr(const IntrusivePtr& rhs) : ptr_(rhs.ptr_) { ref(); }
    IntrusivePtr(IntrusivePtr&& rhs) noexcept
      : ptr_(std::exchange(rhs.ptr_, nullptr)) {}
    IntrusivePtr& operator=(IntrusivePtr rhs) {
        std::swap(ptr_, rhs.ptr_);
        return *this;
    }
    ~IntrusivePtr() {
        // Delay the check until instantiation to allow incomplete types.
        static_assert(std::is_base_of_v<RefCounted<T>, T>,
                      "T must be derived from RefCounted<T>");
        deref();
    }

    T* get() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    T* operator->() const noexcept { return ptr_; }

    explicit operator bool() const noexcept { return ptr_ != nullptr; }
};

} // namespace corral::detail
