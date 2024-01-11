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

#include <cstdint>
#include <iterator>

#include "config.h"
#include "defs.h"
#include "detail/utility.h"

namespace corral {

/// Similar to std::noop_coroutine(), but guaranteed to return the same value
/// for each invocation, so can be compared against.
inline Handle noopHandle() {
    static const Handle ret = std::noop_coroutine();
    return ret;
}

/// Return an awaitable which runs the given callable and then resumes the
/// caller immediately, evaluating to the result thereof.
template <class Callable> auto yieldToRun(Callable cb) {
    return detail::Yield<Callable>(std::move(cb));
}


/// An awaitable similar to std::suspend_always, but with cancellation support.
class SuspendForever {
  public:
    bool await_ready() const noexcept { return false; }
    void await_suspend(Handle) {}
    [[noreturn]] void await_resume() { detail::unreachable(); }
    auto await_cancel(Handle h) noexcept { return std::true_type{}; }
    void await_introspect(auto& c) const noexcept { c.node("SuspendForever"); }
};
static constexpr const detail::CoAwaitFactory<SuspendForever> suspendForever;


/// An awaitable which immediately reschedules the current task.
/// May be used to force execution of anything already scheduled, and
/// also serves as a cancellation point.
class Yield {
  public:
    bool await_ready() const noexcept { return false; }

#ifdef __clang__
    // clang-16 manages to optimize away this function entirely
    // in optimized builds.
    void await_suspend(Handle h) __attribute__((noinline)) { h.resume(); }
#else
    Handle await_suspend(Handle h) { return h; }
#endif

    void await_resume() {}
    auto await_cancel(Handle) noexcept { return std::true_type{}; }
    void await_introspect(auto& c) const noexcept { c.node("Yield"); }
};
static constexpr const detail::CoAwaitFactory<Yield> yield;

/// An awaitable which constructs an async stack trace when awaited.
//
/// Normal stack frames will not be included in the stack trace. The first
/// address will be the return address of the co_await'ing task, and later
/// addresses correspond to progressively higher callers.
///
/// Usage:
///
///    std::vector<uintptr_t> stack;
///    co_await corral::AsyncStackTrace(std::back_inserter(stack));
template <std::output_iterator<uintptr_t> OutIter> class AsyncStackTrace {
  public:
    explicit AsyncStackTrace(OutIter out) : out_(out) {}

    bool await_ready() const noexcept { return false; }

    Handle await_suspend(Handle h) {
        out_ = detail::collectAsyncStackTrace(h, out_);
        return h;
    }

    OutIter await_resume() { return out_; }

  private:
    OutIter out_;
};

/// A wrapper around an awaitable suppressing its cancellation:
///
///    // If this line gets executed...
///    co_await noncancellable([]() -> Task<> {
///        co_await sleep(10_s);
///        // ... this line is guaranteed to get executed as well
///        //     (assuming sleep doesn't throw an exception)
///    });
///    // ... and so is this one
template <class Awaitable> auto noncancellable(Awaitable&& awaitable) {
    return detail::CancellableAdapter<detail::AwaitableType<Awaitable>>(
            detail::getAwaitable(std::forward<Awaitable>(awaitable)));
}

/// A wrapper around an awaitable declaring that its return value
/// is safe to dispose of upon cancellation.
/// May be used on third party awaitables which don't know about
/// corral's async cancellation mechanism. Note that this won't
/// result in the awaitable completing any faster when cancelled;
/// it only affects what happens _after the awaitable completes_
/// when a cancellation has been requested.
template <class Awaitable> auto disposable(Awaitable&& awaitable) {
    return detail::DisposableAdapter<detail::AwaitableType<Awaitable>>(
            detail::getAwaitable(std::forward<Awaitable>(awaitable)));
}

/// A wrapper that adapts an awaitable so it runs upon cancellation
/// instead of immediately. This is the primary suggested method for
/// implementing asynchronous cleanup logic, since C++ doesn't support
/// destructors that are coroutines.
///
/// The awaitable will be started when cancellation is requested, and
/// cancellation will be confirmed when the awaitable completes. The
/// awaitable will not itself execute in a cancelled context -- you can
/// do blocking operations normally -- although it is allowed to use
/// cancellation internally. Consider attaching a timeout if you need
/// to do something that might get stuck.
///
/// This can be used as an async equivalent of a scope guard, to attach
/// async cleanup logic to an async operation:
///
///    co_await anyOf(
///        doSomething(),
///        untilCancelledAnd([]() -> corral::Task<> {
///            co_await doAsyncCleanup();
///        ));
///
/// Make sure not to throw any exceptions from the awaitable, as they
/// will terminate() the process.
template <Awaitable<void> Aw> auto untilCancelledAnd(Aw&& awaitable) {
    return detail::RunOnCancel<detail::AwaitableType<Aw>>(
            detail::getAwaitable(std::forward<Aw>(awaitable)));
}

/// A helper macro useful in nursery bodies.
/// Clang and MSVC currently fail to pay attention to [[noreturn]]
/// of SuspendForever::await_resume(), so have it followed by an unreachable.
#define CORRAL_SUSPEND_FOREVER()                                               \
    (co_await ::corral::SuspendForever{}, ::corral::detail::unreachable())

} // namespace corral
