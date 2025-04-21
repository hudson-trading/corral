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

// Note: this file is not included in <corral/corral.h>, to allow using
// the rest of the library in environments where threads are unavailable.
// #include it explicitly if you need it.

#include <atomic>
#include <condition_variable>
#include <memory>
#include <numeric>
#include <thread>
#include <utility>

#include "concepts.h"
#include "detail/IntrusivePtr.h"
#include "detail/platform.h"
#include "detail/utility.h"

namespace corral {

/// A thread pool, for running CPU-bound tasks asynchronously.
///
/// All public methods of ThreadPool (constructor, destructor, and `run()`)
/// must be called from the same thread in which the event loop passed to the
/// constructor is running.
class ThreadPool : public detail::Noncopyable {
    enum CancelState : uint8_t;

  public:
    /// A representation of the cancellation status of a call to
    /// `ThreadPool.run()`, that can be tested from within the task that's
    /// running in the thread pool.
    class CancelToken {
        std::atomic<CancelState>* state_;
        friend class ThreadPool;

      public:
        /// Test whether cancellation has been requested. Once `true` is
        /// returned here, the cancellation is considered to have been taken:
        /// the corresponding call to `ThreadPool.run()` will terminate by
        /// propagating cancellation, and any value or error returned from
        /// the task will be ignored. So, don't check for cancellation until
        /// you're prepared to act on it. You may check for cancellation from
        /// any thread, as long as all accesses to the CancelToken are
        /// sequenced-before the task that received it completes.
        explicit operator bool() noexcept;

        /// Allows to query for the cancellation status, but not mark
        /// the cancellation taken.
        bool peek() const noexcept;

        /// Marks the cancellation as taken.
        /// No-op if the cancellation was not requested or already consumed.
        void consume() noexcept;
    };

    /// Constructor.
    /// Requires a specialization of `corral::ThreadNotification`
    /// to be defined for the event loop (see corral/defs.h for details).
    template <class EventLoopT>
    ThreadPool(EventLoopT& eventLoop, unsigned threadCount);

    /// Shuts down the thread pool.
    /// UB if there are any pending or in-progress tasks.
    ~ThreadPool();

    /// Submits the task to the pool, and suspends the current coroutine
    /// until the task completes, delivering the result or reraising any
    /// exceptions.
    ///
    /// `fn` may optionally accept a `CancelToken` as its last argument
    /// to periodically check if cancellation of the calling coroutine has been
    /// requested, and wrap up early if so. Querying the token
    /// for the cancellation status counts as confirming the cancellation
    /// request; any returned value (or exception) will be discarded.
    ///
    /// Any references passed as `args` are *not* decay-copied, which is fine
    /// in typical use cases (`co_await threadPool.run(fn, args...)` -- iow,
    /// having run() and co_await in the same full expression).
    /// However, if returning an unawaited task from a function
    /// (e.g. `Awaitable auto foo() { return threadPool.run(fn, args...); }`),
    /// pay attention to the lifetime of the arguments.
    template <class F, class... Args>
        requires(std::invocable<F, Args...> ||
                 std::invocable<F, Args..., ThreadPool::CancelToken>)
    Awaitable auto run(F&& f, Args&&... args);


  private:
    class Task;
    template <class F, class... Args> class TaskImpl;

    struct IThreadNotification;
    template <class EventLoopT> struct ThreadNotificationImpl;

    // Main function of each worker thread.
    void threadBody();

    // Runs in main thread to trampoline to `processCQ()`. `arg` is this
    // ThreadPool.
    static void tick(void* arg);

    // Runs in main thread to post a new task, which can be immediately serviced
    // by a worker thread. Returns false if there was no room.
    bool pushToSQ(Task* task);

    // Runs in a worker thread to obtain a task to run, blocking if necessary.
    // Never returns null, but may return ExitRequest rather than a valid task
    // if the worker thread should terminate.
    Task* popFromSQ();

    // Runs in main thread to record that a new task should be posted once some
    // already-posted tasks have completed to free up submission queue space.
    void pushToBackoffSQ(Task* task);

    // Runs in main thread to call pushToSQ() for one or more tasks previously
    // passed to pushToBackoffSQ().
    void submitBackoffSQ();

    // Runs in worker thread to post the given task as completed so the main
    // thread can pick it up.
    void pushToCQ(Task* task);

    // Runs in main thread to process task completions and wake up their
    // awaiters.
    void drainCQ();

  private:
    static constexpr const uintptr_t ExitRequest = 1;
    static constexpr const size_t Stride = 7;

    struct Slot {
        // Task to run; nullptr means this slot is free.
        //
        // Main thread checks for null using acquire ordering and then writes
        // a non-null value using release ordering. Worker thread reads the
        // value using acquire ordering.
        std::atomic<Task*> task{nullptr};

        // If true, there is a worker thread waiting to service a task in this
        // slot, so it should be woken up once a task has been written.
        // Worker thread writes true/false using release ordering. Main thread
        // checks the value using acquire ordering.
        std::atomic<bool> dequeuing;
    };

    struct Data : detail::RefCounted<Data> {
        // List of worker threads. Only accessed by main thread.
        std::vector<std::thread> threads;

        // Submission queue entries. Each worker thread claims a particular
        // entry using `sqHead.fetch_add(Stride)`, then waits for that entry
        // to be populated.
        std::unique_ptr<Slot[]> sq;

        // Index of the first slot that has not been claimed by a worker thread
        // yet. All accesses use fetch_add() to ensure exactly one thread claims
        // each index. Ever-increasing; each thread individually wraps the
        // claimed index around `sqCapacity` to obtain the actual index
        // into `sq`.
        std::atomic<size_t> sqHead = 0;

        // Index of the first slot that has not been written yet. Accessed only
        // from the main thread. Ever-increasing for uniformity with `sqHead`.
        size_t sqTail = 0;

        // Number of slots in `sq`. Read-only after construction, so doesn't
        // need to be atomic.
        size_t sqCapacity;

        // Number of times a worker thread should spin seeking a new task to run
        // before going to sleep. Adjusted adaptively by worker threads using
        // relaxed ordering.
        std::atomic<uint32_t> sqSpinCutoff = 200;

        // Head and tail of a linked list (linked via Task::next_) of tasks that
        // have not yet been submitted due to lack of room. Accessed only from
        // main thread.
        Task* backoffSqHead = nullptr;
        Task** backoffSqTailPtr = &backoffSqHead;

        // Head of a linked list (linked via Task::next_) of tasks that have
        // been completed. Worker threads prepend new entries; the main thread
        // takes the whole batch and then processes them.
        std::atomic<Task*> cqHead{nullptr};

        // Interface allowing worker threads to enqueue a callback that will
        // run on the main thread. The callback calls `tick()` on the
        // ThreadPool.
        std::unique_ptr<IThreadNotification> notification;
    };
    detail::IntrusivePtr<Data> d;
};


//
// Implementation
//


// Task

enum ThreadPool::CancelState : uint8_t { None, Requested, Confirmed };

inline ThreadPool::CancelToken::operator bool() noexcept {
    CancelState st = CancelState::Requested;
    return state_->compare_exchange_strong(st, CancelState::Confirmed,
                                           std::memory_order_acq_rel) ||
           st == CancelState::Confirmed;
}

inline bool ThreadPool::CancelToken::peek() const noexcept {
    CancelState st = state_->load(std::memory_order_acquire);
    return st == CancelState::Requested || st == CancelState::Confirmed;
}

inline void ThreadPool::CancelToken::consume() noexcept {
    CancelState st = CancelState::Requested;
    state_->compare_exchange_strong(st, CancelState::Confirmed,
                                    std::memory_order_release);
}

class ThreadPool::Task {
    explicit Task(ThreadPool* pool) : pool_(pool) {}

  private:
    virtual void run() = 0;

  protected:
    Handle parent_;

  private:
    ThreadPool* pool_;
    Task* next_ = nullptr;
    friend class ThreadPool;
};

template <class F, class... Args>
class ThreadPool::TaskImpl final : public Task {
    static decltype(auto) doRun(F&& f,
                                std::tuple<Args...>&& argTuple,
                                CancelToken tok) {
        return std::apply(
                [&f, tok](Args&&... args) {
                    if constexpr (std::is_invocable_v<F, Args...,
                                                      CancelToken>) {
                        return std::forward<F>(f)(std::forward<Args>(args)...,
                                                  tok);
                    } else {
                        (void) tok; // no [[maybe_unused]] in lambda captures
                        return std::forward<F>(f)(std::forward<Args>(args)...);
                    }
                },
                std::move(argTuple));
    }
    using Ret = decltype(doRun(std::declval<F>(),
                               std::declval<std::tuple<Args...>>(),
                               std::declval<CancelToken>()));

  public:
    TaskImpl(ThreadPool* pool, F f, Args... args)
      : Task(pool),
        f_(std::forward<F>(f)),
        args_(std::forward<Args>(args)...) {}

    bool await_ready() const noexcept { return false; }
    void await_suspend(Handle h) {
        parent_ = h;
        if (!pool_->pushToSQ(this)) {
            // No space in the submission queue; stash to the local queue
            // to submit later.
            pool_->pushToBackoffSQ(this);
        }
    }
    bool await_cancel(Handle) noexcept {
        cancelState_.store(CancelState::Requested, std::memory_order_release);
        return false;
    }
    bool await_must_resume() const noexcept {
        return cancelState_.load(std::memory_order_acquire) !=
               CancelState::Confirmed;
    }
    Ret await_resume() && { return std::move(result_).value(); }

    void run() override {
        CancelToken token;
        token.state_ = &cancelState_;

#if __cpp_exceptions
        try {
#endif
            if constexpr (std::is_same_v<Ret, void>) {
                doRun(std::forward<F>(f_), std::move(args_), std::move(token));
                result_.storeValue(detail::Void{});
            } else {
                result_.storeValue(doRun(std::forward<F>(f_), std::move(args_),
                                         std::move(token)));
            }
#if __cpp_exceptions
        } catch (...) { result_.storeException(); }
#endif
    }

  private:
    F f_;
    std::tuple<Args...> args_;
    detail::Result<Ret> result_;
    std::atomic<CancelState> cancelState_;
};

template <class F, class... Args>
    requires(std::invocable<F, Args...> ||
             std::invocable<F, Args..., ThreadPool::CancelToken>)
Awaitable auto ThreadPool::run(F&& f, Args&&... args) {
    return makeAwaitable<TaskImpl<F, Args...>, ThreadPool*, F, Args...>(
            this, std::forward<F>(f), std::forward<Args>(args)...);
}


// Type-erased ThreadNotification

namespace detail {
template <class T, class EventLoopT>
concept ValidThreadNotification =
        requires(EventLoopT& evtloop, T& n, void (*fn)(void*), void* arg) {
            { T(evtloop, fn, arg) };
            { n.post(evtloop, fn, arg) } noexcept -> std::same_as<void>;
        };
} // namespace detail

struct ThreadPool::IThreadNotification : detail::Noncopyable {
    virtual ~IThreadNotification() = default;
    virtual void post(void (*fn)(void*), void* arg) noexcept = 0;
};

template <class EventLoopT>
struct ThreadPool::ThreadNotificationImpl : ThreadPool::IThreadNotification {
    ThreadNotificationImpl(EventLoopT& loop, void (*fn)(void*), void* arg)
      : eventLoop_(loop), impl_(loop, fn, arg) {
        // Validate against the above concept to produce a meaningful
        // compile error upon any mismatches
        validateThreadNotification(impl_);
    }

    void post(void (*fn)(void*), void* arg) noexcept override {
        impl_.post(eventLoop_, fn, arg);
    }

  private:
    void validateThreadNotification(
            detail::ValidThreadNotification<EventLoopT> auto&) {}

  private:
    EventLoopT& eventLoop_;
    [[no_unique_address]] corral::ThreadNotification<EventLoopT> impl_;
};


// Public API

template <class EventLoopT>
inline ThreadPool::ThreadPool(EventLoopT& eventLoop, unsigned threadCount)
  : d(new Data) {
    // Round up the capacity to be a power of 2, so it'll be mutually prime
    // with Stride (see below).
    d->sqCapacity = std::bit_ceil(threadCount) * 512;
    d->sq.reset(new Slot[d->sqCapacity]);
    d->notification = std::make_unique<ThreadNotificationImpl<EventLoopT>>(
            eventLoop, &ThreadPool::tick, this);

    d->threads.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
        d->threads.emplace_back([this] { threadBody(); });
    }
}

inline ThreadPool::~ThreadPool() {
    CORRAL_ASSERT(d->backoffSqHead == nullptr);

    for (size_t i = 0; i < d->threads.size(); ++i) {
        bool ret [[maybe_unused]] =
                pushToSQ(reinterpret_cast<Task*>(ExitRequest));
        CORRAL_ASSERT(ret);
    }
    for (auto& t : d->threads) {
        t.join();
    }

    CORRAL_ASSERT(d->sqHead == d->sqTail);
    CORRAL_ASSERT(d->cqHead == nullptr);
}

inline void ThreadPool::threadBody() {
    while (true) {
        Task* t = popFromSQ();
        if (t == reinterpret_cast<Task*>(ExitRequest)) {
            break;
        }
        t->run();
        pushToCQ(t);
    }
}

/*static*/ inline void ThreadPool::tick(void* arg) {
    auto self = static_cast<ThreadPool*>(arg);

    // Grab a reference to member variables (see below)
    detail::IntrusivePtr<Data> d = self->d;

    self->drainCQ(); // Note: this may destroy the ThreadPool

    if (d->backoffSqHead) {
        // If we got here, the ThreadPool must not have been deleted yet
        // (since the non-empty backoffSQ implies that some task is still
        // blocked in run()).
        // Hopefully worker thread consumed a few tasks from
        // the submission queue, so submit more tasks from the backoff
        // queue if possible.
        self->submitBackoffSQ();
    }
}


// SUBMISSION QUEUE
// ----------------
// This is essentially a shamelessly borrowed folly::MPMCQueue,
// dramatically simplified for the ThreadPool's needs (SPMC, fixed-capacity,
// only non-blocking writes and blocking reads).
//
// The queue is implemented as a circular buffer of single-element SPSC queues,
// ("slots"), with ever-increasing head and tail indices.
//
// Each slot can be in one of three states:
//    - empty (task == nullptr, dequeuing == false);
//    - empty with a blocked reader (task == nullptr, dequeuing == true,
//                                   task.wait() has been called);
//    - inhabited (task != nullptr, dequeuing == false).
//
// popFromSQ() advances the head, and tries to dequeue the task from the slot,
// suspending on Slot::task if necessary. Note that permits the queue head
// to go beyond the tail, and does not allow SQ size to be smaller than
// the number of threads.
//
// Stride (hardcoded to 7) is used to prevent false sharing between
// worker threads dequeuing from adjacent slots. Stride needs to be mutually
// prime with the queue capacity (to make sure all slots are used),
// so the capacity is rounded up to a power of 2.

inline bool ThreadPool::pushToSQ(ThreadPool::Task* task) {
    Slot& slot = d->sq[d->sqTail % d->sqCapacity];

    Task* prev = nullptr;
    if (!slot.task.compare_exchange_strong(prev, task,
                                           std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
        return false;
    }
    if (slot.dequeuing.load(std::memory_order_acquire)) {
        slot.task.notify_one();
    }
    d->sqTail += Stride;
    return true;
}

inline ThreadPool::Task* ThreadPool::popFromSQ() {
    size_t head = d->sqHead.fetch_add(Stride, std::memory_order_relaxed);
    Slot& slot = d->sq[head % d->sqCapacity];
    while (true) {
        Task* ret = slot.task.exchange(nullptr, std::memory_order_acquire);
        if (ret) {
            return ret;
        }

        // No task available yet; do some spin-waiting to save on syscalls.
        // In ~1% cases, spin longer and adjust the adaptive cutoff.
        bool updateCutoff = (head % 128 == 0);
        size_t cutoff =
                (updateCutoff
                         ? 20000
                         : d->sqSpinCutoff.load(std::memory_order_relaxed));
        size_t spins = 0;
        while ((ret = slot.task.exchange(nullptr, std::memory_order_acquire)) ==
                       nullptr &&
               ++spins < cutoff) {
            detail::spinLoopBody();
        }
        if (ret && updateCutoff) {
            d->sqSpinCutoff.store(std::max<size_t>(200, spins * 1.25),
                                  std::memory_order_relaxed);
        }

        if (ret) {
            return ret;
        }

        // Still no task available; suspend the thread.
        slot.dequeuing.store(true, std::memory_order_release);
        slot.task.wait(nullptr, std::memory_order_acquire);
        slot.dequeuing.store(false, std::memory_order_release);
    }
}


// BACKOFF SUBMISSION QUEUE
// ------------------------
// As noted above, the submission queue has fixed capacity, so pushToSQ()
// may fail. In that case, the task is pushed to the backoff queue
// (implemented as a singly-linked list and therefore having unbounded
// capacity), which is drained later from tick(), after the worker threads
// dequeue some elements from the submission queue (and process them).
//
// The backoff queue is only accessed by the main thread, so no fancy
// synchronization is needed.

inline void ThreadPool::pushToBackoffSQ(ThreadPool::Task* task) {
    task->next_ = nullptr;
    *d->backoffSqTailPtr = task;
    d->backoffSqTailPtr = &task->next_;
}

inline void ThreadPool::submitBackoffSQ() {
    for (Task* t = d->backoffSqHead; t;) {
        Task* next = t->next_;
        if (!pushToSQ(t)) { // Submission queue at capacity
            d->backoffSqHead = t;
            return;
        }
        // Note: the task may have been already dequeued by a worker thread,
        // processed, and pushed to CQ, and its `next` pointer may have been
        // reused; so grab it before calling pushToSQ().
        t = next;
    }

    // The backoff queue fully submitted
    d->backoffSqHead = nullptr;
    d->backoffSqTailPtr = &d->backoffSqHead;
}


// COMPLETION QUEUE
// ----------------
// The completion queue is naturally MPSC, so we can use a textbook
// implementation based on a lock-free stack.
//
// The consumer thread does not dequeue individual elements, but rather
// grabs the entire queue at once, and then processes elements
// at convenient pace.
//
// The first thread to push to the queue is responsible for notifying
// the consumer thread (by posting a tick() event).

inline void ThreadPool::pushToCQ(ThreadPool::Task* task) {
    Task* head;
    while (true) {
        head = d->cqHead.load();
        task->next_ = head;
        if (d->cqHead.compare_exchange_weak(head, task,
                                            std::memory_order_release,
                                            std::memory_order_relaxed)) {
            break;
        }
    }
    if (!head) {
        d->notification->post(&ThreadPool::tick, this);
    }
}

inline void ThreadPool::drainCQ() {
    detail::IntrusivePtr<Data> dd = d;

    while (true) {
        Task* head = dd->cqHead.exchange(nullptr, std::memory_order_acquire);
        if (!head) {
            break;
        }

        // Reverse the list to process completion events in FIFO order
        Task* prev = nullptr;
        Task* curr = head;
        while (curr) {
            Task* next = curr->next_;
            curr->next_ = prev;
            prev = curr;
            curr = next;
        }

        // Process the list
        for (Task* t = prev; t;) {
            Task* next = t->next_;
            t->parent_.resume();
            // Note: this may have deleted this, so we grabbed
            // `dd` above to extend lifetime of all member
            // variables.
            t = next;
        }
    }
}

} // namespace corral
