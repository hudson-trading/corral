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

#include <functional>
#include <memory>
#include <utility>

#include "concepts.h"
#include "utility.h"

namespace corral {

namespace detail {
template <class CancelToken, class F, class... Args> class ThreadPoolTaskBase;
} // namespace detail


/// A degenerate thread pool, always running tasks inline on the calling thread.
/// API-compatible with ThreadPool.
/// May be useful in cases where the same piece of code needs to compile with
/// or without threading support, in which case one could
///
///     #ifdef HAVE_THREADING
///     using MyThreadPool = corral::ThreadPool;
///     #else
///     using MyThreadPool = corral::DegenerateThreadPool;
///     #endif
class DegenerateThreadPool {
    template <class Fn, class... Args> class Task;

  public:
    struct CancelToken {
        explicit operator bool() noexcept { return false; }
        bool peek() const noexcept { return false; }
        void consume() noexcept {}
    };

    DegenerateThreadPool() = default;

    template <class F, class... Args>
        requires(std::invocable<F, Args...> ||
                 std::invocable<F, Args..., DegenerateThreadPool::CancelToken>)
    Awaitable auto run(F&& f, Args&&... args);

    /// API-compatible constructor.
    ///
    /// The second argument allows passing literal 0 for thread count,
    /// so `MyThreadPool pool(eventLoop, 0)` will compile and run tasks
    /// inline regardless of whether it's aliased to ThreadPool
    /// or DegenerateThreadPool.
    template <class EventLoopT>
    DegenerateThreadPool(EventLoopT&, std::nullptr_t /*threadCount*/)
      : DegenerateThreadPool() {}

    auto isDegenerate() const noexcept { return std::true_type{}; }
};


//
// Implementation
//


namespace detail {

// TaskBase

template <class CancelToken, class F, class... Args> class ThreadPoolTaskBase {
    static decltype(auto) runHelper(F&& f,
                                    std::tuple<Args...>&& argTuple,
                                    CancelToken tok) {
        return std::apply(
                [&f, tok](Args&&... args) -> decltype(auto) {
                    if constexpr (std::is_invocable_v<F, Args...,
                                                      CancelToken>) {
                        return std::invoke(std::forward<F>(f),
                                           std::forward<Args>(args)..., tok);
                    } else {
                        (void) tok; // no [[maybe_unused]] in lambda captures
                        return std::invoke(std::forward<F>(f),
                                           std::forward<Args>(args)...);
                    }
                },
                std::move(argTuple));
    }

    using Ret = decltype(runHelper(std::declval<F>(),
                                   std::declval<std::tuple<Args...>>(),
                                   std::declval<CancelToken>()));

  public:
    ThreadPoolTaskBase(F f, Args... args)
      : f_(std::forward<F>(f)), args_(std::forward<Args>(args)...) {}

    bool await_ready() const noexcept { return false; }

    Ret await_resume() && { return std::move(result_).value(); }

  protected:
    void doRun(CancelToken cancelToken) {
#if __cpp_exceptions
        try {
#endif
            if constexpr (std::is_same_v<Ret, void>) {
                runHelper(std::forward<F>(f_), std::move(args_),
                          std::move(cancelToken));
                result_.storeValue(detail::Void{});
            } else {
                result_.storeValue(runHelper(std::forward<F>(f_),
                                             std::move(args_),
                                             std::move(cancelToken)));
            }
#if __cpp_exceptions
        } catch (...) { result_.storeException(); }
#endif
    }

  private:
    F f_;
    std::tuple<Args...> args_;
    detail::Result<Ret> result_;
};

} // namespace detail


//
// DegenerateThreadPool
//

template <class F, class... Args>
class DegenerateThreadPool::Task
  : public detail::ThreadPoolTaskBase<CancelToken, F, Args...> {
  public:
    using Task::ThreadPoolTaskBase::ThreadPoolTaskBase;

    bool await_suspend(Handle h) {
        this->doRun(CancelToken());
        return false;
    }
};

template <class F, class... Args>
    requires(std::invocable<F, Args...> ||
             std::invocable<F, Args..., DegenerateThreadPool::CancelToken>)
Awaitable auto DegenerateThreadPool::run(F&& f, Args&&... args) {
    return makeAwaitable<Task<F, Args...>, F, Args...>(
            std::forward<F>(f), std::forward<Args>(args)...);
}

} // namespace corral
