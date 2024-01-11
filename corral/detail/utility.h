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
#include <optional>
#include <utility>
#include <version>

#include "../concepts.h"
#include "../config.h"
#include "../defs.h"
#include "frames.h"
#include "introspect.h"

namespace corral {

template <class T> class Task;
class Executor;

namespace detail {

/// Base class to inherit Task<T> from, to easily figure out if something
/// is a Task.
struct TaskTag {};

[[noreturn]] inline void unreachable() {
#if __cpp_lib_unreachable
    std::unreachable();
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_unreachable();
#elif defined(_MSC_VER)
    __assume(false);
#endif // would still invoke UB because of [[noreturn]]
}

/// Like std::conditional, but for templates.
template <bool If,
          template <class...>
          class Then,
          template <class...>
          class Else>
struct ConditionalTmpl {
    template <class... Args> using With = Then<Args...>;
};
template <template <class...> class Then, template <class...> class Else>
struct ConditionalTmpl<false, Then, Else> {
    template <class... Args> using With = Else<Args...>;
};

template <class T> struct RemoveRvalueReference {
    using type = T;
};
template <class T> struct RemoveRvalueReference<T&> {
    using type = T&;
};
template <class T> struct RemoveRvalueReference<T&&> {
    using type = T;
};
template <class T>
using RemoveRvalueReference_t = typename RemoveRvalueReference<T>::type;

static_assert(std::is_same_v<RemoveRvalueReference_t<const int&>, const int&>);
static_assert(std::is_same_v<RemoveRvalueReference_t<int&&>, int>);

/// Mimics what the compiler does to obtain an awaitable from whatever
/// is passed to co_await, plus a fallback to support AwaitableLambda:
/// any suitable `corral::detail::operator co_await(T&&)` will be
/// considered even if it would not be found via ADL, as long as the
/// `operator co_await` is declared before `getAwaitable()` is defined.
/// You will need a corresponding 'ThisIsAwaitableTrustMe'
/// specialization in order to make the object satisfy Awaitable, since
/// the Awaitable concept was declared before the `operator co_await`.
///
/// The return type of this function is as follows:
/// - If T&& is ImmediateAwaitable, then T&&. (Like std::forward: you get
///   a lvalue or rvalue reference depending on the value category of `t`,
///   and no additional object is created.)
/// - If T&& defines operator co_await() returning value type A or rvalue
///   reference A&&, then A. (The awaitable is constructed or moved into
///   the return value slot.)
/// - If T&& defines operator co_await() returning lvalue reference A&,
///   then A&. (We do not make a copy.)
///
/// It is important to pay attention to the value category in order to
/// avoid a dangling reference if a function constructs a combination of
/// awaitables and then returns it. Typically the return value of
/// getAwaitable(T&&) should be used to initialize an object of type
/// AwaitableType<T&&>; AwaitableType will be a value type or lvalue
/// reference, but not an rvalue reference.
template <class T> decltype(auto) getAwaitable(T&& t);

/// Returns the type that getAwaitable() would return, stripped of any
/// rvalue-reference part (so you might get T or T&, but not T&&). This
/// is the appropriate type to store in an object that wraps another
/// awaitable(s).
template <class Aw>
using AwaitableType =
        RemoveRvalueReference_t<decltype(getAwaitable(std::declval<Aw>()))>;


#if !defined(CORRAL_AWAITABLE_STATE_DEBUG)

// A runtime validator for the awaitable state machine. This version
// should compile to nothing and be empty. The version in the other
// branch of the #ifdef has the actual checking logic.
// These are all noexcept so that any exceptions thrown immediately
// crash the program.
struct AwaitableStateChecker {
    // Mark the end of using this checker to process a particular awaitable.
    // Not necessary if it only handles one awaitable during its lifetime.
    void reset() noexcept {}

    // Like reset(), but don't check that the awaitable is in a valid state
    // to abandon.
    void forceReset() {}

    // Note that await_ready() returned the given value. Returns the
    // same value for convenience.
    auto readyReturned(auto val) const noexcept { return val; }

    // Note that await_early_cancel() returned the given value. Returns the
    // same value for convenience.
    auto earlyCancelReturned(auto val) noexcept { return val; }

    // Note that await_set_executor() is about to be invoked.
    void aboutToSetExecutor() noexcept {}

    // Transform a coroutine handle before passing it to await_suspend().
    Handle aboutToSuspend(Handle h) noexcept { return h; }

    // Note that await_suspend() threw an exception.
    void suspendThrew() noexcept {}

    // Transform a coroutine handle before passing it to await_cancel().
    Handle aboutToCancel(Handle h) noexcept { return h; }

    // Note that await_cancel() returned the given value. Returns the
    // same value for convenience.
    auto cancelReturned(auto val) noexcept { return val; }

    // Note that await_must_resume() returned the given value. Returns the
    // same value for convenience.
    auto mustResumeReturned(auto val) const noexcept { return val; }

    // Note that await_resume() is about to be invoked.
    void aboutToResume() noexcept {}
};

#else // defined(CORRAL_AWAITABLE_STATE_DEBUG)

struct AwaitableStateChecker : ProxyFrame {
    // See doc/02_adapting.md for much more detail on this state machine.
    enum class State {
        Initial,          // We haven't done anything with the awaitable yet
        NotReady,         // We called await_ready() and it returned false
        InitialCxlPend,   // Initial + await_early_cancel() returned false
        CancelPending,    // NotReady + await_early_cancel() returned false
        ReadyImmediately, // await_ready() returned true before await_suspend()
        Running,          // await_suspend() has started
        Cancelling,       // Running + await_cancel() returned false
        ReadyAfterCancel, // Resumed from Cancelling or ready after CxlPend;
                          // needs await_must_resume()
        Ready,            // Resumed from Running; needs await_resume()
        Cancelled,        // Operation complete, without result due to cancel
        Done,             // Operation complete with result (value or error)
    };
    Handle realHandle_;
    bool hasExecutor_ = false;
    mutable State state_ = State::Initial;

    AwaitableStateChecker() {
        this->resumeFn = +[](CoroutineFrame* frame) {
            auto* self = static_cast<AwaitableStateChecker*>(frame);
            switch (self->state_) {
                case State::Running:
                    self->state_ = State::Ready;
                    break;
                case State::Cancelling:
                    self->state_ = State::ReadyAfterCancel;
                    break;
                default:
                    CORRAL_ASSERT_UNREACHABLE();
                    break;
            }
            self->realHandle_.resume();
        };
    }
    ~AwaitableStateChecker() { reset(); }

    void reset() noexcept {
        // If you find that this assertion is firing in State::Ready or
        // State::ReadyImmediately, check whether you're writing a co_await
        // expression inside a CATCH_CHECK() macro or unevaluated portion
        // of a short-circuiting boolean expression. If you are, try not; it
        // runs into a terrible gcc bug which half-evaluates the unevaluated:
        // https://gcc.gnu.org/bugzilla/show_bug.cgi?id=112360
        CORRAL_ASSERT(state_ == State::Cancelled || state_ == State::Done ||
                      state_ == State::Initial);
        forceReset();
    }

    void forceReset() {
        state_ = State::Initial;
        hasExecutor_ = false;
    }

    auto readyReturned(auto val) const noexcept {
        switch (state_) {
            case State::Initial:
            case State::NotReady:
                state_ = val ? State::ReadyImmediately : State::NotReady;
                break;
            case State::InitialCxlPend:
            case State::CancelPending:
                state_ = val ? State::ReadyAfterCancel : State::CancelPending;
                break;
            case State::ReadyImmediately:
            case State::ReadyAfterCancel:
                // Redundant readiness check is allowed as long as we haven't
                // suspended yet (which hasExecutor is a proxy for) and
                // we don't backtrack in readiness
                CORRAL_ASSERT(!hasExecutor_ && val);
                break;
            default:
                CORRAL_ASSERT_UNREACHABLE();
                break;
        }
        return val;
    }
    auto earlyCancelReturned(auto val) noexcept {
        switch (state_) {
            case State::Initial:
                state_ = val ? State::Cancelled : State::InitialCxlPend;
                break;
            case State::NotReady:
                state_ = val ? State::Cancelled : State::CancelPending;
                break;
            case State::ReadyImmediately:
                state_ = val ? State::Cancelled : State::ReadyAfterCancel;
                break;
            default:
                CORRAL_ASSERT_UNREACHABLE();
                break;
        }
        return val;
    }
    void aboutToSetExecutor() noexcept {
        CORRAL_ASSERT(
                state_ == State::NotReady || state_ == State::CancelPending ||
                state_ == State::Initial || state_ == State::InitialCxlPend);
        hasExecutor_ = true;
    }
    Handle aboutToSuspend(Handle h) noexcept {
        CORRAL_ASSERT(hasExecutor_);
        switch (state_) {
            case State::NotReady:
                state_ = State::Running;
                break;
            case State::CancelPending:
                state_ = State::Cancelling;
                break;
            default:
                CORRAL_ASSERT_UNREACHABLE();
                break;
        }
        realHandle_ = h;
        this->linkTo(h);
        return this->toHandle();
    }
    void suspendThrew() noexcept {
        CORRAL_ASSERT(state_ == State::Running || state_ == State::Cancelling);
        state_ = State::Done;
    }
    Handle aboutToCancel(Handle h) noexcept {
        CORRAL_ASSERT(state_ == State::Running);
        CORRAL_ASSERT(realHandle_ == h);
        state_ = State::Cancelling;
        return this->toHandle();
    }
    auto cancelReturned(auto val) noexcept {
        if (val) {
            CORRAL_ASSERT(state_ == State::Cancelling);
            state_ = State::Cancelled;
        }
        return val;
    }
    auto mustResumeReturned(auto val) const noexcept {
        CORRAL_ASSERT(state_ == State::ReadyAfterCancel);
        state_ = val ? State::Ready : State::Cancelled;
        return val;
    }
    void aboutToResume() noexcept {
        CORRAL_ASSERT(state_ == State::ReadyImmediately ||
                      state_ == State::Ready);
        state_ = State::Done;
    }
};

#endif // defined(CORRAL_AWAITABLE_STATE_DEBUG)

//
// Wrappers around await_*() awaitable functions
//

/// A sanitized version of await_suspend() which always returns a Handle.
template <class Aw, class Promise>
Handle awaitSuspend(Aw&& awaitable, CoroutineHandle<Promise> h) {
    // Note: Aw is unconstrained here, as awaitables requiring being
    // rvalue-qualified are still passed by lvalue (we're not consuming
    // them until await_resume()).
    using RetType = decltype(std::forward<Aw>(awaitable).await_suspend(h));
    if constexpr (std::is_same_v<RetType, void>) {
        std::forward<Aw>(awaitable).await_suspend(h);
        return std::noop_coroutine();
    } else if constexpr (std::is_convertible_v<RetType, Handle>) {
        return std::forward<Aw>(awaitable).await_suspend(h);
    } else {
        if (std::forward<Aw>(awaitable).await_suspend(h)) {
            return std::noop_coroutine();
        } else {
            return h;
        }
    }
}

template <class Aw> auto awaitEarlyCancel(Aw& awaitable) noexcept {
    if constexpr (CustomizesEarlyCancel<Aw>) {
        return awaitable.await_early_cancel();
    } else {
        return std::true_type{};
    }
}

template <class Aw> auto awaitCancel(Aw& awaitable, Handle h) noexcept {
    if constexpr (Cancellable<Aw>) {
        return awaitable.await_cancel(h);
    } else {
        return false;
    }
}

template <class Aw> auto awaitMustResume(const Aw& awaitable) noexcept {
    if constexpr (CustomizesMustResume<Aw>) {
        return awaitable.await_must_resume();
    } else if constexpr (CancelAlwaysSucceeds<Aw>) {
        return std::false_type{};
    } else {
        static_assert(!Cancellable<Aw>);
        return true;
    }
}

template <class Aw>
void awaitIntrospect(const Aw& awaitable, TaskTreeCollector& c) noexcept {
    if constexpr (Introspectable<Aw>) {
        awaitable.await_introspect(c);
    } else {
        c.node(&typeid(Aw));
    }
}


/// A quality-of-life adapter allowing passing lambdas returning Task<>
/// instead of tasks themselves, saving on a bunch of parentheses,
/// not driving clang-indent crazy, and (most importantly) not exposing
/// users to problems with lifetimes of lambda object themselves.
template <class Callable> class AwaitableLambda {
    using TaskT = std::invoke_result_t<Callable>;
    using AwaitableT = AwaitableType<TaskT>;

  public:
    explicit AwaitableLambda(Callable&& c)
      : callable_(std::forward<Callable>(c)) {}

    // NB: these forwarders are specialized for TaskAwaitable, and would
    // need generalization to support non-Task awaitables

    // We know that a TaskAwaitable will be not-ready (except corral::noop(),
    // but that one doesn't mind if you suspend on it anyway). We need to
    // initialize the awaitable before await_resume() gets called,
    // can't do it here since the method is const, and
    // await_set_executor() only runs if we're going to suspend.
    bool await_ready() const noexcept { return false; }

    void await_set_executor(Executor* ex) noexcept {
        if (!task_) {
            task_ = callable_();
            awaitable_ = task_.operator co_await();
        }
        awaitable_.await_set_executor(ex);
    }
    auto await_suspend(Handle h) { return awaitable_.await_suspend(h); }
    decltype(auto) await_resume() {
        return std::forward<AwaitableT>(awaitable_).await_resume();
    }

    auto await_early_cancel() noexcept {
        CORRAL_ASSERT(!task_);
        task_ = callable_();
        awaitable_ = task_.operator co_await();
        return awaitable_.await_early_cancel();
    }
    auto await_cancel(Handle h) noexcept { return awaitable_.await_cancel(h); }
    auto await_must_resume() const noexcept {
        return awaitable_.await_must_resume();
    }

    void await_introspect(auto& c) const noexcept {
        awaitable_.await_introspect(c);
    }

  private:
    Callable callable_;
    TaskT task_;
    AwaitableT awaitable_;
};

template <class Callable>
    requires(std::derived_from<std::invoke_result_t<Callable>, TaskTag>)
AwaitableLambda<Callable> operator co_await(Callable && c) {
    return AwaitableLambda<Callable>(std::forward<Callable>(c));
}

// The AwaitableLambda `operator co_await()` is not found via ADL and
// was not declared before `concept Awaitable`, so we need to
// specialize `ThisIsAwaitableTrustMe` in order to make callables
// returning Task<T> satisfy `Awaitable`. Note that compiler-generated
// co_await logic outside of `namespace corral::detail` would similarly not
// find it, but since our `BasePromise::await_transform()` uses
// `corral::detail::getAwaitable()`, corral tasks can await lambdas.
// We specifically _don't_ want to enable this for non-corral tasks, because
// they won't know to call `await_set_executor`, which prevents
// AwaitableLambda from working.
template <class Callable, class Ret>
    requires(std::derived_from<std::invoke_result_t<Callable>, TaskTag> &&
             (std::same_as<Ret, Unspecified> ||
              std::convertible_to<
                      typename std::invoke_result_t<Callable>::ReturnType,
                      Ret>) )
constexpr bool ThisIsAwaitableTrustMe<Callable, Ret> = true;

/// A utility awaitable to perform a function with the current task
/// temporarily suspended.
/// Can be used to add a suspension point.
template <class Callable, class ReturnType>
class [[nodiscard]] YieldImpl : private Callable {
  public:
    explicit YieldImpl(Callable cb) : Callable(std::move(cb)) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(Handle h) {
        result_ = Callable::operator()();
        return false;
    }
    ReturnType await_resume() { return std::move(result_); }
    void await_introspect(auto& c) const noexcept { c.node("Yield"); }

  private:
    ReturnType result_;
};

template <class Callable, class ReturnType>
class [[nodiscard]] YieldImpl<Callable, ReturnType&&> : private Callable {
  public:
    explicit YieldImpl(Callable cb) : Callable(std::move(cb)) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(Handle h) {
        result_ = &(Callable::operator()());
        return false;
    }
    ReturnType&& await_resume() { return static_cast<ReturnType&&>(result_); }
    void await_introspect(auto& c) const noexcept { c.node("Yield"); }

  private:
    ReturnType* result_ = nullptr;
};

template <class Callable>
class [[nodiscard]] YieldImpl<Callable, void> : private Callable {
  public:
    explicit YieldImpl(Callable cb) : Callable(std::move(cb)) {}

    bool await_ready() const noexcept { return false; }
    bool await_suspend(Handle h) {
        Callable::operator()();
        return false;
    }
    void await_resume() {}
    void await_introspect(auto& c) const noexcept { c.node("Yield"); }
};

template <class Callable>
using Yield = YieldImpl<Callable, std::invoke_result_t<Callable>>;

template <ValidImmediateAwaitable Aw> Aw staticAwaitableCheck(Aw&& aw) {
    return std::forward<Aw>(aw);
}

template <class T> decltype(auto) getAwaitable(T&& t) {
    static_assert(Awaitable<T>, "tried to co_await on not an awaitable");

    // clang-format off
    if constexpr (requires() {
            {std::forward<T>(t)} -> ImmediateAwaitable; }) {
        // Explicit template argument so we can preserve a provided rvalue
        // reference rather than moving into a new object. The referent
        // was created in the co_await expression so it will live long
        // enough for us and we can save a copy. Without the explicit argument,
        // universal reference rules would infer Aw = T (which is what we
        // want for the other calls where we're passing a temporary that was
        // created in this function).
        return staticAwaitableCheck<T&&>(std::forward<T>(t));
    } else if constexpr (requires() {
            {std::forward<T>(t).operator co_await()} -> ImmediateAwaitable; }) {
        return staticAwaitableCheck(std::forward<T>(t).operator co_await());
    } else if constexpr (requires() {
            {operator co_await(std::forward<T>(t))} -> ImmediateAwaitable; }) {
        return staticAwaitableCheck(operator co_await(std::forward<T>(t)));
    } else {
        // if !Awaitable<T>, then the static_assert above fired and we don't
        // need to fire this one also
        static_assert(!Awaitable<T>,
                      "co_await argument satisfies Awaitable concept but "
                      "we couldn't extract an ImmediateAwaitable from it");
        return std::suspend_never{};
    }
    // clang-format on
}


/// A dummy type used instead of void when temporarily storing results
/// of awaitables, to allow void results to be stored without specialization.
struct Void {};

template <class T>
using ReturnType = std::conditional_t<std::is_same_v<T, void>, Void, T>;

template <class Aw>
using AwaitableReturnType =
        ReturnType<decltype(std::declval<AwaitableType<Aw>>().await_resume())>;

/// An adapter which sanitizes an awaitable in these ways:
///   - its await_suspend() always returns a coroutine_handle<>;
///   - its await_resume() always returns something which can be stored
///     in a local variable or stuffed into std::variant or std::tuple;
///   - provides possibly-dummy versions of all optional await_*() methods:
///     await_set_executor, await_early_cancel, await_cancel,
///     await_must_resume, await_introspect
/// Many of the 'standardized' implementations for individual await_foo()
/// methods are available as detail::awaitFoo() also.
template <class Aw> struct AwaitableAdapter {
    static_assert(ImmediateAwaitable<Aw>,
                  "AwaitableAdapter must be initialized with an immediate "
                  "awaitable; pass your object through getAwaitable().");

    using Ret = decltype(std::declval<Aw>().await_resume());

  public:
    explicit AwaitableAdapter(Aw&& awaitable)
      : awaitable_(staticAwaitableCheck<Aw&&>(std::forward<Aw>(awaitable))) {}

    bool await_ready() const noexcept {
        return checker_.readyReturned(awaitable_.await_ready());
    }

    Handle await_suspend(Handle h) {
#ifdef CORRAL_AWAITABLE_STATE_DEBUG
        try {
            return awaitSuspend(awaitable_, checker_.aboutToSuspend(h));
        } catch (...) {
            checker_.suspendThrew();
            throw;
        }
#else
        return awaitSuspend(awaitable_, h);
#endif
    }

    decltype(auto) await_resume() {
        checker_.aboutToResume();
        if constexpr (std::is_same_v<Ret, void>) {
            std::forward<Aw>(awaitable_).await_resume();
            return Void{};
        } else {
            return std::forward<Aw>(awaitable_).await_resume();
        }
    }

    auto await_early_cancel() noexcept {
        return checker_.earlyCancelReturned(awaitEarlyCancel(awaitable_));
    }
    auto await_cancel(Handle h) noexcept {
        return checker_.cancelReturned(
                awaitCancel(awaitable_, checker_.aboutToCancel(h)));
    }
    auto await_must_resume() const noexcept {
        return checker_.mustResumeReturned(awaitMustResume(awaitable_));
    }

    void await_set_executor(Executor* ex) noexcept {
        checker_.aboutToSetExecutor();
        if constexpr (NeedsExecutor<Aw>) {
            awaitable_.await_set_executor(ex);
        }
    }

    void await_introspect(TaskTreeCollector& c) const noexcept {
        awaitIntrospect(awaitable_, c);
    }

    // Used by Runner::run() if the event loop stops before the
    // awaitable completes. Disables the awaitable checker (if any),
    // allowing the awaitable to be destroyed even in states where it
    // shouldn't be.
    void abandon() { checker_.forceReset(); }

  private:
    [[no_unique_address]] AwaitableStateChecker checker_;
    Aw awaitable_;
};


/// A common part for corral::noncancellable() and corral::disposable().
template <class Aw> class CancellableAdapter {
  public:
    explicit CancellableAdapter(Aw&& awaitable)
      : awaitable_(std::forward<Aw>(awaitable)) {}

    void await_set_executor(Executor* ex) noexcept {
        if constexpr (NeedsExecutor<Aw>) {
            awaitable_.await_set_executor(ex);
        }
    }

    bool await_ready() const noexcept { return awaitable_.await_ready(); }
    bool await_early_cancel() noexcept { return false; }
    auto await_suspend(Handle h) { return awaitable_.await_suspend(h); }
    bool await_must_resume() const noexcept { return true; }
    decltype(auto) await_resume() & { return awaitable_.await_resume(); }
    decltype(auto) await_resume() && {
        return std::move(awaitable_).await_resume();
    }

    void await_introspect(TaskTreeCollector& c) const noexcept {
        c.node("Noncancellable");
        c.child(c);
    }

  protected:
    Aw awaitable_;
};

/// A wrapper around an awaitable declaring that its return value
/// is safe to dispose of upon cancellation.
/// May be used on third party awaitables which don't know about
/// corral async's cancellation mechanism.
template <class Aw>
class DisposableAdapter : public detail::CancellableAdapter<Aw> {
  public:
    explicit DisposableAdapter(Aw&& awaitable)
      : detail::CancellableAdapter<Aw>(std::forward<Aw>(awaitable)) {}

    auto await_early_cancel() noexcept {
        return awaitEarlyCancel(this->awaitable_);
    }
    auto await_cancel(Handle h) noexcept {
        return awaitCancel(this->awaitable_, h);
    }
    auto await_must_resume() const noexcept { return std::false_type{}; }

    void await_introspect(TaskTreeCollector& c) const noexcept {
        c.node("Disposable");
        c.child(c);
    }
};

/// A utility class allowing expressing things like SuspendAlways
/// as a global constant.
template <class T> class CoAwaitFactory {
  public:
    T operator co_await() const noexcept { return T{}; }
};

/// A utility class kicking off an awaitable upon cancellation.
template <class Aw> class RunOnCancel {
  public:
    explicit RunOnCancel(Aw&& awaitable)
      : awaitable_(std::forward<Aw>(awaitable)) {}

    void await_set_executor(Executor* ex) noexcept {
        awaitable_.await_set_executor(ex);
    }

    bool await_ready() const noexcept { return false; }
    bool await_early_cancel() noexcept {
        cancelPending_ = true;
        return false;
    }
    void await_suspend(Handle h) {
        if (cancelPending_) {
            this->await_cancel(h);
        }
    }
    [[noreturn]] void await_resume() { detail::unreachable(); }

    bool await_cancel(Handle h) noexcept {
        // If the awaitable immediately resumes, then this is sort of
        // like a synchronous cancel, but we still need to structure
        // it as "resume the handle ourselves, then return false" in
        // order to make sure await_must_resume() gets called to check
        // for exceptions.
        if (awaitable_.await_ready()) {
            h.resume();
        } else {
            awaitable_.await_suspend(h).resume();
        }
        return false;
    }
    auto await_must_resume() const noexcept {
        awaitable_.await_resume(); // terminate() on any pending exception
        return std::false_type{};
    }

    void await_introspect(TaskTreeCollector& c) const noexcept {
        c.node("RunOnCancel");
        c.child(awaitable_);
    }

  protected:
    [[no_unique_address]] mutable AwaitableAdapter<Aw> awaitable_;
    bool cancelPending_ = false;
};

/// A set of helpers which allow storing rvalue and lvalue references,
/// thus allowing them to appear in return types of tasks and awaitables.
template <class T> struct Storage {
  public:
    using Type = T;
    static T&& wrap(T&& value) { return std::move(value); }
    static T&& unwrap(T&& stored) { return std::move(stored); }
    static const T& unwrapCRef(const T& stored) { return stored; }
};

template <class T> struct Storage<T&> {
    using Type = T*;
    static T* wrap(T& value) { return &value; }
    static T& unwrap(T* stored) { return *stored; }
    static const T& unwrapCRef(T* stored) { return *stored; }
};

template <class T> struct Storage<T&&> {
    using Type = T*;
    static T* wrap(T&& value) { return &value; }
    static T&& unwrap(T* stored) { return std::move(*stored); }
    static const T& unwrapCRef(T* stored) { return *stored; }
};

template <> struct Storage<void> {
    struct Type {};
    // wrap nod defined
    static void unwrap(Type) {}
    static void unwrapCRef(Type) {}
};

/// A wrapper wrapping a pointer to a std::optional<Ref>-like interface.
template <class T>
    requires(std::is_reference_v<T>)
class OptionalRef {
    using Pointee = std::remove_reference_t<T>;

  public:
    OptionalRef() noexcept : ptr_(nullptr) {}
    OptionalRef(T value) noexcept : ptr_(&value) {}
    OptionalRef(std::nullopt_t) noexcept : ptr_(nullptr) {}

    bool has_value() const noexcept { return ptr_ != nullptr; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    Pointee& operator*() noexcept { return *ptr_; }
    const Pointee& operator*() const noexcept { return *ptr_; }

    Pointee* operator->() noexcept { return ptr_; }
    const Pointee* operator->() const noexcept { return ptr_; }

    Pointee& value() { return ref(); }
    const Pointee& value() const { return ref(); }

    template <class U> Pointee value_or(U&& def) const {
        return has_value() ? *ptr_ : static_cast<Pointee>(std::forward<U>(def));
    }

    void reset() noexcept { ptr_ = nullptr; }
    void swap(OptionalRef& other) noexcept { std::swap(ptr_, other.ptr_); }

  private:
    Pointee& ref() const {
        if (has_value()) {
            return *ptr_;
        } else {
            throw std::bad_optional_access();
        }
    }

  private:
    Pointee* ptr_;
};

template <std::output_iterator<uintptr_t> OutIter>
OutIter collectAsyncStackTrace(Handle h, OutIter out) {
    using detail::CoroutineFrame;
    using detail::ProxyFrame;
    using detail::TaskFrame;

    CoroutineFrame* f = CoroutineFrame::fromHandle(h);

    while (f) {
        if (TaskFrame* task = detail::frameCast<TaskFrame>(f)) {
            *out++ = task->pc;
        }

        if (ProxyFrame* proxy = detail::frameCast<ProxyFrame>(f)) {
            f = CoroutineFrame::fromHandle(proxy->followLink());
        } else {
            f = nullptr;
        }
    }

    return out;
}

} // namespace detail

template <class T>
using Optional =
        typename detail::ConditionalTmpl<std::is_reference_v<T>,
                                         detail::OptionalRef,
                                         std::optional>::template With<T>;
// Note: cannot use std::conditional_t<..., OptionalRef<T>> because it would
// instantiate both branches before checking the condition, and OptionalRef<T>
// would fail instantiation for non-references.

} // namespace corral
