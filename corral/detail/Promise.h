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
class RethrowCurrentException;

/// An object that can serve as the parent of a task. It receives the task's
/// result (value, exception, or cancellation) and can indicate where
/// execution should proceed after the task completes. This is implemented
/// by TaskAwaiter (detail/TaskAwaiter.h) and Nursery.
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
    /// Type-erased cancellation related portion of awaiter interface.
    /// This captures how to cancel an awaiter the task is waiting
    /// on, if any; otherwise its storage may be reused for denoting
    /// task status (ready / running / cancelled).
    class TypeErasedAwaiter {
      public:
        template <class T>
        explicit TypeErasedAwaiter(T& object)
          : TypeErasedAwaiter(&object, functions<T>()) {}

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

        TypeErasedAwaiter(void* object, const IFunctions* functions)
          : object_(object), functions_(functions) {}

      private:
        void* object_ = nullptr;
        const IFunctions* functions_ = nullptr;
    };

    // TypeErasedAwaiter lives in a union
    static_assert(std::is_trivially_copyable_v<TypeErasedAwaiter>);
    static_assert(std::is_trivially_destructible_v<TypeErasedAwaiter>);

  public:
    BasePromise(BasePromise&&) = delete;
    BasePromise& operator=(BasePromise&&) = delete;

    void setExecutor(Executor* ex) { executor_ = ex; }

    /// Requests the cancellation of the running task.
    ///
    /// If its current awaitee (if any) supports cancellation,
    /// proxies the request to the awaitee; otherwise marks the task
    /// as pending cancellation, and any further `co_await`
    /// on a cancellable awaiter would result in immediate cancellation
    /// of the task.
    ///
    /// In either case, if the awaitee is in fact cancelled (as opposed to
    /// to completing its operation despite the cancellation request), the
    /// awaiting task will also terminate by cancellation, and so on
    /// up the stack.
    void cancel() {
        CORRAL_TRACE("pr %p cancellation requested", this);

        if (!hasAwaiter()) {
            // Mark pending cancellation; coroutine will be cancelled
            // at its next suspension point (for running coroutines) or when
            // executed by executor (for ready coroutines). This is a no-op
            // if cancel() was already called.
            cancelState_ = CancelState::Requested;
        } else {
            // Coroutine currently suspended, so intercept the flow at
            // its resume point, and forward cancellation request to the
            // awaitee
            onResume<&BasePromise::doResumeAfterCancel>();
            if (awaiter_.cancel(proxyHandle())) {
                propagateCancel();
            }
        }
    }

    /// Destroys the promise and any locals within the coroutine frame.
    /// Only safe to call on not-yet-started tasks or those
    /// already completed (i.e., whose parent has resumed).
    void destroy() {
        if (hasCoroutine()) {
            realHandle().destroy();
        } else {
            // Call the `TaskFrame::destroyFn` filled in by makeStub().
            // This is the only place where that's actually a function
            // pointer; normally we use it as a parent-task link.
            proxyHandle().destroy();
        }
    }

    void await_introspect(TaskTreeCollector& c) const noexcept {
        if (!hasCoroutine()) {
            c.node("<noop>");
            return;
        }
        c.taskPC(pc);
        if (state_ == State::Ready) {
            c.footnote("<SCHEDULED>");
        } else if (state_ == State::Running) {
            c.footnote("<ON CPU>");
        } else {
            CORRAL_ASSERT(hasAwaiter());
            awaiter_.introspect(c);
        }
    }

    bool checkImmediateResult(BaseTaskParent* parent) noexcept {
        if (!hasCoroutine()) {
            // If we have a value to provide immediately, then provide
            // it without a trip through the executor
            parent_ = parent;
            // Invoke callback stashed by makeStub()
            CoroutineFrame::resumeFn(this);
            // Make sure it's only called once
            CoroutineFrame::resumeFn = +[](CoroutineFrame*) {};
            return true;
        }
        return false;
    }

  protected:
    BasePromise() {
        CORRAL_TRACE("pr %p created", this);
        state_ = State::Ready;
        cancelState_ = CancelState::None;
    }

    ~BasePromise() { CORRAL_TRACE("pr %p destroyed", this); }

    /// Replaces the parent of an already started task.
    void reparent(BaseTaskParent* parent, Handle caller) {
        parent_ = parent;
        linkTo(caller);
    }

    /// Returns a handle which would schedule task startup if resume()d or
    /// returned from an await_suspend() elsewhere.
    /// `parent` is an entity which arranged the execution and which will get
    /// notified (through parent->continuation().resume()) upon coroutine
    /// completion.
    Handle start(BaseTaskParent* parent, Handle caller) {
        if (checkImmediateResult(parent)) {
            return parent->continuation(this);
        }
        reparent(parent, caller);
        CORRAL_TRACE("pr %p started", this);
        onResume<&BasePromise::doResume>();
        return proxyHandle();
    }
    // NOLINTNEXTLINE(clang-analyzer-core.uninitialized.UndefReturn)
    BaseTaskParent* parent() const noexcept { return parent_; }

    /// Cause this promise to not resume a coroutine when it is started.
    /// Instead, it will invoke the given callback and then resume its parent.
    /// This can be used to create promises that are not associated with a
    /// coroutine; see just() and noop(). Must be called before start().
    template <class Derived, void (Derived::* onStart)()>
    void makeStub(bool deleteThisOnDestroy) {
        CORRAL_ASSERT(state_ == State::Ready && parent_ == nullptr);
        state_ = State::Stub;
        pc = 0;

        // Since stub promises never use their inline CoroutineFrame,
        // we can reuse them to store callbacks for start and destroy
        CoroutineFrame::resumeFn = +[](CoroutineFrame* self) {
            (static_cast<Derived*>(self)->*onStart)();
        };
        if (deleteThisOnDestroy) {
            CoroutineFrame::destroyFn = +[](CoroutineFrame* self) {
                delete static_cast<Derived*>(self);
            };
        } else {
            CoroutineFrame::destroyFn = +[](CoroutineFrame* self) {};
        }
    }

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

    bool hasAwaiter() const noexcept { return state_ > State::Stub; }
    bool hasCoroutine() const noexcept { return state_ != State::Stub; }

    template <void (BasePromise::* trampolineFn)()> void onResume() {
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
        if (hasAwaiter() && awaiter_.mustResume()) {
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

    /// Hooks onto Aw::await_suspend() and keeps track of the awaiter
    /// so cancellation can be arranged if necessary.
    ///
    /// This function is called when *this* task blocks on an
    /// awaiter.
    template <class Awaiter> Handle hookAwaitSuspend(Awaiter& awaiter) {
        bool cancelRequested = (cancelState_ == CancelState::Requested);
        CORRAL_TRACE("pr %p suspended%s on...", this,
                     cancelRequested ? " (with pending cancellation)" : "");

        awaiter_ = BasePromise::TypeErasedAwaiter(awaiter);
        // this resets cancelState_

        if (cancelRequested) {
            if (awaitEarlyCancel(awaiter)) {
                CORRAL_TRACE("    ... early-cancelled awaiter (skipped)");
                propagateCancel();
                return std::noop_coroutine();
            }
            onResume<&BasePromise::doResumeAfterCancel>();
            if (awaiter.await_ready()) {
                CORRAL_TRACE("    ... already-ready awaiter");
                return proxyHandle();
            }
        } else {
            onResume<&BasePromise::doResume>();
        }
        awaiter.await_set_executor(executor_);

        try {
            return detail::awaitSuspend(awaiter, proxyHandle());
        } catch (...) {
            CORRAL_TRACE("pr %p: exception thrown from await_suspend", this);
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

    // These enums live in a union with TypeErasedAwaiter, so their values must
    // be distinguishable from the possible object representations of an
    // TypeErasedAwaiter. TypeErasedAwaiter consists of two non-null pointers.
    // The first (TypeErasedAwaiter::object_, aliased with State) is not
    // aligned, but we can reasonably assume that 0x1 and 0x2 are not valid
    // addresses. The second (TypeErasedAwaiter::functions_, aliased with
    // CancelState) is aligned to a word size.
    enum class State : size_t { Ready = 0, Running = 1, Stub = 2 };
    enum class CancelState : size_t { None = 0, Requested = 1 };

    /// Possible values of state_:
    /// - State::Stub for promises associated with no coroutine,
    ///   implementing just(T) or noop()
    /// - State::Ready for tasks scheduled for execution
    ///   (i.e., whose proxyHandle() resume()d)
    /// - State::Running for tasks being executed at the moment
    ///   (i.e., whose realHandle() resume()d);
    /// otherwise the task is suspended on an awaiter, and awaiter_
    /// is populated accordingly.
    ///
    /// Furthermore, for running or ready tasks,
    /// cancelState == CancelState::Requested if cancel() has been called
    /// (such a task will get cancelled as soon as possible).
    union {
        TypeErasedAwaiter awaiter_;
        struct {
            State state_;
            CancelState cancelState_;
        };
    };

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
    template <class Awaitable> class AwaitProxy {
        // Use decltype instead of AwaiterType in order to reference
        // (rather than moving) an incoming Awaitable&& that is
        // Awaiter; even if it's a temporary, it will live for
        // the entire co_await expression including suspension.
        using AwaiterType = decltype(getAwaiter(std::declval<Awaitable>()));

      public:
        AwaitProxy(BasePromise* promise, Awaitable&& awaitable)
          : awaiter_(std::forward<Awaitable>(awaitable)), promise_(promise) {}

        bool await_ready() const noexcept {
            // NOLINTNEXTLINE(clang-analyzer-core.UndefinedBinaryOperatorResult)
            if (promise_->cancelState_ == CancelState::Requested) {
                // If the awaiting task has pending cancellation, we want
                // to execute the more involved logic in hookAwaitSuspend().
                return false;
            } else {
                return awaiter_.await_ready();
            }
        }

        CORRAL_NOINLINE auto await_suspend(Handle) {
            promise_->pc = reinterpret_cast<uintptr_t>(CORRAL_RETURN_ADDRESS());
            return promise_->hookAwaitSuspend(awaiter_);
        }

        decltype(auto) await_resume() { return awaiter_.await_resume(); }

      private:
        [[no_unique_address]] SanitizedAwaiter<Awaitable, AwaiterType> awaiter_;
        BasePromise* promise_;
    };

  public:
    CORRAL_NOINLINE auto initial_suspend() noexcept {
        pc = reinterpret_cast<uintptr_t>(CORRAL_RETURN_ADDRESS());
        return std::suspend_always{};
    }

    static auto final_suspend() noexcept { return FinalSuspendProxy(); }

    template <class Awaitable>
    auto await_transform(Awaitable&& awaitable) noexcept {
        static_assert(
                !std::is_same_v<std::decay_t<Awaitable>, FinalSuspendProxy>);
        // Note: intentionally not constraining Awaitable here to get a nicer
        // compilation error (constraint will be checked in getAwaiter()).
        return AwaitProxy<Awaitable>(this, std::forward<Awaitable>(awaitable));
    }

    friend RethrowCurrentException;
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

    void reparent(TaskParent<T>* parent, Handle caller) {
        BasePromise::reparent(parent, caller);
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

/// The promise type for a task that is not backed by a coroutine and
/// immediately returns a value of type T when invoked. Used by just()
/// and noop().
template <class T> class StubPromise : public Promise<T> {
  public:
    explicit StubPromise(T value) : value_(std::forward<T>(value)) {
        this->template makeStub<StubPromise, &StubPromise::onStart>(
                /* deleteThisOnDestroy = */ true);
    }

  private:
    void onStart() { this->return_value(std::forward<T>(value_)); }
    T value_;
};
template <> class StubPromise<void> : public Promise<void> {
  public:
    static StubPromise& instance() {
        static StubPromise inst;
        return inst;
    }

  private:
    StubPromise() {
        this->template makeStub<StubPromise, &StubPromise::onStart>(
                /* deleteThisOnDestroy = */ false);
    }
    void onStart() { this->return_void(); }
};

struct DestroyPromise {
    template <class T> void operator()(Promise<T>* p) const { p->destroy(); }
};

template <class T>
using PromisePtr = std::unique_ptr<Promise<T>, DestroyPromise>;

} // namespace corral::detail
