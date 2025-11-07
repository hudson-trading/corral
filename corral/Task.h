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

#include <bit>

#include "ErrorPolicy.h"
#include "detail/Promise.h"
#include "detail/TaskAwaiter.h"
#include "utility.h"

namespace corral {

namespace detail {
class NurseryBase;
template <class> class ReadyAwaiter;
} // namespace detail
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
    explicit Task(detail::Promise<T>& promise) : storage_(&promise) {}

    bool valid() const { return !storage_.isEmpty(); }

    /// co_await'ing on a task starts it and suspends the caller until its
    /// completion.
    Awaiter<T> auto operator co_await() noexcept {
        return detail::TaskAwaiter<T>(std::move(storage_));
    }

  private:
    explicit Task(std::in_place_t, detail::InhabitedType<T> value)
      : storage_(std::forward<detail::InhabitedType<T>>(value)) {}

    bool ready() const noexcept { return storage_.hasValue(); }
    detail::Promise<T>* release() { return storage_.takePromise(); }
    detail::InhabitedType<T> takeValue() && {
        return std::move(storage_).takeValue();
    }

  private:
    detail::ValueOrPromise<T> storage_;

    friend detail::NurseryBase;
    template <class, class, class...> friend class detail::TryBlock;

    template <class PolicyT> friend class BasicNursery;
    template <class U> friend class detail::ReadyAwaiter;
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
        return Task<U>(std::in_place, U(std::forward<T>(value_)));
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

    operator Task<void>() { return Task<void>(std::in_place, Void{}); }
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
/// This may bypass heap allocation if the value is trivial and small enough
/// to be stored in one machine word (and has a few unused bits).
template <class T> Awaitable<T> auto just(T value) {
    return detail::ReadyAwaiter<T>(std::forward<T>(value));
}

/// Same as above, but for floating point numbers in cases where two
/// least-significant bits of the mantissa can be sacrificed to avoid heap
/// allocation.
template <std::floating_point T> Awaitable<T> auto justApx(T value) {
    if constexpr (sizeof(T) == sizeof(uintptr_t)) {
        return just(std::bit_cast<T>(std::bit_cast<uintptr_t>(value) &
                                     ~detail::ValueOrPromise<T>::Mask));
    } else {
        return just(value);
    }
}

} // namespace corral
