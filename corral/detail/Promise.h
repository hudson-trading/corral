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
#include <stddef.h>

#include "../Executor.h"
#include "../config.h"
#include "../defs.h"
#include "../utility.h"
#include "ABI.h"
#include "IntrusiveList.h"
#include "ScopeGuard.h"
#include "frames.h"
#include "introspect.h"
#include "platform.h"

namespace corral::detail {

class BasePromise;
template <class T> class Promise;
class NurseryScopeBase {};

/// An object that can serve as the parent of a task. It receives the task's
/// result (value, exception, or cancellation) and can indicate where
/// execution should proceed after the task completes. This is implemented
/// by TaskAwaitable (detail/task_awaitables.h) and Nursery.
///
/// BaseTaskParent contains the parts that do not depend on the task's return
/// type.
class BaseTaskParent {
  public:
    /// Called when a task finishes execution (either storeValue() or
    /// storeException() will be called before). Returns a coroutine handle to
    /// chain execution to (or std::noop_coroutine()).
    virtual Handle continuation(BasePromise*) noexcept = 0;

    /// Called when task execution ended with an unhandled exception
    /// (available through std::current_exception()).
    virtual void storeException() = 0;

    /// Called when task confirms its cancellation.
    virtual void cancelled() {}

  protected:
    /*non-virtual*/ ~BaseTaskParent() = default;
};

/// An object that can serve as the parent of a task that returns T.
/// See BaseTaskParent.
template <class T> class TaskParent : public BaseTaskParent {
  public:
    /// Called when task exited normally and returned a value.
    virtual void storeValue(T) = 0;

  protected:
    ~TaskParent() = default;
};

/// An object that can serve as the parent of a task that returns void.
/// See BaseTaskParent.
template <> class TaskParent<void> : public BaseTaskParent {
  public:
    /// Called when task exited normally.
    virtual void storeSuccess() = 0;

  protected:
    ~TaskParent() = default;
};

/// The promise type for a corral coroutine. (Promise<T> adds the parts
/// that depend on the task's return type.)
class BasePromise : private TaskFrame, public IntrusiveListItem<BasePromise> {
    /// Type-erased cancellation related portion of awaitable interface.
    /// This captures how to cancel an awaitable the task is waiting
    /// on, if any; otherwise its storage may be reused for denoting
    /// task status (ready / running / cancelled).
    class Awaitee {
      public:
        template <class T>
        explicit Awaitee(T& object) : Awaitee(&object, functions<T>()) {}

        bool cancel(Handle h) noexcept {
            return functions_->cancel(object_, h);
        }
        bool mustResume() const noexcept {
            return functions_->mustResume(object_);
        }
        void introspect(TaskTreeCollector& c) const noexcept {
            return functions_->introspect(object_, c);
        }

      private:
        struct IFunctions {
            virtual bool cancel(void* aw, Handle h) const noexcept = 0;
            virtual bool mustResume(const void* aw) const noexcept = 0;
            virtual void introspect(const void* aw,
                                    TaskTreeCollector& c) const noexcept = 0;
        };

        template <class T> struct Functions : IFunctions {
            bool cancel(void* aw, Handle h) const noexcept override {
                return awaitCancel(*reinterpret_cast<T*>(aw), h);
            }
            bool mustResume(const void* aw) const noexcept override {
                return awaitMustResume(*reinterpret_cast<const T*>(aw));
            }
            void introspect(const void* aw,
                            TaskTreeCollector& c) const noexcept override {
                c.child(*static_cast<const T*>(aw));
            }
        };

        template <class T> static const IFunctions* functions() {
            static const Functions<T> ret;
            return &ret;
        }

        Awaitee(void* object, const IFunctions* functions)
          : object_(object), functions_(functions) {}

      private:
        void* object_ = nullptr;
        const IFunctions* functions_ = nullptr;
    };

    // Awaitee lives in a union
    static_assert(std::is_trivially_copyable_v<Awaitee>);
    static_assert(std::is_trivially_destructible_v<Awaitee>);

  public:
    BasePromise(BasePromise&&) = delete;
    BasePromise& operator=(BasePromise&&) = delete;

    void setExecutor(Executor* ex) { executor_ = ex; }

    /// Requests the cancellation of the running task.
    ///
    /// If its current awaitee (if any) supports cancellation,
    /// proxies the request to the awaitee; otherwise marks the task
    /// as pending cancellation, and any further `co_await`
    /// on a cancellable awaitable would result in immediate cancellation
    /// of the awaitable.
    ///
    /// In either case, if the awaitee is in fact cancelled (as opposed to
    /// to completing its operation despite the cancellation request), the
    /// awaiting task will also terminate by cancellation, and so on
    /// up the stack.
    void cancel() {
        CORRAL_TRACE("pr %p cancellation requested", this);

        if (cancelState_ == CancelState::Requested) {
            // Do nothing

        } else if (state_ == State::Ready || state_ == State::Running) {
            // Mark pending cancellation; coroutine will be cancelled
            // at its next suspension point (for running coroutines) or when
            // executed by executor (for ready coroutines)
            cancelState_ = CancelState::Requested;

        } else {
            // Coroutine currently suspended, so intercept the flow at
            // its resume point, and forward cancellation request to the
            // awaitee
            onResume<&BasePromise::doResumeAfterCancel>();
            Handle h = checker_.aboutToCancel(proxyHandle());
            if (checker_.cancelReturned(awaitee_.cancel(h))) {
                propagateCancel();
            }
        }
    }

    /// Destroys the promise and any locals within the coroutine frame.
    /// Only safe to call on not-yet-started tasks or those
    /// already completed (i.e., whose parent has resumed).
    void destroy() { realHandle().destroy(); }

    void await_introspect(TaskTreeCollector& c) const noexcept {
        c.taskPC(pc);
        if (state_ == State::Ready) {
            c.footnote("<SCHEDULED>");
        } else if (state_ == State::Running) {
            c.footnote("<ON CPU>");
        } else {
            awaitee_.introspect(c);
        }
    }

  protected:
    BasePromise() {
        CORRAL_TRACE("pr %p created", this);
        state_ = State::Ready;
        cancelState_ = CancelState::None;
    }

    ~BasePromise() { CORRAL_TRACE("pr %p destroyed", this); }

    /// Returns a handle which would schedule task startup if resume()d or
    /// returned from an await_suspend() elsewhere.
    /// `parent` is an entity which arranged the execution and which will get
    /// notified (through parent->continuation().resume()) upon coroutine
    /// completion.
    Handle start(BaseTaskParent* parent, Handle caller) {
        CORRAL_TRACE("pr %p started", this);
        onResume<&BasePromise::doResume>();
        parent_ = parent;
        linkTo(caller);
        return proxyHandle();
    }
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    BaseTaskParent* parent() const noexcept { return parent_; }

  private /*methods*/:
    /// Returns a handle which, when resume()d, will immediately execute
    /// the next step of the task.
    CoroutineHandle<BasePromise> realHandle() noexcept {
        // NB: this is technically UB, as C++ does not allow up- or
        // downcasting coroutine_handle<>s. However, on any reasonable
        // implementation coroutine_handle<>::from_promise just shifts
        // promise address by a fixed, implementation-defined offset;
        // so provided BasePromise is the first base of the most derived
        // promise type, this should work fine.
        return CoroutineHandle<BasePromise>::from_promise(*this);
    }

    /// Returns a handle which, when resume()d, will schedule the next
    /// step of the task to run through its executor (or cancel it, if
    /// the cancellation is pending). This is the type of handle that
    /// is passed to awaitees.
    Handle proxyHandle() noexcept { return CoroutineFrame::toHandle(); }

    bool hasAwaitee() const noexcept {
        return state_ != State::Ready && state_ != State::Running;
    }

    template <void (BasePromise::*trampolineFn)()> void onResume() {
        CoroutineFrame::resumeFn = +[](CoroutineFrame* frame) {
            auto promise = static_cast<BasePromise*>(frame);
            (promise->*trampolineFn)();
        };
    }

    void doNothing() {
        CORRAL_TRACE("pr %p already scheduled, skipping", this);
    }

    /// Called if anybody resume()s proxyHandle(), i.e., this task's
    /// awaitee completed or the handle returned from start() was resumed.
    void doResume() {
        CORRAL_TRACE("pr %p scheduled", this);
        state_ = State::Ready;

        // Prevent further doResume()s from scheduling the task again
        onResume<&BasePromise::doNothing>();

        executor_->runSoon(
                +[](void* arg) noexcept {
                    auto h = CoroutineHandle<BasePromise>::from_address(arg);
                    BasePromise& self = h.promise();
                    CORRAL_TRACE("pr %p resumed", &self);
                    self.state_ = State::Running;
                    auto guard = self.executor_->markActive(self.proxyHandle());
                    h.resume();
                },
                realHandle().address());
    }

    /// Called when this task's awaitee completes after its cancellation was
    /// requested but didn't succeed immediately.
    void doResumeAfterCancel() {
        if (hasAwaitee() &&
            checker_.mustResumeReturned(awaitee_.mustResume())) {
            // This awaitee completed normally, so don't propagate the
            // cancellation. Attempt it again on the next co_await.
            cancelState_ = CancelState::Requested;
            doResume();
        } else {
            propagateCancel();
        }
    }

    /// Actually propagate a cancellation; called if await_cancel()
    /// returns true or if we're resumed after cancelling our awaitee
    /// and its await_must_resume() returns false.
    void propagateCancel() {
        CORRAL_TRACE("pr %p cancelled", this);
        BaseTaskParent* parent = std::exchange(parent_, nullptr);
        parent->cancelled();
        parent->continuation(this).resume();
    }

    /// Hooks onto Aw::await_suspend() and keeps track of the awaitable
    /// so cancellation can be arranged if necessary.
    ///
    /// This function is called when *this* task blocks on an
    /// Awaitable.
    template <class Awaitee> Handle hookAwaitSuspend(Awaitee& awaitee) {
        bool cancelRequested = (cancelState_ == CancelState::Requested);
        CORRAL_TRACE("pr %p suspended%s on...", this,
                     cancelRequested ? " (with pending cancellation)" : "");
        awaitee_ = BasePromise::Awaitee(awaitee); // this resets cancelState_

        if (cancelRequested) {
            if (checker_.earlyCancelReturned(awaitEarlyCancel(awaitee))) {
                CORRAL_TRACE("    ... early-cancelled awaitee (skipped)");
                propagateCancel();
                return std::noop_coroutine();
            }
            onResume<&BasePromise::doResumeAfterCancel>();
            if (checker_.readyReturned(awaitee.await_ready())) {
                CORRAL_TRACE("    ... already-ready awaitee");
                return proxyHandle();
            }
        } else {
            onResume<&BasePromise::doResume>();
        }
        checker_.aboutToSetExecutor();
        if constexpr (NeedsExecutor<Awaitee>) {
            awaitee.await_set_executor(executor_);
        }

        try {
            return detail::awaitSuspend(awaitee,
                                        checker_.aboutToSuspend(proxyHandle()));
        } catch (...) {
            CORRAL_TRACE("pr %p: exception thrown from await_suspend", this);
            checker_.suspendThrew();
            state_ = State::Running;
            if (cancelRequested) {
                cancelState_ = CancelState::Requested;
            }
            throw;
        }
    }

    /// Called during finalization.
    Handle hookFinalSuspend() {
        CORRAL_TRACE("pr %p finished", this);
        BaseTaskParent* parent = std::exchange(parent_, nullptr);
        CORRAL_ASSERT(parent != nullptr);
        return parent->continuation(this);
    }

  private /*fields*/:
    Executor* executor_ = nullptr;
    BaseTaskParent* parent_ = nullptr;

    // These constants live in lower bits, so they can coexist with
    // an (aligned) pointer in a union, and one can tell them
    // from a pointer value.
    enum class State : size_t { Ready = 1, Running = 3 };
    enum class CancelState : size_t { None = 0, Requested = 1 };

    /// state_ == State::Ready for tasks scheduled for execution
    /// (i.e. whose proxyHandle() resume()d);
    /// state_ == State::Running for tasks being executed at the moment
    /// (i.e. whose realHandle() resume()d);
    /// otherwise the task is suspended on an awaitable, and awaitee_
    /// is populated accordingly.
    ///
    /// Furthermore, for running or ready tasks,
    /// cancelState == CancelState::Requested if cancel() has been called
    /// (such a task will get cancelled as soon as possible).
    union {
        Awaitee awaitee_;
        struct {
            State state_;
            CancelState cancelState_;
        };
    };
    [[no_unique_address]] AwaitableStateChecker checker_;

    //
    // Hooks
    //

  private:
    class FinalSuspendProxy : public std::suspend_always {
      public:
        template <class Promise>
        Handle await_suspend(CoroutineHandle<Promise> h) noexcept {
            return h.promise().hookFinalSuspend();
        }
    };

    /// A proxy which allows Promise to have control over its own suspension.
    template <class Awaitee> class AwaitProxy {
      public:
        AwaitProxy(BasePromise* promise, Awaitee&& wrapped) noexcept
          : awaitee_(std::forward<Awaitee>(wrapped)), promise_(promise) {}

        bool await_ready() const noexcept {
            promise_->checker_.reset();
            // NOLINTNEXTLINE(clang-analyzer-core.UndefinedBinaryOperatorResult)
            if (promise_->cancelState_ == CancelState::Requested) {
                // If the awaiting task has pending cancellation, we want
                // to execute the more involved logic in hookAwaitSuspend().
                return false;
            } else {
                return promise_->checker_.readyReturned(awaitee_.await_ready());
            }
        }
        CORRAL_NOINLINE auto await_suspend(Handle) {
            promise_->pc = reinterpret_cast<uintptr_t>(CORRAL_RETURN_ADDRESS());
            return promise_->hookAwaitSuspend(awaitee_);
        }

        decltype(auto) await_resume() {
            promise_->checker_.aboutToResume();
            return std::forward<Awaitee>(awaitee_).await_resume();
        }

      private:
        [[no_unique_address]] Awaitee awaitee_;
        BasePromise* promise_;
    };

  public:
    CORRAL_NOINLINE auto initial_suspend() noexcept {
        pc = reinterpret_cast<uintptr_t>(CORRAL_RETURN_ADDRESS());
        return std::suspend_always{};
    }

    static auto final_suspend() noexcept { return FinalSuspendProxy(); }

    template <class Awaitee> auto await_transform(Awaitee&& awaitee) noexcept {
        static_assert(
                !std::is_same_v<std::decay_t<Awaitee>, FinalSuspendProxy>);
        // Note: intentionally not constraining Awaitee here to get a nicer
        // compilation error (constraint will be checked in getAwaitable()).
        // Use decltype instead of AwaitableType in order to reference
        // (rather than moving) an incoming Awaitee&& that is
        // ImmediateAwaitable; even if it's a temporary, it will live for
        // the entire co_await expression including suspension.
        using Ret = decltype(getAwaitable(std::forward<Awaitee>(awaitee)));
        return AwaitProxy<Ret>(this,
                               getAwaitable(std::forward<Awaitee>(awaitee)));
    }
};

template <class T> class Promise;

template <class T> class ReturnValueMixin {
  public:
    void return_value(T value) {
        static_cast<Promise<T>*>(this)->parent()->storeValue(
                std::forward<T>(value));
    }
};

/// The promise type for a corral coroutine that returns T.
template <class T>
class Promise : public BasePromise, public ReturnValueMixin<T> {
  public:
    void unhandled_exception() { parent()->storeException(); }

    Task<T> get_return_object();

    Handle start(TaskParent<T>* parent, Handle caller) {
        return BasePromise::start(parent, caller);
    }

    /// Allows using `co_yield` instead of `co_await` for nursery factories.
    /// This is purely a syntactic trick to allow the
    /// `CORRAL_WITH_NURSERY(n) { ... }` syntax to work, by making it expand
    /// to a binary operator which binds more tightly than `co_yield`.
    /// (`co_await` binds too tightly.)
    template <std::derived_from<NurseryScopeBase> U> auto yield_value(U&& u) {
        return await_transform(std::forward<U>(u));
    }

  private:
    friend ReturnValueMixin<T>;
    TaskParent<T>* parent() {
        return static_cast<TaskParent<T>*>(BasePromise::parent());
    }
};

template <> class ReturnValueMixin<void> {
  public:
    void return_void() {
        static_cast<Promise<void>*>(this)->parent()->storeSuccess();
    }
};

/// A tag value for a promise object which should do nothing and
/// immediately wake up its parent if co_await'ed.  Note that the
/// object only exists to provide a unique and valid address. It does
/// not represent any task in a valid state. and calling *any* of its
/// member functions is UB.
inline Promise<void>* noopPromise() {
    static Promise<void> p;
    return &p;
}

} // namespace corral::detail
