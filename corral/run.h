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
#include "Executor.h"
#include "detail/frames.h"
#include "detail/platform.h"

namespace corral {

inline Executor* currentRunExecutor(); // defined below

namespace detail {

class RunnerTracking {
    template <class EventLoop> friend class Runner;
    friend Executor* corral::currentRunExecutor();

  private:
    static Executor*& currentExecutor() {
        CORRAL_THREAD_LOCAL Executor* ex = nullptr;
        return ex;
    }
};

template <class EventLoop>
class Runner : private RunnerTracking, private TaskFrame {
  public:
    explicit Runner(EventLoop& eventLoop) : eventLoop_(&eventLoop) {}

    template <class Awaitable>
    CORRAL_NOINLINE decltype(auto) run(Awaitable&& awaitable) && {
        SanitizedAwaiter<Awaitable&&> awaiter(
                std::forward<Awaitable>(awaitable));
        using ELTraits = EventLoopTraits<std::decay_t<EventLoop>>;
        if constexpr (requires { ELTraits::isRunning(*eventLoop_); }) {
            CORRAL_ASSERT(!ELTraits::isRunning(*eventLoop_));
        }

        Executor executor(*eventLoop_, awaiter);
        executor_ = &executor;
        Executor* prevExec =
                std::exchange(RunnerTracking::currentExecutor(), &executor);
        ScopeGuard guard([this, prevExec] {
            executor_ = nullptr;
            RunnerTracking::currentExecutor() = prevExec;
        });

        if (!awaiter.await_ready()) {
            CoroutineFrame::resumeFn = +[](CoroutineFrame* frame) {
                auto runner = static_cast<Runner*>(frame);
                runner->executor_->runSoon([runner]() noexcept {
                    EventLoop* eventLoop =
                            std::exchange(runner->eventLoop_, nullptr);
                    ELTraits::stop(*eventLoop);
                });
            };
            TaskFrame::pc =
                    reinterpret_cast<uintptr_t>(CORRAL_RETURN_ADDRESS());
            awaiter.await_set_executor(&executor);
            awaiter.await_suspend(CoroutineFrame::toHandle()).resume();
            executor.runSoon();
            if (eventLoop_) {
                ELTraits::run(*eventLoop_);
            }
            if (eventLoop_) {
                // The event loop was stopped before the awaitable completed.
                // Run the executor one more time in case that's enough.
                executor.drain();
            }
            if (eventLoop_) [[unlikely]] {
                // Nope, still running, so it must be waiting on some
                // I/O that's not going to happen anymore. Fail.
                // Do our best to clean up if there is a custom implementation
                // of FAIL_FOR_DANGLING_TASKS that throws an exception.
                ScopeGuard cleanupGuard([&] {
                    if (awaiter.await_cancel(CoroutineFrame::toHandle())) {
                        // cancelled immediately
                        return;
                    }
                    executor.drain();
                    if (!eventLoop_) {
                        // cancelled after a spin of the executor
                        return;
                    }
                    // failed to cancel -- we already know something is wrong,
                    // avoid follow-on errors that obscure the original issue
                    awaiter.abandon();
                });
                CORRAL_FAIL_FOR_DANGLING_TASKS(
                        "Event loop stopped before the awaitable passed to "
                        "corral::run() completed",
                        awaiter);
                // We don't have anything to return below, so if the
                // above failure allowed execution to proceed, we must:
                std::terminate();
            }
        }

        return awaiter.await_resume();
    }

  private:
    EventLoop* eventLoop_;
    Executor* executor_ = nullptr;
};
} // namespace detail


/// Run a task or other awaitable from non-async context, using the
/// given event loop (which must not already be running). This is the
/// entry point into corral for your application.
template <class EventLoop, class Awaitable>
CORRAL_NOINLINE decltype(auto) run(EventLoop& eventLoop,
                                   Awaitable&& awaitable) {
    return detail::Runner(eventLoop).run(std::forward<Awaitable>(awaitable));
}

/// Returns a pointer to the executor associated with the current
/// `corral::run()`. Example use case: collecting the task tree from
/// non-async context (signal handler, etc).
inline Executor* currentRunExecutor() {
    return detail::RunnerTracking::currentExecutor();
}

} // namespace corral
