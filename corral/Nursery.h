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
#include <functional>
#include <variant>
#include <vector>

#include "Executor.h"
#include "Task.h"
#include "detail/IntrusiveList.h"
#include "detail/ParkingLot.h"
#include "detail/Promise.h"
#include "utility.h"

namespace corral {

namespace detail {
class TagCtor {};
struct JoinTag {
    explicit constexpr JoinTag(TagCtor) {}
};
struct CancelTag {
    explicit constexpr CancelTag(TagCtor) {}
};
using NurseryBodyRetval = std::variant<JoinTag, CancelTag>;

class TaskStartedTag {
    explicit constexpr TaskStartedTag(TagCtor) {}
};

}; // namespace detail

static constexpr detail::JoinTag join{detail::TagCtor{}};
static constexpr detail::CancelTag cancel{detail::TagCtor{}};

template <class Ret = void> class TaskStarted;


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
/// If any task exits with an unhandled exception, all other tasks in the
/// nursery will be cancelled, and the exception will be rethrown once the
/// tasks in the nursery have completed. If multiple tasks exit with
/// unhandled exceptions, only the first exception will propagate.
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
class Nursery : private detail::TaskParent<void> {
    template <class Ret> class StartAwaitableBase;
    template <class Ret, class Callable, class... Args> class StartAwaitable;
    template <class Ret> friend class TaskStarted;

  public:
    struct Factory;

    /// A nursery construction macro.
#define CORRAL_WITH_NURSERY(argname)                                           \
    co_yield ::corral::Nursery::Factory{} % [&](::corral::Nursery & argname)   \
            -> ::corral::Task<::corral::detail::NurseryBodyRetval>

    ~Nursery() { CORRAL_ASSERT(tasks_.empty()); }

    unsigned taskCount() const noexcept { return taskCount_; }

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
    void cancel();

    /// Returns the executor for this nursery. This may be nullptr if the
    /// nursery is closed (meaning no new tasks can be started in it).
    Executor* executor() const noexcept { return executor_; }

    template <class Callable> class Scope;

  protected:
    template <class Derived> class ParentAwaitable;

    Nursery() = default;
    Nursery(Nursery&&) = default;

    void doStart(detail::Promise<void>* p) { addPromise(p, this).resume(); }

    void rethrowException();
    static std::exception_ptr cancellationRequest();

    template <class Ret>
    Handle addPromise(detail::Promise<Ret>* p, TaskParent<Ret>* parent);

    template <class Ret>
    Handle addTask(Task<Ret> task, TaskParent<Ret>* parent) {
        return addPromise(task.release(), parent);
    }

    /// TaskParent implementation
    Handle continuation(detail::BasePromise* promise) noexcept override;
    void storeSuccess() override {}
    void storeException() override;

    void doCancel();

    template <class Callable, class... Args>
    detail::Promise<void>* makePromise(Callable c, Args... args);

    void adopt(detail::BasePromise* promise);

    template <class Ret>
    static TaskStarted<Ret> makeTaskStarted(StartAwaitableBase<Ret>* parent) {
        return TaskStarted<Ret>(parent);
    }

  protected:
    Executor* executor_ = nullptr;
    detail::IntrusiveList<detail::BasePromise> tasks_;
    unsigned taskCount_ = 0;
    unsigned pendingTaskCount_ = 0;
    Handle parent_ = nullptr;
    std::exception_ptr exception_;
};


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
class UnsafeNursery final : public Nursery, private Executor {
    class Awaitable;

  public:
    template <class EventLoopT>
    explicit UnsafeNursery(EventLoopT&& eventLoop)
      : Executor(std::forward<EventLoopT>(eventLoop),
                 *this,
                 Executor::Capacity::Small) {
        executor_ = this;
    }

    UnsafeNursery(const UnsafeNursery&) = delete;
    UnsafeNursery(UnsafeNursery&&) = delete;
    UnsafeNursery& operator=(const UnsafeNursery&) = delete;
    UnsafeNursery& operator=(UnsafeNursery&&) = delete;

    // This is in UnsafeNursery because a regular nursery is never
    // observably empty (it will resume its parent, thus destroying
    // the nursery, as soon as it has no tasks left)
    bool empty() const noexcept { return tasks_.empty(); }

    ~UnsafeNursery() { close(); }

    /// Perform the operation done by the destructor explicitly.
    /// Cancel any tasks still running, give them a chance to clean up
    /// (one run of the executor, so the cleanup can't block on I/O),
    /// and fail if this was not sufficient. Once close() returns successfully,
    /// the nursery is closed and any attempt to submit more tasks to it
    /// will produce undefined behavior.
    void close() {
        if (!tasks_.empty()) {
            this->schedule(
                    +[](UnsafeNursery* self) noexcept { self->cancel(); },
                    this);
        }
        this->Executor::drain();
        assertEmpty();
        executor_ = nullptr;
    }

    /// Asynchronously closes the nursery.
    /// Any tasks still running will be cancelled; the provided continuation
    /// will be executed once the nursery becomes empty
    /// (at which point it's safe to destroy).
    ///
    /// Note that the continuation will be immediately executed
    /// (before asyncClose() returns) if the nursery is already empty.
    void asyncClose(std::invocable<> auto continuation) {
        CORRAL_ASSERT(parent_ == nullptr &&
                      "nursery already joined or asyncClose()d");
        if (tasks_.empty()) {
            executor_ = nullptr;
            continuation();
        } else {
            parent_ = asCoroutineHandle(
                    [this, c = std::move(continuation)]() noexcept {
                        if (exception_ != cancellationRequest()) {
                            // terminate() on any exception
                            std::rethrow_exception(exception_);
                        }
                        c();
                    });
            cancel();
        }
    }

    void assertEmpty() {
        if (!tasks_.empty()) {
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
    corral::Awaitable<void> auto join();

    // Allow the nursery itself to be an introspection root for its executor
    void await_introspect(detail::TaskTreeCollector& c) const noexcept {
        static_assert(std::is_base_of_v<
                      detail::IntrusiveListItem<detail::BasePromise>,
                      detail::BasePromise>);
        c.node("UnsafeNursery");
        for (auto& t : tasks_) {
            c.child(t);
        }
    }
};


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

    explicit TaskStarted(Nursery::StartAwaitableBase<Ret>* parent)
      : parent_(parent) {}
    friend Nursery;

  public:
    TaskStarted() = default;

    TaskStarted(TaskStarted&& rhs)
      : parent_(std::exchange(rhs.parent_, nullptr)) {}
    TaskStarted& operator=(TaskStarted rhs) {
        std::swap(parent_, rhs.parent_);
        return *this;
    }

    void operator()(detail::ReturnType<Ret> ret)
        requires(!std::is_same_v<Ret, void>);

    void operator()()
        requires(std::is_same_v<Ret, void>);

    // Required for constraints on `Nursery::start()`.
    explicit(false) TaskStarted(detail::TaskStartedTag); // not defined

  protected:
    Nursery::StartAwaitableBase<Ret>* parent_ = nullptr;
};


// ------------------------------------------------------------------------------------
// Implementation

//
// Nursery::StartAwaitable
//

template <class Ret>
class Nursery::StartAwaitableBase : protected TaskParent<void> {
    friend TaskStarted<Ret>;

  public:
    explicit StartAwaitableBase(Nursery* nursery) : nursery_(nursery) {}

    StartAwaitableBase(StartAwaitableBase&& rhs)
      : nursery_(std::exchange(rhs.nursery_, nullptr)) {
        CORRAL_ASSERT((handle_ == noopHandle()) && !promise_);
    }
    StartAwaitableBase& operator=(StartAwaitableBase&&) = delete;

  private:
    void storeSuccess() override {
        CORRAL_ASSERT(!"Nursery task completed without signalling readiness");
    }
    void storeException() override { result_.storeException(); }
    void cancelled() override { result_.markCancelled(); }

    Handle continuation(detail::BasePromise* p) noexcept override {
        if (Nursery* n = std::exchange(nursery_, nullptr)) {
            // The task completed without calling TaskStarted<>::operator().
            --n->pendingTaskCount_;
        }
        return std::exchange(handle_, noopHandle());
    }

    void handOff() {
        detail::Promise<void>* p = promise_.release();
        if (p) {
            p->setExecutor(nursery_->executor());
            p->reparent(nursery_, nursery_->parent_);
            Nursery* n = std::exchange(nursery_, nullptr);
            --n->pendingTaskCount_;
            n->adopt(p);
            std::exchange(handle_, noopHandle()).resume();
        } else {
            // TaskStarted<> was invoked before promise construction,
            // so there's nothing to hand off to the nursery yet.
            //
            // StartAwaitable::await_suspend() will take care of submitting
            // the promise properly when it becomes available.
        }
    }

  protected:
    Nursery* nursery_;
    detail::Result<Ret> result_;
    Handle handle_ = noopHandle();
    detail::PromisePtr<void> promise_;
    Executor* executor_ = nullptr;
};

template <class Ret, class Callable, class... Args>
class Nursery::StartAwaitable : public StartAwaitableBase<Ret> {
    friend Nursery;

  public:
    StartAwaitable(StartAwaitable&&) = default;
    StartAwaitable& operator=(StartAwaitable&&) = delete;

    bool await_early_cancel() noexcept { return false; }
    bool await_ready() const noexcept { return false; }

    void await_set_executor(Executor* ex) noexcept { this->executor_ = ex; }

    Handle await_suspend(Handle h) {
        CORRAL_TRACE("    ...Nursery::start() %p", this);
        detail::PromisePtr<void> promise{std::apply(
                [this](auto&&... args) {
                    return this->nursery_->makePromise(std::move(callable_),
                                                       std::move(args)...,
                                                       makeTaskStarted(this));
                },
                std::move(args_))};

        if (this->result_.completed()) {
            // TaskStarted<> was invoked before promise construction,
            // and handOff() was skipped; hand off the promise to the nursery
            // ourselves.
            std::exchange(this->nursery_, nullptr)->doStart(promise.release());
            return h;
        } else {
            ++this->nursery_->pendingTaskCount_;
            this->handle_ = h;
            promise->setExecutor(this->executor_);
            this->promise_ = std::move(promise);
            return this->promise_->start(this, h);
        }
    }

    Ret await_resume() && { return std::move(this->result_).value(); }
    bool await_cancel(Handle) noexcept {
        if (this->promise_) {
            this->promise_->cancel();
        }
        return false;
    }
    auto await_must_resume() const noexcept {
        return !this->result_.wasCancelled();
    }

    ~StartAwaitable() {
        if (this->nursery_) {
            std::apply(
                    [this](auto&&... args) {
                        this->nursery_->start(
                                std::move(callable_), std::move(args)...,
                                makeTaskStarted(
                                        static_cast<StartAwaitableBase<Ret>*>(
                                                nullptr)));
                    },
                    std::move(args_));
        }
    }

  private:
    StartAwaitable(Nursery* nursery, Callable&& callable, Args&&... args)
      : StartAwaitableBase<Ret>(nursery),
        callable_(std::forward<Callable>(callable)),
        args_(std::forward<Args>(args)...) {}

  private:
    Callable callable_;
    std::tuple<Args...> args_;
};

template <class Ret>
void TaskStarted<Ret>::operator()(detail::ReturnType<Ret> ret)
    requires(!std::is_same_v<Ret, void>)
{
    if (auto p = std::exchange(parent_, nullptr)) {
        p->result_.storeValue(std::forward<Ret>(ret));
        p->handOff();
    }
}

template <class Ret>
void TaskStarted<Ret>::operator()()
    requires(std::is_same_v<Ret, void>)
{
    if (auto p = std::exchange(parent_, nullptr)) {
        p->result_.storeSuccess();
        p->handOff();
    }
}

//
// Nursery
//

/// An exception_ptr value meaning the nursery has been cancelled due to an
/// explicit request. This won't result in any exception being propagated to the
/// caller, but any further tasks spawned into the nursery will get immediately
/// cancelled.
inline std::exception_ptr Nursery::cancellationRequest() {
    struct Tag {};
    static const std::exception_ptr ret = std::make_exception_ptr(Tag{});
    return ret;
}

inline void Nursery::rethrowException() {
    if (exception_ && exception_ != cancellationRequest()) {
        std::rethrow_exception(exception_);
    }
}

inline void Nursery::adopt(detail::BasePromise* promise) {
    CORRAL_ASSERT(executor_ && "Nursery is closed to new arrivals");
    CORRAL_TRACE("pr %p handed to nursery %p (%zu tasks total)", promise, this,
                 taskCount_ + 1);
    if (exception_) {
        promise->cancel();
    }
    tasks_.push_back(*promise);
    ++taskCount_;
}

template <class Ret>
inline Handle Nursery::addPromise(detail::Promise<Ret>* promise,
                                  TaskParent<Ret>* parent) {
    CORRAL_ASSERT(promise);
    adopt(promise);
    promise->setExecutor(executor_);
    return promise->start(parent, parent_);
}

template <class Callable, class... Args>
detail::Promise<void>* Nursery::makePromise(Callable callable, Args... args) {
    Task<void> ret;
    if constexpr ((std::is_reference_v<Callable> &&
                   std::is_invocable_r_v<Task<>, Callable>) ||
                  std::is_convertible_v<Callable, Task<> (*)()>) {
        // The awaitable is an async lambda (lambda that produces a Task<>)
        // and it either was passed by lvalue reference or it is stateless,
        // and no arguments were supplied.
        // In this case, we don't have to worry about the lifetime of its
        // captures, and can thus save an allocation here.
        ret = callable();
    } else {
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
            ret = [](Callable c, auto obj, auto... a) -> Task<> {
                if constexpr (std::is_pointer_v<decltype(obj)>) {
                    co_await (obj->*c)(std::move(a)...);
                } else if constexpr (detail::is_reference_wrapper_v<
                                             decltype(obj)>) {
                    co_await (obj.get().*c)(std::move(a)...);
                } else {
                    co_await (std::move(obj).*c)(std::move(a)...);
                }
            }(std::move(callable), std::move(args)...);
        } else {
            ret = [](Callable c, Args... a) -> Task<> {
                co_await (std::move(c))(std::move(a)...);
            }(std::move(callable), std::move(args)...);
        }
    }

    return ret.release();
}

template <class Callable, class... Args>
    requires(std::invocable<Callable, Args...> &&
             Awaitable<std::invoke_result_t<Callable, Args...>> &&
             !std::invocable<Callable, Args..., detail::TaskStartedTag>)
void Nursery::start(Callable callable, Args... args) {
    doStart(makePromise(std::forward<Callable>(callable),
                        std::forward<Args>(args)...));
}

template <class Ret /* = detail::Unspecified*/, class Callable, class... Args>
    requires(std::invocable<Callable, Args..., detail::TaskStartedTag> &&
             Awaitable<std::invoke_result_t<Callable,
                                            Args...,
                                            detail::TaskStartedTag>>)
Awaitable auto Nursery::start(Callable callable, Args... args) {
    if constexpr (std::is_same_v<Ret, detail::Unspecified>) {
        using Sig = detail::CallableSignature<Callable>;
        using TaskStartedArg = typename Sig::template Arg<Sig::Arity - 1>;
        using ResultType = typename TaskStartedArg::ResultType;
        return StartAwaitable<ResultType, Callable, Args...>(
                this, std::move(callable), std::move(args)...);
    } else {
        return StartAwaitable<Ret, Callable, Args...>(this, std::move(callable),
                                                      std::move(args)...);
    }
}

inline void Nursery::doCancel() {
    if (!executor_ || tasks_.empty()) {
        return;
    }

    // Task cancellation may modify tasks_ arbitrarily,
    // invalidating iterators to task being cancelled or its
    // neighbors, thereby making it impossible to traverse through
    // tasks_ safely; so defer calling cancel() through the executor.
    Executor* ex = executor_;
    ex->capture(
            [this] {
                for (detail::BasePromise& t : tasks_) {
                    executor_->schedule(
                            +[](detail::BasePromise* p) noexcept {
                                p->cancel();
                            },
                            &t);
                }
            },
            taskCount_);

    ex->runSoon();
}

inline void Nursery::cancel() {
    if (exception_) {
        return; // already cancelling
    }
    CORRAL_TRACE("nursery %p cancellation requested", this);
    if (!exception_) {
        exception_ = cancellationRequest();
    }
    doCancel();
}

inline void Nursery::storeException() {
    if (parent_ == nullptr) {
        // This is an UnsafeNursery that has not been join()'ed. There is no
        // one we can pass our exception to, so we have no choice but to...
        std::terminate();
    }
    bool needCancel = (!exception_);
    if (!exception_ || exception_ == cancellationRequest()) {
        exception_ = std::current_exception();
    }
    if (needCancel) {
        doCancel();
    }
}


inline Handle Nursery::continuation(detail::BasePromise* promise) noexcept {
    CORRAL_TRACE("pr %p done in nursery %p (%zu tasks remaining)", promise,
                 this, taskCount_ - 1);
    tasks_.erase(*promise);
    --taskCount_;

    Executor* executor = executor_;
    Handle ret = noopHandle();
    // NB: in an UnsafeNursery, parent_ is the task that called join(), or
    // nullptr if no one has yet
    if (tasks_.empty() && pendingTaskCount_ == 0 && parent_ != nullptr) {
        ret = std::exchange(parent_, nullptr);
        executor_ = nullptr; // nursery is now closed
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

//
// Logic for binding the nursery parent to the nursery
//

template <class Derived> class Nursery::ParentAwaitable {
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
    void await_resume() {
        CORRAL_TRACE("nursery %p done", &self().nursery_);
        self().nursery_.rethrowException();
    }
    bool await_must_resume() const noexcept {
        return self().nursery_.exception_ != Nursery::cancellationRequest();
    }
};

class UnsafeNursery::Awaitable : public Nursery::ParentAwaitable<Awaitable> {
    friend ParentAwaitable;
    friend UnsafeNursery;
    UnsafeNursery& nursery_;

    explicit Awaitable(UnsafeNursery& nursery) : nursery_(nursery) {}

  public:
    bool await_ready() const noexcept { return nursery_.executor_ == nullptr; }
    bool await_suspend(Handle h) {
        CORRAL_ASSERT(!nursery_.parent_);
        if (nursery_.tasks_.empty()) {
            // Just close the nursery, don't actually suspend
            nursery_.executor_ = nullptr;
            return false;
        }
        nursery_.parent_ = h;
        return true;
    }
    void await_introspect(detail::TaskTreeCollector& c) const noexcept {
        nursery_.await_introspect(c);
    }
};

inline corral::Awaitable<void> auto UnsafeNursery::join() {
    return Awaitable(*this);
}

//
// Nursery construction
//

template <class Callable>
class Nursery::Scope : public detail::NurseryScopeBase,
                       public Nursery::ParentAwaitable<Scope<Callable>>,
                       private detail::TaskParent<detail::NurseryBodyRetval> {
    class Impl : public Nursery {
      public:
        Impl() = default;
        Impl(Impl&&) noexcept = default;

        void introspect(detail::TaskTreeCollector& c) const noexcept {
            c.node("Nursery");
            for (auto& t : tasks_) {
                c.child(t);
            }
        }
    };

  public:
    explicit Scope(Callable&& c) : callable_(std::move(c)) {}

    void await_set_executor(Executor* ex) noexcept { nursery_.executor_ = ex; }

    bool await_ready() const noexcept { return false; }

    Handle await_suspend(Handle h) {
        nursery_.parent_ = h;
        Task<detail::NurseryBodyRetval> body = callable_(nursery_);
        CORRAL_TRACE("    ... nursery %p starting with task %p", &nursery_,
                     body.promise_.get());
        return nursery_.addTask(std::move(body), this);
    }

    void await_introspect(detail::TaskTreeCollector& c) const noexcept {
        nursery_.introspect(c);
    }

  private:
    void storeValue(detail::NurseryBodyRetval retval) override {
        if (std::holds_alternative<detail::CancelTag>(retval)) {
            nursery_.cancel();
        }
    }
    void storeException() override { nursery_.storeException(); }
    Handle continuation(detail::BasePromise* promise) noexcept override {
        return nursery_.continuation(promise);
    }

  private:
    friend Nursery::ParentAwaitable<Scope>; // so it can access nursery_
    [[no_unique_address]] Callable callable_;
    Impl nursery_;
};

struct Nursery::Factory {
    template <class Callable> auto operator%(Callable&& c) {
        return Scope<Callable>(std::forward<Callable>(c));
    };
};


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
/// the nursery is opened, and reset to nullptr when the
/// `openNursery()` task receives a cancellation request (which may be
/// slightly before the nursery closes). Does not return until cancelled.
inline Task<void> openNursery(Nursery*& ptr, TaskStarted<> started = {}) {
    CORRAL_WITH_NURSERY(nursery) {
        ptr = &nursery;
        detail::ScopeGuard guard([&] { ptr = nullptr; });
        started();
        co_await SuspendForever{};
        co_return join; // make MSVC happy
    };
}

} // namespace corral
