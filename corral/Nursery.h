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
#include <variant>
#include <vector>

#include "ErrorPolicy.h"
#include "Executor.h"
#include "Task.h"
#include "detail/IntrusiveList.h"
#include "detail/ParkingLot.h"
#include "detail/PointerBits.h"
#include "detail/Promise.h"
#include "utility.h"

namespace corral {

template <class Ret = void> class TaskStarted;

namespace detail {
class TagCtor {};
struct JoinTag {
    explicit constexpr JoinTag(TagCtor) {}
};
struct CancelTag {
    explicit constexpr CancelTag(TagCtor) {}
};

template <class Policy>
using NurseryBodyRetval = std::conditional_t<
        PolicyUsesErrorCodes<Policy>,
        std::variant<typename Policy::ErrorType, JoinTag, CancelTag>,
        std::variant<JoinTag, CancelTag>>;

class TaskStartedTag {
    explicit constexpr TaskStartedTag(TagCtor) {}
};

template <class NurseryT> class NurseryJoinAwaiter;
template <class Derived, class Policy> class NurseryParentAwaiter;
template <class Ret> class TaskStartedSink;
template <class Poilicy, class Ret, class Callable, class... Args>
class NurseryStartAwaiter;

// A portion of `BasicNursery` invariant of used error policy.
class NurseryBase : private Noncopyable {
    template <class Ret> friend class TaskStarted;

  public:
    ~NurseryBase() { CORRAL_ASSERT(tasks_.empty()); }

  protected /*methods*/:
    Executor* executor() const noexcept { return executor_.ptr(); }
    bool cancelling() const noexcept { return executor_.bits() & Cancelling; }
    void doCancel();
    void cancel();
    void adopt(detail::BasePromise* promise);

    template <class Ret>
    Handle addPromise(detail::Promise<Ret>* p, TaskParent<Ret>* parent);

    template <class Ret>
    Handle addTask(Task<Ret> task, TaskParent<Ret>* parent) {
        return addPromise(task.release(), parent);
    }

  protected /*fields*/:
    static constexpr const size_t Cancelling = 1;

    detail::PointerBits<Executor, size_t, 1> executor_{nullptr, 0};
    detail::IntrusiveList<detail::BasePromise> tasks_;
    unsigned taskCount_ = 0;
    unsigned pendingTaskCount_ = 0;
    Handle parent_ = nullptr;

    template <class Ret> friend class detail::TaskStartedSink;
};

struct NurseryOpener;

} // namespace detail

static constexpr detail::JoinTag join{detail::TagCtor{}};
static constexpr detail::CancelTag cancel{detail::TagCtor{}};

/// A nursery represents a scope for a set of tasks to live in.
/// Execution cannot continue past the end of the nursery block until
/// all the tasks that were running in the nursery have completed.
///
/// Since C++ does not support asynchronous destructors, a nursery
/// requires special syntax to construct:
///
///    CORRAL_WITH_NURSERY(n) {
///        // `corral::Nursery& n` defined in this scope
///        n.start(...);
///        co_return corral::join; // or corral::cancel, see below
///    };
///
/// If any task exits with an error, all other tasks in the
/// nursery will be cancelled, and the error will be propagated further up
/// once the tasks in the nursery have completed. If multiple tasks exit with
/// errors, only the first error will propagate.
///
/// For `Nursery`, "error" in the above discussion means a thrown exception.
/// If you use a different convention for representing errors, such as
/// `std::expected`, then use `CORRAL_WITH_BASIC_NURSERY(Policy, n)`,
/// where `Policy` is an error policy class as described in `ErrorPolicy.h`.
/// The resulting nursery type if a policy is specified will be
/// `BasicNursery<Policy>`.
///
/// The body of the nursery block is the first task that runs in the
/// nursery. Be careful defining local variables within this block;
/// they will be destroyed when this initial task completes, but other
/// tasks may still be running. Anything that you intend to make
/// available to other tasks in the nursery should be declared _outside_
/// the nursery block so that its scope covers the entire nursery.
///
/// The initial task that forms the nursery block must end by returning
/// either `corral::join` or `corral::cancel`. `join` will wait for all
/// tasks in the nursery to exit normally; `cancel` will cancel the
/// remaining tasks. Note that in the latter case, after the cancellation
/// request is forwarded to the tasks, the nursery will still wait for them
/// to finish.
///
/// Tasks do not need to be spawned from directly within the nursery
/// block; you can pass the nursery reference to another function,
/// store a pointer, etc, and use the nursery's `start()` method to
/// start new tasks from any context that has access to the
/// reference. Once all tasks in the nursery have exited, execution
/// will proceed in the nursery's parent task, meaning the nursery
/// will be destroyed and any attempt to spawn new tasks will produce
/// undefined behavior. To avoid lifetime issues from unexpected
/// nursery closure, you should be careful not to preserve a
/// reference/pointer to the nursery outside the lifetime of some
/// specific task in the nursery.
template <class PolicyT>
class BasicNursery
  : public detail::NurseryBase,
    private detail::TaskParent<detail::PolicyReturnTypeFor<PolicyT, void>> {
    using NurseryBase = detail::NurseryBase;

    template <class, class> friend class detail::NurseryParentAwaiter;
    template <class> friend class detail::NurseryJoinAwaiter;
    template <class, class, class, class...>
    friend class detail::NurseryStartAwaiter;

    friend detail::NurseryOpener;

  protected:
    using Policy = PolicyT;
    using TaskReturnType = detail::PolicyReturnTypeFor<PolicyT, void>;

  public:
    struct Factory;

    /// A nursery construction macro.
#define CORRAL_WITH_BASIC_NURSERY(PolicyT, argname)                            \
    co_yield ::corral::BasicNursery<PolicyT>::Factory{} %                      \
            [&](::corral::BasicNursery<PolicyT> & argname)                     \
            -> ::corral::Task<::corral::detail::NurseryBodyRetval<PolicyT>>

    ~BasicNursery()
        requires ApplicableErrorPolicy<PolicyT, TaskReturnType>
    {}

    unsigned taskCount() const noexcept { return this->taskCount_; }

    /// Starts a task in the nursery that runs
    /// `co_await std::invoke(c, args...)`.
    /// The callable and its arguments will be moved into storage that
    /// lives as long as the new task does. You can wrap arguments in
    /// `std::ref()` or `std::cref()` if you want to actually pass by
    /// reference; be careful that the referent will live long enough.
    template <class Callable, class... Args>
        requires(std::invocable<Callable, Args...> &&
                 Awaitable<std::invoke_result_t<Callable, Args...>> &&
                 !std::invocable<Callable, Args..., detail::TaskStartedTag>)
    void start(Callable c, Args... args);

    /// Same as above, but allowing the task to notify the starter
    /// of the successful initialization of the task.
    template <class Ret = detail::Unspecified, class Callable, class... Args>
        requires(std::invocable<Callable, Args..., detail::TaskStartedTag> &&
                 Awaitable<std::invoke_result_t<Callable,
                                                Args...,
                                                detail::TaskStartedTag>>)
    Awaitable auto start(Callable c, Args... args);

    /// Requests cancellation of all tasks.
    void cancel() { NurseryBase::cancel(); }

    /// Returns the executor for this nursery. This may be nullptr if the
    /// nursery is closed (meaning no new tasks can be started in it).
    Executor* executor() const noexcept { return NurseryBase::executor(); }

    template <class Callable> class Scope;

  protected:
    void doStart(detail::Promise<TaskReturnType>* p) {
        this->addPromise(p, this).resume();
    }

    TaskReturnType wrapError();

    /// TaskParent implementation
    Handle continuation(detail::BasePromise* promise) noexcept override;
    void storeError(typename Policy::ErrorType e) noexcept;
    void storeValue(detail::InhabitedType<TaskReturnType> value) override;
    void storeException() override;

    template <class Callable, class... Args>
    detail::Promise<TaskReturnType>* makePromise(Callable c, Args... args);

    void introspect(detail::TaskTreeCollector& c,
                    const char* title) const noexcept;

  protected:
    typename Policy::ErrorType error_{};
};

/// A nursery with the default error handling policy, which uses
/// C++ exceptions for error propagation.
using Nursery = BasicNursery<detail::DefaultErrorPolicy>;

#define CORRAL_WITH_NURSERY(argname)                                           \
    CORRAL_WITH_BASIC_NURSERY(::corral::detail::DefaultErrorPolicy, argname)


/// A variant of a nursery which can be used when adding async
/// functions to existing code, where propagating nurseries throughout
/// the code might not be feasible.  Unlike a regular Nursery, it is
/// constructible, which allows storing it in user classes.
///
/// Users of UnsafeNursery need to manually make sure no tasks are still alive
/// when the nursery goes out of scope, with no aid from the compiler on that
/// (hence "unsafe"). It will attempt to cancel() anything still alive from its
/// destructor, which will do the job if all tasks spawned are known to support
/// synchronous cancellation. Otherwise, you will get an assertion failure
/// (or other behavior as provided by any custom CORRAL_FAIL_FOR_DANGLING_TASKS
/// macro that you've provided; see config.h).
///
/// It is possible to "adopt" an UnsafeNursery by awaiting its join() method.
/// This basically turns it into a regular nursery with the task
/// that called join() as its parent: any further exception
/// raised in its child task will be reraised in the parent, cancelling the
/// parent will cancel the nursery's tasks, and the parent will not resume
/// until the nursery is closed (all tasks have exited and no more are allowed
/// to be spawned).
///
/// If a task in an UnsafeNursery terminates with an exception when there is
/// no other task blocked on its UnsafeNursery::join() method, std::terminate()
/// will be called.
///
/// Usage of this class is generally discouraged because it requires
/// a deep understanding of the nature of any tasks spawned directly or
/// indirectly into this nursery.
template <class Policy>
class BasicUnsafeNursery final : public BasicNursery<Policy>, private Executor {
    using TaskReturnType = typename BasicNursery<Policy>::TaskReturnType;

  public:
    template <class EventLoopT>
    explicit BasicUnsafeNursery(EventLoopT&& eventLoop)
      : Executor(std::forward<EventLoopT>(eventLoop),
                 *this,
                 Executor::Capacity::Small) {
        this->executor_.set(this, 0);
    }

    // This is in UnsafeNursery because a regular nursery is never
    // observably empty (it will resume its parent, thus destroying
    // the nursery, as soon as it has no tasks left)
    bool empty() const noexcept { return this->tasks_.empty(); }

    ~BasicUnsafeNursery() { close(); }

    /// Perform the operation done by the destructor explicitly.
    /// Cancel any tasks still running, give them a chance to clean up
    /// (one run of the executor, so the cleanup can't block on I/O),
    /// and fail if this was not sufficient. Once close() returns successfully,
    /// the nursery is closed and any attempt to submit more tasks to it
    /// will produce undefined behavior.
    void close() {
        if (!this->tasks_.empty()) {
            this->schedule(
                    +[](BasicUnsafeNursery* self) noexcept { self->cancel(); },
                    this);
        }
        this->Executor::drain();
        assertEmpty();
        this->executor_.setPtr(nullptr);
    }

    /// Asynchronously closes the nursery.
    /// Any tasks still running will be cancelled; the provided continuation
    /// will be executed once the nursery becomes empty
    /// (at which point it's safe to destroy).
    ///
    /// Note that the continuation will be immediately executed
    /// (before asyncClose() returns) if the nursery is already empty.
    void asyncClose(std::invocable<> auto continuation) {
        CORRAL_ASSERT(this->parent_ == nullptr &&
                      "nursery already joined or asyncClose()d");
        if (this->tasks_.empty()) {
            this->executor_.setPtr(nullptr);
            continuation();
        } else {
            this->parent_ = asCoroutineHandle(
                    [this, c = std::move(continuation)]() noexcept {
                        if (Policy::hasError(this->error_)) {
                            Policy::terminateBy(this->error_);
                            CORRAL_ASSERT_UNREACHABLE();
                        }
                        c();
                    });
            this->cancel();
        }
    }

    void assertEmpty() {
        if (!this->tasks_.empty()) {
            CORRAL_FAIL_FOR_DANGLING_TASKS(
                    "UnsafeNursery destroyed with tasks still active", *this);
        }
    }

    /// A task can call this async function to "adopt" the nursery
    /// (become its new parent), as if it were a regular/safe nursery
    /// originally opened in that context. Any further exceptions will
    /// go to the new parent task, cancellation will filter from the
    /// new parent to the nursery's children, etc. Once join() returns,
    /// the nursery is closed and any attempt to submit more tasks to it
    /// will produce undefined behavior.
    corral::Awaitable<TaskReturnType> auto join();

    // Allow the nursery itself to be an introspection root for its executor
    void await_introspect(detail::TaskTreeCollector& c) const noexcept {
        this->introspect(c, "UnsafeNursery");
    }
};

using UnsafeNursery = BasicUnsafeNursery<detail::DefaultErrorPolicy>;


/// A callable spawned into a nursery can take a `TaskStarted` argument
/// it can later call to indicate that it has started running.
///
/// `TaskStarted<T>` allows passing a result back to the caller once
/// the task is initialized: it must be called with an argument of type
/// T, and the corresponding `co_await nursery.start(...)` will evaluate
/// to that T value. `TaskStarted<>` or `TaskStarted<void>` must be
/// called with no arguments.
template <class Ret> class TaskStarted {
    using ResultType = Ret;

    explicit TaskStarted(detail::TaskStartedSink<Ret>* parent)
      : parent_(parent) {}

    template <class Policy> friend class BasicNursery;
    template <class, class, class, class...>
    friend class detail::NurseryStartAwaiter;

  public:
    TaskStarted() = default;

    TaskStarted(TaskStarted&& rhs)
      : parent_(std::exchange(rhs.parent_, nullptr)) {}
    TaskStarted& operator=(TaskStarted rhs) {
        std::swap(parent_, rhs.parent_);
        return *this;
    }

    void operator()(detail::InhabitedType<Ret> ret)
        requires(!std::is_same_v<Ret, void>);

    void operator()()
        requires(std::is_same_v<Ret, void>);

    // Required for constraints on `Nursery::start()`.
    explicit(false) TaskStarted(detail::TaskStartedTag); // not defined

  protected:
    detail::TaskStartedSink<Ret>* parent_ = nullptr;
};


namespace detail {
struct NurseryOpener {
    explicit constexpr NurseryOpener(TagCtor) {}

    template <class Policy>
    Task<PolicyReturnTypeFor<Policy, void>> operator()(
            BasicNursery<Policy>*& ptr, TaskStarted<> started = {}) const;

    template <class Policy>
    Task<PolicyReturnTypeFor<Policy, void>> operator()(
            std::reference_wrapper<BasicNursery<Policy>*> ptr,
            TaskStarted<> started = {}) const;
};

} // namespace detail

/// Usable for implementing live objects, if the only things needed
/// from their `run()` methods is establishing a nursery:
///
///     class MyLiveObject {
///         corral::Nursery* nursery_;
///       public:
///         auto run() { return corral::openNursery(nursery_); }
///         void startStuff() { nursery_->start(doStuff()); }
///     };
///
/// The nursery pointer passed as an argument will be initialized once
/// the nursery is opened, and reset to nullptr when the nursery
/// is closed. Does not return until cancelled.
static constexpr detail::NurseryOpener openNursery(detail::TagCtor{});


// ------------------------------------------------------------------------------------
// Implementation

//
// Nursery::StartAwaiter
//

namespace detail {

template <class Ret> class TaskStartedSink : private Noncopyable {
    friend TaskStarted<Ret>;
    virtual void markStarted(detail::InhabitedType<Ret>) = 0;
};

template <class Policy, class Ret, class Callable, class... Args>
class NurseryStartAwaiter
  : public detail::TaskStartedSink<Ret>,
    private detail::TaskParent<detail::PolicyReturnTypeFor<Policy, void>> {
    friend BasicNursery<Policy>;
    class Maker;
    using TaskReturnType = PolicyReturnTypeFor<Policy, void>;

  public:
    NurseryStartAwaiter(NurseryStartAwaiter&&) = default;
    NurseryStartAwaiter& operator=(NurseryStartAwaiter&&) = delete;

    ~NurseryStartAwaiter() {
        CORRAL_ASSERT(!nursery_ && "Nursery::StartAwaiter not awaited");
    }

    bool await_early_cancel() noexcept {
        setCancelling();
        return false;
    }

    bool await_ready() const noexcept { return false; }

    void await_set_executor(Executor* ex) noexcept { executor_.setPtr(ex); }

    Handle await_suspend(Handle h) {
        CORRAL_TRACE("    ...Nursery::start() %p", this);
        detail::PromisePtr<TaskReturnType> promise{std::apply(
                [this](auto&&... args) {
                    return nursery_->makePromise(std::move(callable_),
                                                 std::move(args)...,
                                                 TaskStarted<Ret>{this});
                },
                std::move(args_))};

        if (result_.completed()) {
            // TaskStarted<> was invoked before promise construction,
            // and handOff() was skipped; hand off the promise to the nursery
            // ourselves.
            std::exchange(nursery_, nullptr)->doStart(promise.release());
            return h;
        } else {
            ++nursery_->pendingTaskCount_;
            handle_ = h;
            promise->setExecutor(executor_.ptr());
            if (isCancelling()) {
                promise->cancel();
            }
            promise_ = std::move(promise);
            return promise_->start(this, h);
        }
    }

    detail::PolicyReturnTypeFor<Policy, Ret> await_resume() && {
        return std::move(result_).value();
    }

    bool await_cancel(Handle) noexcept {
        if (promise_) {
            setCancelling();
            promise_->cancel();
        }
        return false;
    }
    auto await_must_resume() const noexcept { return std::false_type{}; }

  private:
    NurseryStartAwaiter(BasicNursery<Policy>* nursery,
                        Callable&& callable,
                        std::tuple<Args...>&& args)
      : nursery_(nursery),
        callable_(std::forward<Callable>(callable)),
        args_(std::move(args)) {}

    void markStarted(detail::InhabitedType<Ret> ret) override {
        if constexpr (!std::is_same_v<Ret, void>) {
            result_.storeValue(Policy::wrapValue(std::move(ret)));
        } else if constexpr (!detail::PolicyUsesErrorCodes<Policy>) {
            result_.storeValue(detail::Void{});
        } else {
            result_.storeValue(Policy::wrapValue());
        }
        handOff();
    }

    enum Flags { Cancelling = 1 };
    bool isCancelling() const { return executor_.bits() & Cancelling; }
    void setCancelling() { executor_.setBits(Cancelling); }

    Handle continuation(detail::BasePromise* p) noexcept override {
        if (BasicNursery<Policy>* n = std::exchange(nursery_, nullptr)) {
            // The task completed without calling TaskStarted<>::operator().
            --n->pendingTaskCount_;
        }
        return std::exchange(handle_, noopHandle());
    }

    void handOff() {
        if (isCancelling()) {
            return;
        }

        detail::Promise<TaskReturnType>* p = promise_.release();
        if (p) {
            p->setExecutor(nursery_->executor());
            p->reparent(nursery_, nursery_->parent_);
            BasicNursery<Policy>* n = std::exchange(nursery_, nullptr);
            --n->pendingTaskCount_;
            n->adopt(p);
            std::exchange(handle_, noopHandle()).resume();
        } else {
            // TaskStarted<> was invoked before promise construction,
            // so there's nothing to hand off to the nursery yet.
            //
            // StartAwaiter::await_suspend() will take care of submitting
            // the promise properly when it becomes available.
        }
    }

    // TaskParent<> implementation
    void storeValue(
            detail::InhabitedType<detail::PolicyReturnTypeFor<Policy, void>>
                    ret) override {
        auto err = Policy::unwrapError(ret);
        if (!Policy::hasError(err)) {
            CORRAL_ASSERT(
                    isCancelling() &&
                    "Nursery task completed without signalling readiness");
        } else if constexpr (detail::PolicyUsesErrorCodes<Policy>) {
            result_.storeValue(Policy::wrapError(std::move(err)));
        } else {
            CORRAL_ASSERT_UNREACHABLE();
        }
    }
    void storeException() override { result_.storeException(); }
    void cancelled() override { result_.markCancelled(); }

    // Construction

    class Maker {
      public:
        Maker(BasicNursery<Policy>* nursery,
              Callable&& callable,
              Args&&... args)
          : nursery_(nursery),
            callable_(std::forward<Callable>(callable)),
            args_(std::forward<Args>(args)...) {}

        // NOLINTNEXTLINE(performance-noexcept-move-constructor)
        Maker(Maker&& rhs) noexcept(
                std::is_nothrow_move_constructible_v<Callable> &&
                std::is_nothrow_move_constructible_v<std::tuple<Args...>>)
          : nursery_(std::exchange(rhs.nursery_, nullptr)),
            callable_(std::move(rhs.callable_)),
            args_(std::move(rhs.args_)) {}

        Maker& operator=(Maker&&) = delete;

        ~Maker() {
            if (nursery_) {
                std::apply(
                        [this](auto&&... args) {
                            nursery_->start(std::move(callable_),
                                            std::move(args)...,
                                            TaskStarted<Ret>(nullptr));
                        },
                        std::move(args_));
            }
        }

        auto operator co_await() && {
            return NurseryStartAwaiter<Policy, Ret, Callable, Args...>(
                    std::exchange(nursery_, nullptr), std::move(callable_),
                    std::move(args_));
        }

      private:
        BasicNursery<Policy>* nursery_;
        Callable callable_;
        std::tuple<Args...> args_;
    };

    static auto makeAwaitable(BasicNursery<Policy>* nursery,
                              Callable&& callable,
                              Args&&... args) {
        return Maker(nursery, std::forward<Callable>(callable),
                     std::forward<Args>(args)...);
    }

  private:
    BasicNursery<Policy>* nursery_;
    Handle handle_ = noopHandle();
    detail::PromisePtr<TaskReturnType> promise_;
    detail::PointerBits<Executor, Flags, 1> executor_;

    Callable callable_;
    std::tuple<Args...> args_;
    detail::Result<detail::PolicyReturnTypeFor<Policy, Ret>> result_;
};

} // namespace detail

template <class Ret>
void TaskStarted<Ret>::operator()(detail::InhabitedType<Ret> ret)
    requires(!std::is_same_v<Ret, void>)
{
    if (auto p = std::exchange(parent_, nullptr)) {
        p->markStarted(std::forward<Ret>(ret));
    }
}

template <class Ret>
void TaskStarted<Ret>::operator()()
    requires(std::is_same_v<Ret, void>)
{
    if (auto p = std::exchange(parent_, nullptr)) {
        p->markStarted(detail::Void{});
    }
}

//
// detail::NurseryBase
//

namespace detail {

template <class Ret>
inline Handle NurseryBase::addPromise(detail::Promise<Ret>* promise,
                                      TaskParent<Ret>* parent) {
    CORRAL_ASSERT(promise);
    adopt(promise);
    promise->setExecutor(executor());
    return promise->start(parent, parent_);
}

inline void NurseryBase::adopt(detail::BasePromise* promise) {
    CORRAL_ASSERT(executor_.ptr() && "Nursery is closed to new arrivals");
    CORRAL_TRACE("pr %p handed to nursery %p (%zu tasks total)", promise, this,
                 taskCount_ + 1);
    if (cancelling()) {
        promise->cancel();
    }
    tasks_.push_back(*promise);
    ++taskCount_;
}

inline void NurseryBase::doCancel() {
    if (!executor() || tasks_.empty()) {
        return;
    }

    // Task cancellation may modify tasks_ arbitrarily,
    // invalidating iterators to task being cancelled or its
    // neighbors, thereby making it impossible to traverse through
    // tasks_ safely; so defer calling cancel() through the executor.
    Executor* ex = executor_.ptr();
    ex->capture(
            [this] {
                for (detail::BasePromise& t : tasks_) {
                    executor()->schedule(
                            +[](detail::BasePromise* p) noexcept {
                                p->cancel();
                            },
                            &t);
                }
            },
            taskCount_);

    ex->runSoon();
}

inline void NurseryBase::cancel() {
    if (cancelling()) {
        return; // already cancelling
    }
    CORRAL_TRACE("nursery %p cancellation requested", this);
    executor_.setBits(Cancelling);
    doCancel();
}

} // namespace detail


//
// BasicNursery
//

template <class Policy>
detail::PolicyReturnTypeFor<Policy, void> BasicNursery<Policy>::wrapError() {
    if (!Policy::hasError(error_)) {
        return Policy::wrapValue();
    } else if constexpr (!detail::PolicyUsesErrorCodes<Policy>) {
        Policy::wrapError(error_);
        CORRAL_ASSERT_UNREACHABLE();
    } else {
        return Policy::wrapError(error_);
    }
}

template <class Policy>
template <class Callable, class... Args>
detail::Promise<typename BasicNursery<Policy>::TaskReturnType>* //
BasicNursery<Policy>::makePromise(Callable callable, Args... args) {
    using Ret = Task<TaskReturnType>;
    Ret ret;
    if constexpr ((std::is_reference_v<Callable> &&
                   std::is_invocable_r_v<Ret, Callable>) ||
                  std::is_convertible_v<Callable, Ret (*)()>) {
        // The awaitable is an async lambda (lambda that produces a Task<>)
        // and it either was passed by lvalue reference or it is stateless,
        // and no arguments were supplied.
        // In this case, we don't have to worry about the lifetime of its
        // captures, and can thus save an allocation here.
        ret = callable();
    } else {
        using PassedReturnType = detail::AwaitableReturnType<
                std::invoke_result_t<Callable, Args...>>;
        // The lambda has captures, or we're working with a different
        // awaitable type, so wrap it into another async function.
        // The contents of the awaitable object (such as the lambda
        // captures) will be kept alive as an argument of the new
        // async function.

        // Note: cannot use `std::invoke()` here, as any temporaries
        // created inside it will be destroyed before `invoke()` returns.
        // We need funciton call and `co_await` inside one statement,
        // so mimic `std::invoke()` logic here.
        if constexpr (std::is_member_pointer_v<Callable>) {
            ret = [](Callable c, auto obj, auto... a) -> Ret {
                if constexpr (std::is_same_v<PassedReturnType, detail::Void> ||
                              std::is_same_v<TaskReturnType, void>) {
                    // Either we are given a void-returning awaitable, or we are
                    // a void-returning task ourselves here; in either case
                    // we cannot `co_return co_await ...` here.

                    // Either exceptions are used to indicate failure
                    // (in which case they will auto-propagate)...
                    if constexpr (std::is_pointer_v<decltype(obj)>) {
                        co_await (obj->*c)(std::move(a)...);
                    } else if constexpr (detail::is_reference_wrapper_v<
                                                 decltype(obj)>) {
                        co_await (obj.get().*c)(std::move(a)...);
                    } else {
                        co_await (std::move(obj).*c)(std::move(a)...);
                    }

                    // ...or the task is infallible, so return an indication
                    // of success in the used policy.
                    if constexpr (!std::is_same_v<TaskReturnType, void>) {
                        co_return Policy::wrapValue();
                    }

                } else {
                    // The awaitable indicates its success or failure through
                    // returning a value, so simply pass it further up.
                    if constexpr (std::is_pointer_v<decltype(obj)>) {
                        co_return co_await (obj->*c)(std::move(a)...);
                    } else if constexpr (detail::is_reference_wrapper_v<
                                                 decltype(obj)>) {
                        co_return co_await (obj.get().*c)(std::move(a)...);
                    } else {
                        co_return co_await (std::move(obj).*c)(std::move(a)...);
                    }
                }
            }(std::move(callable), std::move(args)...);
        } else {
            ret = [](Callable c, Args... a) -> Ret {
                // See comments above for description of alternatives here
                if constexpr (std::is_same_v<PassedReturnType, detail::Void>) {
                    co_await (std::move(c))(std::move(a)...);
                    if constexpr (!std::is_same_v<TaskReturnType, void>) {
                        co_return Policy::wrapValue();
                    }
                } else {
                    co_return co_await (std::move(c))(std::move(a)...);
                }
            }(std::move(callable), std::move(args)...);
        }
    }

    return ret.release();
}

template <class Policy>
template <class Callable, class... Args>
    requires(std::invocable<Callable, Args...> &&
             Awaitable<std::invoke_result_t<Callable, Args...>> &&
             !std::invocable<Callable, Args..., detail::TaskStartedTag>)
void BasicNursery<Policy>::start(Callable callable, Args... args) {
    doStart(makePromise(std::forward<Callable>(callable),
                        std::forward<Args>(args)...));
}

template <class Policy>
template <class Ret /* = detail::Unspecified*/, class Callable, class... Args>
    requires(std::invocable<Callable, Args..., detail::TaskStartedTag> &&
             Awaitable<std::invoke_result_t<Callable,
                                            Args...,
                                            detail::TaskStartedTag>>)
Awaitable auto BasicNursery<Policy>::start(Callable callable, Args... args) {
    if constexpr (!std::is_same_v<Ret, detail::Unspecified>) {
        return detail::NurseryStartAwaiter<Policy, Ret, Callable, Args...>::
                makeAwaitable(this, std::move(callable), std::move(args)...);
    } else if constexpr (std::invocable<Callable, Args..., TaskStarted<>>) {
        // Explicitly check for `TaskStarted<void>` applicability here
        // to simplify passing polymorphic callables, whose signature
        // cannot be deduced (including `openNursery()`).
        return detail::NurseryStartAwaiter<Policy, void, Callable, Args...>::
                makeAwaitable(this, std::move(callable), std::move(args)...);
    } else {
        using Sig = detail::CallableSignature<Callable>;
        using TaskStartedArg = typename Sig::template Arg<Sig::Arity - 1>;
        using ResultType = typename TaskStartedArg::ResultType;
        return detail::NurseryStartAwaiter<
                Policy, ResultType, Callable,
                Args...>::makeAwaitable(this, std::move(callable),
                                        std::move(args)...);
    }
}

template <class Policy>
void BasicNursery<Policy>::storeError(typename Policy::ErrorType e) noexcept {
    if (parent_ == nullptr) {
        // This is an UnsafeNursery that has not been join()'ed. There is no
        // one we can pass our error to, so we have no choice but to...
        Policy::terminateBy(e);
    }
    bool needCancel = !cancelling();
    if (!Policy::hasError(error_)) {
        error_ = std::move(e);
    }
    executor_.setBits(Cancelling);
    if (needCancel) {
        doCancel();
    }
}

template <class Policy>
void BasicNursery<Policy>::storeValue(
        detail::InhabitedType<typename BasicNursery<Policy>::TaskReturnType>
                value) {
    if constexpr (!std::is_same_v<TaskReturnType, void>) {
        auto err = Policy::unwrapError(value);
        if (Policy::hasError(err)) {
            storeError(std::move(err));
        }
    }
}

template <class Policy> void BasicNursery<Policy>::storeException() {
    storeError(detail::errorFromCurrentException<Policy>());
}

template <class Policy>
inline Handle BasicNursery<Policy>::continuation(
        detail::BasePromise* promise) noexcept {
    CORRAL_TRACE("pr %p done in nursery %p (%zu tasks remaining)", promise,
                 this, taskCount_ - 1);
    tasks_.erase(*promise);
    --taskCount_;

    Executor* executor = executor_.ptr();
    Handle ret = noopHandle();
    // NB: in an UnsafeNursery, parent_ is the task that called join(), or
    // nullptr if no one has yet
    if (tasks_.empty() && pendingTaskCount_ == 0 && parent_ != nullptr) {
        ret = std::exchange(parent_, nullptr);
        executor_.setPtr(nullptr); // nursery is now closed
    }

    // Defer promise destruction to the executor, as this may call
    // scope guards, essentially interrupting the coroutine which called
    // Nursery::cancel().
    executor->runSoon(
            +[](detail::BasePromise* p) noexcept { p->destroy(); }, promise);

    // To be extra safe, defer the resume() call to the executor as well,
    // so we can be sure we don't resume the parent before destroying the frame
    // of the last child.
    if (ret != noopHandle()) {
        executor->runSoon(
                +[](void* arg) noexcept { Handle::from_address(arg).resume(); },
                ret.address());
    }
    return std::noop_coroutine();
}


template <class Policy>
void BasicNursery<Policy>::introspect(detail::TaskTreeCollector& c,
                                      const char* title) const noexcept {
    c.node(title);
    for (auto& t : tasks_) {
        c.child(t);
    }
}

//
// Logic for binding the nursery parent to the nursery
//

namespace detail {

template <class Derived, class Policy> class NurseryParentAwaiter {
    Derived& self() { return static_cast<Derived&>(*this); }
    const Derived& self() const { return static_cast<const Derived&>(*this); }

  public:
    bool await_early_cancel() noexcept {
        self().nursery_.cancel();
        return false;
    }
    bool await_cancel(Handle) noexcept {
        self().nursery_.cancel();
        return false;
    }
    [[nodiscard]] PolicyReturnTypeFor<Policy, void> await_resume() {
        CORRAL_TRACE("nursery %p done", &self().nursery_);
        return self().nursery_.wrapError();
    }
    bool await_must_resume() const noexcept {
        return Policy::hasError(self().nursery_.error_);
    }
};

template <class NurseryT>
class NurseryJoinAwaiter
  : public NurseryParentAwaiter<NurseryJoinAwaiter<NurseryT>,
                                typename NurseryT::Policy> {
    friend NurseryParentAwaiter<NurseryJoinAwaiter<NurseryT>,
                                typename NurseryT::Policy>;
    friend NurseryT;

    NurseryT& nursery_;

    explicit NurseryJoinAwaiter(NurseryT& nursery) : nursery_(nursery) {}

  public:
    ~NurseryJoinAwaiter()
        requires(std::derived_from<NurseryT,
                                   BasicNursery<typename NurseryT::Policy>>)
    = default;

    bool await_ready() const noexcept {
        return nursery_.executor_.ptr() == nullptr;
    }
    bool await_suspend(Handle h) {
        CORRAL_ASSERT(!nursery_.parent_);
        if (nursery_.tasks_.empty()) {
            // Just close the nursery, don't actually suspend
            nursery_.executor_.setPtr(nullptr);
            return false;
        }
        nursery_.parent_ = h;
        return true;
    }
    void await_introspect(detail::TaskTreeCollector& c) const noexcept {
        nursery_.await_introspect(c);
    }
};

} // namespace detail

template <class Policy>
Awaitable<typename BasicNursery<Policy>::TaskReturnType> auto
BasicUnsafeNursery<Policy>::join() {
    return detail::NurseryJoinAwaiter(*this);
}

//
// Nursery construction
//

template <class Policy>
template <class Callable>
class BasicNursery<Policy>::Scope
  : public detail::NurseryScopeBase,
    public detail::NurseryParentAwaiter<Scope<Callable>, Policy>,
    private detail::TaskParent<detail::NurseryBodyRetval<Policy>> {
    class Impl : public BasicNursery<Policy> {
      public:
        Impl() = default;
        Impl(Impl&&) noexcept = default;
    };

  public:
    explicit Scope(Callable&& c) : callable_(std::move(c)) {}

    void await_set_executor(Executor* ex) noexcept {
        nursery_.executor_.setPtr(ex);
    }

    bool await_ready() const noexcept { return false; }

    Handle await_suspend(Handle h) {
        nursery_.parent_ = h;
        Task<detail::NurseryBodyRetval<Policy>> body = callable_(nursery_);
        CORRAL_TRACE("    ... nursery %p starting with task %p", &nursery_,
                     body.promise_.get());
        return nursery_.addTask(std::move(body), this);
    }

    void await_introspect(detail::TaskTreeCollector& c) const noexcept {
        nursery_.introspect(c, "Nursery");
    }

  private:
    void storeValue(
            detail::NurseryBodyRetval<Policy> retval) noexcept override {
        if (std::holds_alternative<detail::JoinTag>(retval)) {
            // do nothing
        } else if (std::holds_alternative<detail::CancelTag>(retval)) {
            nursery_.cancel();
        } else if constexpr (detail::PolicyUsesErrorCodes<Policy>) {
            nursery_.storeError(
                    std::get<typename Policy::ErrorType>(std::move(retval)));
        } else {
            CORRAL_ASSERT_UNREACHABLE();
        }
    }

    void storeException() noexcept override {
        nursery_.storeError(detail::errorFromCurrentException<Policy>());
    }

    Handle continuation(detail::BasePromise* promise) noexcept override {
        return nursery_.continuation(promise);
    }

  private:
    friend detail::NurseryParentAwaiter<Scope, Policy>; // so it can access
                                                        // nursery_
    CORRAL_NO_UNIQUE_ADDR Callable callable_;
    Impl nursery_;
};

template <class Policy> struct BasicNursery<Policy>::Factory {
    template <class Callable> auto operator%(Callable&& c) {
        return Scope<Callable>(std::forward<Callable>(c));
    };
};


namespace detail {
template <class Policy>
class BackreferencedNursery final : public BasicNursery<Policy> {
    friend NurseryOpener;
    friend NurseryJoinAwaiter<BackreferencedNursery<Policy>>;

  private /*methods*/:
    BackreferencedNursery(Executor* executor, BasicNursery<Policy>*& backref)
      : backref_(backref) {
        backref_ = this;
        this->executor_.set(executor, 0);
    }

    Handle continuation(detail::BasePromise* p) noexcept override {
        if (this->taskCount_ == 1 && this->pendingTaskCount_ == 0) {
            backref_ = nullptr;
        }
        return BasicNursery<Policy>::continuation(p);
    }

    corral::Awaitable<PolicyReturnTypeFor<Policy, void>> auto join() {
        return detail::NurseryJoinAwaiter(*this);
    }

    void await_introspect(detail::TaskTreeCollector& c) const noexcept {
        this->introspect(c, "Nursery");
    }

  private /*fields*/:
    BasicNursery<Policy>*& backref_;
};

template <class Policy>
Task<PolicyReturnTypeFor<Policy, void>> NurseryOpener::operator()(
        BasicNursery<Policy>*& ptr, TaskStarted<> started /*= {}*/) const {
    using Ret = detail::PolicyReturnTypeFor<Policy, void>;

    Executor* ex = co_await getExecutor();
    detail::BackreferencedNursery<Policy> nursery(ex, ptr);
    nursery.start([]() -> Task<Ret> {
        co_await SuspendForever{};
        unreachable(); // make clang happy
    });
    started();

    if constexpr (std::is_same_v<Ret, void>) {
        co_await nursery.join();
    } else {
        co_return co_await nursery.join();
    }
}

template <class Policy>
Task<PolicyReturnTypeFor<Policy, void>> NurseryOpener::operator()(
        std::reference_wrapper<BasicNursery<Policy>*> ptr,
        TaskStarted<> started /*= {}*/) const {
    return (*this)(ptr.get(), std::move(started));
}

} // namespace detail

} // namespace corral
