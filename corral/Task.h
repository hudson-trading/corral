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

#pragma once
#include "detail/Promise.h"
#include "detail/task_awaitables.h"
#include "utility.h"

namespace corral {
class Nursery;

/// An async task backed by a C++20 coroutine.
///
/// This type shows up primarily as the return value of async functions.
/// It has no public methods but is movable and awaitable. The async function
/// does not start running until the Task wrapping it is awaited.
template <class T = void> class [[nodiscard]] Task : public detail::TaskTag {
  public:
    using promise_type = detail::Promise<T>;
    using ReturnType = T;

    Task() = default;
    Task(Task<T>&& c) noexcept : promise_(std::exchange(c.promise_, nullptr)) {}
    explicit Task(detail::Promise<T>& promise) : promise_(&promise) {}

    Task& operator=(Task<T>&& c) noexcept {
        if (this != &c) {
            destroy();
            promise_ = std::exchange(c.promise_, nullptr);
        }
        return *this;
    }

    explicit operator bool() const { return promise_ != nullptr; }

    ~Task() { destroy(); }

    /// co_await'ing on a task starts it and suspends the caller until its
    /// completion.
    auto operator co_await() { return detail::TaskAwaitable<T>(promise_); }

  private:
    void destroy() {
        auto promise = std::exchange(promise_, nullptr);
        if constexpr (std::is_same_v<T, void>) {
            if (promise == detail::noopPromise()) {
                return;
            }
        }
        if (promise) {
            promise->destroy();
        }
    }

    detail::Promise<T>* release() { return std::exchange(promise_, nullptr); }

  private:
    detail::Promise<T>* promise_ = nullptr;
    friend class Nursery;
};

namespace detail {
template <class T> Task<T> Promise<T>::get_return_object() {
    return Task<T>(*this);
}
} // namespace detail

/// A no-op task. Always await_ready(), and co_await'ing on it is a no-op
/// either (i.e. immediately resumes the caller).
///
/// Can be useful when defining interfaces having optional methods:
///
///     struct IExample {
///         virtual corral::Task<void> optionalToImplement() {
///             return corral::noop();
///         }
///     };
///
/// saving on coroutine frame allocation (compared to `{ co_return; }`).
inline Task<void> noop() {
    return Task<void>(*detail::noopPromise());
}

} // namespace corral
