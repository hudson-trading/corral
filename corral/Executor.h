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

#include <cstdint>
#include <iterator>
#include <utility>

#include "config.h"
#include "defs.h"
#include "detail/Queue.h"
#include "detail/ScopeGuard.h"
#include "detail/platform.h"
#include "detail/utility.h"

namespace corral {

namespace detail {

template <class Aw>
concept RootAwaitable = Introspectable<Aw> || Awaiter<Aw>;

}

/// A lightweight executor for coroutine resumptions and other actions
/// that corral might need to defer for a short time.
///
/// The primary reason the executor exists is to enforce the rule, helpful
/// in reasoning about cooperative multitasking, that other tasks can only
/// run during a `co_await` expression. A synchronous function call such as
/// `event.trigger()` might need to wake up tasks that are waiting for
/// the event to occur; those tasks can't run immediately, so they get
/// submitted to the executor in order to run at the next `co_await` point.
/// More precisely, the executor ensures that callbacks submitted to it
/// are not _nested_: something submitted to the executor can run only
/// when nothing else that has been submitted is in the middle of running.
/// (It is possible to defeat this protection using the executor's
/// `capture()` method; see CBPortal for an example of why that might
/// be needed.)
///
/// The executor runs only on demand, and fully drains its list of
/// pending actions every time it runs; it has no concept of scheduling
/// something to run at a time other than "as soon as possible". Anything
/// other than a task step, including checking for I/O and timeouts,
/// can only run when the executor is idle; be careful not to starve
/// your program with a steady flow of ready-to-run tasks.
///
/// There is one executor per root of the corral task tree. That means
/// that in a typical program that uses `corral::run()`, there is only
/// one executor. If you use `UnsafeNursery`, each of those is another
/// executor. Each executor is associated with a particular event
/// loop identity; executors for the same event loop will cooperate
/// to ensure that their callbacks are not nested, providing similar
/// semantics as if only one executor were used, but executors for
/// different event loops are entirely independent. (The independence ensures
/// that you don't get a deadlock if, from the context of a `corral::run()`,
/// you call a synchronous function that happens to internally do another
/// `corral::run()`.)
///
/// The basic pattern used when submitting to the executor is to write
/// `executor->runSoon(<thing>)`. This will run the `<thing>` immediately
/// if there is not another executor callback currently running for the same
/// event loop on this thread, or else will schedule it to run after whatever
/// is running currently. `executor->capture()` exposes an interface
/// to bypass the executor queueing and run a task step synchronously.
class Executor {
    using Task = std::pair<void (*)(void*) noexcept, void*>;
    using Tasks = detail::Queue<Task>;

  public:
    struct Capacity {
        static constexpr const size_t Default = 128;
        static constexpr const size_t Small = 4;
    };

    template <detail::RootAwaitable AwaitableT>
    explicit Executor(EventLoopID eventLoopID,
                      const AwaitableT& rootAwaitable,
                      size_t capacity = Capacity::Default)
      : Executor(eventLoopID, nullptr, capacity) {
        rootAwaitable_ = &rootAwaitable;
        collectTaskTree_ = +[](const void* root,
                               detail::TaskTreeCollector& c) noexcept {
            detail::awaitIntrospect(*static_cast<const AwaitableT*>(root), c);
        };
    }

    explicit Executor(EventLoopID eventLoopID,
                      std::nullptr_t,
                      size_t capacity = Capacity::Default)
      : eventLoopID_(eventLoopID), buffer_(capacity) {}

    template <class EventLoopT>
    explicit Executor(EventLoopT&& eventLoop,
                      const detail::RootAwaitable auto& rootAwaitable,
                      size_t capacity = Capacity::Default)
      : Executor(EventLoopTraits<std::decay_t<EventLoopT>>::eventLoopID(
                         eventLoop),
                 rootAwaitable,
                 capacity) {}

    template <class EventLoopT>
        requires(!std::convertible_to<EventLoopT, EventLoopID>)
    explicit Executor(EventLoopT&& eventLoop,
                      std::nullptr_t,
                      size_t capacity = Capacity::Default)
      : Executor(EventLoopTraits<std::decay_t<EventLoopT>>::eventLoopID(
                         eventLoop),
                 nullptr,
                 capacity) {}

    /// Disallow construction from temporary awaitables.
    explicit Executor(auto&&, const auto&&, size_t = 0) = delete;

    ~Executor() {
        if (scheduled_) {
            // We had a call pending from another executor. Based on the
            // invariant that we can't be destroyed with our own callbacks
            // scheduled, we must not need that call anymore; the most
            // likely way to hit this is by destroying an UnsafeNursery
            // inside an async task, which deals with the pending callbacks
            // using Executor::drain().
            scheduled_->buffer_.for_each_item([&](Task& task) {
                if (task.second == this) {
                    task.first = +[](void*) noexcept {};
                    task.second = nullptr;
                }
            });
        }

        CORRAL_ASSERT(buffer_.empty());
        if (running_ != nullptr) {
            *running_ = false;
        }
        if (current() == this) {
            current() = nullptr;
        }
    }

    /// Arranges `fn(arg)` to be called on the next executor loop,
    /// then runs the executor loop unless already running
    /// (see below for details).
    template <class T> void runSoon(void (*fn)(T*) noexcept, T* arg) {
        schedule(fn, arg);
        runSoon();
    }

    /// Runs executor loop until it's empty.
    /// This function is reenterant.
    void drain() { drain(*ready_); }

    /// Arranges `fn(arg)` to be called on the next executor loop,
    /// but does *not* run the executor loop.
    ///
    /// The caller should arrange runSoon() to be called at some point
    /// in the future, or else the callback will never be called.
    ///
    /// This is an advanced interface that should be used with care.
    template <class T> void schedule(void (*fn)(T*) noexcept, T* arg) {
        ready_->emplace_back(reinterpret_cast<void (*)(void*) noexcept>(fn),
                             arg);
    }

    /// Runs executor loop.
    /// If called from within the loop, does nothing; if called from another
    /// executor's loop, schedules this executor's event loop to be run
    /// from current executor (therefore not introducing an interruption point).
    ///
    /// NB: this will continue running executor until its run queue empties,
    ///     and only then would return so I/O checks can be made.
    ///     Pay attention not to starve your program with a steady flow
    ///     of ready tasks.
    void runSoon() noexcept {
        if (running_ != nullptr || scheduled_ != nullptr) {
            // Do nothing; our callbacks are already slated to run soon
        } else if (current() != nullptr &&
                   current()->eventLoopID_ == eventLoopID_) {
            // Schedule the current executor to run our callbacks
            scheduled_ = current();
            scheduled_->runSoon(
                    +[](Executor* ex) noexcept { ex->runOnce(); }, this);
        } else {
            // No current executor, or it's for a different event loop:
            // run our callbacks immediately
            runOnce();
        }
    }

    /// Runs the function, temporarily capturing any tasks scheduled into a
    /// separate list. Then runs everything in the list.
    /// NB: tasks scheduled as a result of executing the capture list
    /// go into previously used list.
    template <class Fn> void capture(Fn fn, size_t capacity = Capacity::Small) {
        Tasks tmp{capacity};
        detail::ScopeGuard guard([&] { drain(tmp); });

        Tasks* oldReady = ready_;
        ready_ = &tmp;
        detail::ScopeGuard guard2([&] { ready_ = oldReady; });

        fn();
    }

    void collectTaskTree(detail::TaskTreeCollector& c) const noexcept {
        if (collectTaskTree_) {
            collectTaskTree_(rootAwaitable_, c);
        }
    }

    /// Save a `std::coroutine_handle` for the task that is currently running
    /// on the CPU.
    ///
    /// This is necessary for `collectAsyncStackTrace`. The task will be
    /// automatically deactivated when the returned guard is destroyed.
    [[nodiscard]] auto markActive(Handle h) noexcept {
        // We occasionally enter the Executor recursively, for example, through
        // CBPortal. We stash the currently active coroutine in case we are
        // recursively entering the Executor.
        Handle prev = std::exchange(active_, h);

        // Can call only while in drain().
        bool* running = running_;
        CORRAL_ASSERT(running != nullptr);

        return detail::ScopeGuard([this, h, prev, running]() {
            // Silence unused lambda capture warnings for h if CORRAL_ASSERT is
            // compiled away. Unfortunately, [[maybe_unused]] cannot be used on
            // a lambda capture.
            (void) h;

            if (!*running) {
                // The executor could have been destroyed by the time this guard
                // runs. drain() guards against this possibility, so we can do
                // the same here.
                return;
            }

            CORRAL_ASSERT(active_ == h);
            active_ = prev;
        });
    }

    template <std::output_iterator<uintptr_t> OutIter>
    static OutIter collectAsyncStackTrace(OutIter out) noexcept {
        Executor* executor = current();
        return executor ? detail::collectAsyncStackTrace(executor->active_, out)
                        : out;
    }

  private:
    void runOnce() noexcept {
        scheduled_ = nullptr;
        if (running_ == nullptr) {
            CORRAL_TRACE("--running executor--");
            drain();
            CORRAL_TRACE("--executor done--");
        }
    }

    void drain(Tasks& tasks) noexcept {
        Executor* prev = current();
        current() = this;
        detail::ScopeGuard guard([&] { current() = prev; });

        bool b = true;
        if (running_ == nullptr) {
            running_ = &b;
        }
        bool* running = running_;
        detail::ScopeGuard guard2([&] {
            if (*running && running_ == &b) {
                running_ = nullptr;
            }
        });

        while (*running && !tasks.empty()) {
            auto [fn, arg] = tasks.front();
            tasks.pop_front();
            fn(arg);
        }
    }

    static Executor*& current() noexcept {
        // NB: Executor::current() is the executor that is currently
        // running (i.e., is inside a call to drain()), and will be
        // nullptr if all tasks are blocked waiting for I/O. It's
        // used to avoid nesting executor callbacks, and is also
        // useful for collecting traces from the current location.
        // For introspection from outside a currently-running async
        // task, use corral::currentRunExecutor() instead; it stays
        // consistent until the corral::run() call returns.
        CORRAL_THREAD_LOCAL Executor* ex = nullptr;
        return ex;
    }

  private:
    EventLoopID eventLoopID_;
    Tasks buffer_;
    Handle active_;

    /// Currently used list of ready tasks. Normally points to buffer_,
    /// but can be temporarily replaced from capture().
    Tasks* ready_ = &buffer_;

    bool* running_ = nullptr;

    /// Stores the pointer to an outer executor on which we've scheduled
    /// our runOnce() to run soon, or nullptr if not scheduled.
    Executor* scheduled_ = nullptr;

    const void* rootAwaitable_ = nullptr;
    void (*collectTaskTree_)(const void* root,
                             detail::TaskTreeCollector&) noexcept = nullptr;

    using UniverseGuard = decltype(CORRAL_ENTER_ASYNC_UNIVERSE);
    CORRAL_NO_UNIQUE_ADDR CORRAL_UNUSED_MEMBER UniverseGuard universeGuard_ =
            CORRAL_ENTER_ASYNC_UNIVERSE;
};

namespace detail {
class GetExecutor {
    Executor* executor_ = nullptr;

  public:
    void await_set_executor(Executor* executor) noexcept {
        executor_ = executor;
    }
    bool await_ready() const noexcept { return executor_ != nullptr; }
    Handle await_suspend(Handle h) { return h; }
    Executor* await_resume() { return executor_; }
};

template <std::output_iterator<TreeDumpElement> OutIt> class CollectTreeImpl {
    Executor* executor_ = nullptr;
    OutIt out_;

  public:
    explicit CollectTreeImpl(OutIt out) : out_(out) {}

    void await_set_executor(Executor* executor) noexcept {
        executor_ = executor;
    }
    bool await_ready() const noexcept { return false; }

    Handle await_suspend(Handle h) {
        TaskTreeCollector c(
                +[](void* pthis, TreeDumpElement elt) {
                    auto& out = static_cast<CollectTreeImpl*>(pthis)->out_;
                    *out++ = std::move(elt);
                },
                this);
        executor_->collectTaskTree(c);
        return h;
    }

    OutIt await_resume() { return out_; }

    void await_introspect(TaskTreeCollector& c) const noexcept {
        c.node("<YOU ARE HERE>");
    }
};

} // namespace detail

/// Returns an awaitable that you can use in an async context to get the
/// currently-running executor.
inline Awaitable<Executor*> auto getExecutor() {
    return detail::GetExecutor{};
}

/// Obtains the current task tree.
template <std::output_iterator<TreeDumpElement> OutIt>
Awaitable<OutIt> auto dumpTaskTree(OutIt out) {
    return detail::CollectTreeImpl<OutIt>(out);
}

/// Obtains an async stack trace, i.e., chain of tasks co_await'ing on
/// each other. Successive elements are written to `*out++`.
///
/// Like a regular stack trace, this produces a list of instruction
/// pointer/program counter values, innermost first. You can
/// use a tool such as backtrace_symbols() or addr2line to convert them
/// to human-readable information such as function names or file/line
/// numbers. The first element in the resulting list will be the
/// most recent `co_await` statement in the task that's currently
/// executing on the CPU, and the last will be the original call to
/// `corral::run()`.
template <std::output_iterator<uintptr_t> OutIt>
OutIt collectAsyncStackTrace(OutIt out) {
    return Executor::collectAsyncStackTrace(out);
}

} // namespace corral
