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
#include "ErrorPolicy.h"
#include "detail/Promise.h"
#include "detail/TaskAwaiter.h"
#include "utility.h"

namespace corral {

namespace detail {
class NurseryBase;
}
template <class PolicyT> class BasicNursery;


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
    explicit Task(detail::Promise<T>& promise) : promise_(&promise) {}

    bool valid() const { return promise_.get() != nullptr; }

    /// co_await'ing on a task starts it and suspends the caller until its
    /// completion.
    Awaiter<T> auto operator co_await() noexcept {
        return detail::TaskAwaiter<T>(promise_.get());
    }

  private:
    detail::Promise<T>* release() { return promise_.release(); }

  private:
    detail::PromisePtr<T> promise_;

    friend detail::NurseryBase;
    template <class, class, class...> friend class detail::TryBlock;

    template <class PolicyT> friend class BasicNursery;
};

namespace detail {
template <class T> Task<T> Promise<T>::get_return_object() {
    return Task<T>(*this);
}

/// A non-cancellable awaitable which is immediately ready, producing a
/// value of type T. It can also be implicitly converted to a Task<T>.
template <class T> class ReadyAwaiter {
  public:
    explicit ReadyAwaiter(T&& value) : value_(std::forward<T>(value)) {}

    bool await_early_cancel() const noexcept { return false; }
    bool await_ready() const noexcept { return true; }
    bool await_suspend(Handle) { return false; }
    bool await_cancel(Handle) noexcept { return false; }
    bool await_must_resume() const noexcept { return true; }
    T await_resume() && { return std::forward<T>(value_); }

    template <std::constructible_from<T> U> operator Task<U>() && {
        return Task<U>(*new StubPromise<U>(std::forward<T>(value_)));
    }

  private:
    T value_;
};

template <> class ReadyAwaiter<void> {
  public:
    bool await_early_cancel() const noexcept { return false; }
    bool await_ready() const noexcept { return true; }
    bool await_suspend(Handle) { return false; }
    bool await_cancel(Handle) noexcept { return false; }
    bool await_must_resume() const noexcept { return true; }
    void await_resume() && {}

    operator Task<void>() { return Task<void>(StubPromise<void>::instance()); }
};

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
inline Awaitable<void> auto noop() {
    return detail::ReadyAwaiter<void>();
}

/// Create a task that immediately returns a given value when co_await'ed.
template <class T> Awaitable<T> auto just(T value) {
    return detail::ReadyAwaiter<T>(std::forward<T>(value));
}

} // namespace corral
