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
#include <tuple>

#include "Executor.h"
#include "detail/Promise.h"
#include "detail/utility.h"
#include "detail/wait.h"

namespace corral {

namespace detail {
template <class... Ts> struct MaybeTuple {
    using Type = std::tuple<Ts...>;
    static Type&& fromTuple(std::tuple<Ts...>&& t) { return std::move(t); }
};
template <class T> struct MaybeTuple<T> {
    using Type = T;
    static T&& fromTuple(std::tuple<T>&& t) {
        return std::forward<T>(std::get<0>(t));
    }
};
template <> struct MaybeTuple<> {
    using Type = void;
    static void fromTuple(std::tuple<>&&) {}
};

template <class InitiateFn, class CancelFn, class... CBPortals>
class CBPortalProxy;
enum class CBPortalProxyStatus : uint8_t;
} // namespace detail

template <class... Ts> class CBPortal;

/// Generate a callback and wait for it to be invoked, potentially
/// multiple times and/or arbitrating between multiple callbacks
/// (such as a success and an error callback).
///
/// This is the primary facility that allows an async task to
/// interoperate with legacy code that indicates progress by calling a
/// user-provided callback (sometimes called "continuation-passing style").
///
/// Usage example:
///
///     void doSmthAndThenCall(std::function<void(int, int)> cb);
///
///     corral::Task<> example() {
///         corral::CBPortal<int, int> portal;
///         auto [x, y] = co_await corral::untilCBCalled(
///             [](std::function<void(int, int)> cb) {
///                 doSmthAndThenCall(cb);
///             }, portal);
///         // do something with x and y
///     }
///
/// `untilCBCalled()` generates a "bridge callback", `cb` in this
/// example, and passes it to its argument callable (the "initiate
/// function").  The actual object representing the bridge callback is
/// small and trivially copyable (an instance of `CBPortal::Callback`),
/// so you can accept it using a lighter-weight callback type than
/// `std::function` if you have one of those available. (It is also
/// passed by reference to a member of the CBPortal, so you can use the
/// future `std::function_ref` safely.) The template arguments of CBPortal
/// become parameters of the bridge callback. The arguments that are
/// passed to the first invocation of the bridge callback become the
/// result of the `co_await untilCBCalled(...)` expression; you get a
/// `std::tuple` if there are multiple arguments, a single object if
/// there is only one argument, or `void` if there are none. The
/// arguments passed to successive invocations of the bridge callback
/// become the results of successive `co_await portal` expressions.
///
/// If the bridge callback is going to be called multiple times, it is
/// essential for the awaiting task to keep up -- i.e., after handling one
/// call to the bridge callback, immediately await the CBPortal again.
/// There is only one call's worth of internal buffer; if the bridge
/// callback is invoked more than once in between successive instance
/// of awaiting the portal, you will get an assertion failure.
/// This generally precludes doing any blocking work in the
/// same task between successive retrievals of portal results,
/// unless you have a way to backpressure the invocations of the
/// bridge callback. (But you can push the blocking into a background
/// task that sets a `corral::Event` upon completion, and arbitrate
/// between the Event's readiness and the next callback invocation
/// using `corral::anyOf()`.)
///
/// Each call to the bridge callback will synchronously run one step of
/// the awaiting task. (That's why untilCBCalled() is structured
/// to pass the bridge callback to an initiate function rather than
/// returning it: we have to assume that the initiator could invoke
/// the callback immediately, and the coroutine must already
/// be suspended when that happens.) The initiator and the rest of
/// the task are effectively executed in parallel, interleaved by
/// invocations of the bridge callback:
///
///     corral::Task<> example() {
///         corral::CBPortal<> portal;
///         co_await untilCBCalled([](std::function<void()> cb) {
///             printf("1");
///             cb();
///             printf("3");
///             cb();
///         }, portal);
///         printf("2");
///         co_await portal;
///         printf("4");
///     }
///
/// prints 1, 2, 3, 4.
///
/// If the bridge callback accepts parameters of reference type, then
/// the awaiting task will receive the same references. They refer
/// directly to the arguments to the bridge callback, which means the
/// references must not be used after the bridge callback returns (at the
/// next `co_await` expression in the awaiting task). You should
/// not try to combine portals with `corral::allOf()` since it may delay
/// resumption of the parent task past the end of the bridge callback
/// invocation. `anyOf()` is fine as long as all of its children are CBPortals
/// or are otherwise synchronously cancellable.
///
/// Each `co_await untilCBCalled(...)` or `co_await portal` expression
/// is cancellable. You are responsible for either suppressing the
/// cancellation using `corral::noncancellable()`, or using a RAII guard
/// such as `folly::ScopeGuard` to ensure that no further calls are made
/// to the bridge callback after a cancellation occurs. We suggest a
/// pattern along the lines of:
///
///     CBPortal<> portal;
///     auto guard = folly::makeGuard([&]{
///         // Prevent any more calls from occurring, since
///         // we're about to destroy the portal.
///         cancelSmthAndDontCallCB();
///     });
///     co_await untilCBCalled([](std::function<void()> cb) {
///         doSmthAndThenCall(cb);
///     }, portal);
///     guard.dismiss();  // We were not cancelled, so dismiss the guard
///
/// If there is no way to _synchronously_ guarantee that the bridge callback
/// won't be called anymore, then you probably will need to write a custom
/// awaitable type if you want to support cancellation.
///
/// Make sure the bridge callback will not be called after the CBPortal
/// goes out of scope.
///
/// If the initiate function accepts multiple bridge-callback arguments,
/// it can be used in conjunction with multiple CBPortals to arbitrate
/// between calls to multiple callbacks. For example:
///
///     void getMeAnInt(std::function<void(int)> onInt,
///                     std::function<void(std::string)> onError);
///
///     corral::Task<> example() {
///         CBPortal<int> intp;
///         CBPortal<std::string> errp;
///
///         auto [res, err] = co_await corral::untilCBCalled(
///             [](std::function<void(int)> intCB,
///                std::function<void(std::string)> errorCB) {
///                 getMeAnInt(intCB, errorCB);
///             },
///             intp, errp
///         );
///         if (res) {
///             // do something with *res
///         }
///
///         // Subsequent awaits, if the callback(s) are going to be called
///         // multiple times:
///         std::tie(res, err) = co_await corral::anyOf(intp, errp);
///     }
template <class InitiateFn, class... PortalArgs, class... MorePortals>
auto untilCBCalled(InitiateFn&& initiateFn,
                   CBPortal<PortalArgs...>& firstPortal,
                   MorePortals&... morePortals);

/// Bridge object used by `untilCBCalled()`. Does not do anything useful
/// until activated by a call to `untilCBCalled()`.
template <class... Ts> class CBPortal {
    using ReturnType = typename detail::MaybeTuple<Ts...>::Type;
    static constexpr const bool IsVoid = std::is_same_v<ReturnType, void>;

  public:
    CBPortal() = default;
    CBPortal(CBPortal&&) = delete;
    CBPortal& operator=(CBPortal&&) = delete;

    // Awaitable interface. The result type is as follows:
    // - `CBPortal<>`: `void`
    // - `CBPortal<T>`: `T`
    // - `CBPortal<Ts...>`: `std::tuple<Ts...>`
    void await_set_executor(Executor* ex) noexcept {
        executor_.set(ex, executor_.bits());
    }
    bool await_ready() const noexcept { return value_.has_value(); }
    void await_suspend(Handle h) &;
    ReturnType await_resume();
    bool await_cancel(Handle) noexcept {
        // NB: wakeUp()'s use of Executor::capture() will ensure that
        // any RAII guard our parent task is using to prevent further
        // callbacks (as suggested in the docs above) runs before any
        // callback can intervene. If we were to just return true
        // instead, then our parent task's resumption could be delayed by
        // the executor, so we might lose a callback that happened right
        // after cancellation.
        wakeUp();
        return false;
    }
    bool await_must_resume() const noexcept { return await_ready(); }

    /// The type of a bridge callback used to interface with this portal.
    /// There is no way to obtain one of these from a CBPortal directly;
    /// you can only get it using the initiate function passed to
    /// `co_await corral::untilCBCalled(...)`.
    class Callback {
        CBPortal* portal_ = nullptr;
        Callback() = default;
        friend CBPortal;

      public:
        void operator()(Ts... values) const;
    };

  protected:
    // CBPortal is not movable, because we might have handed out a Callback
    // that holds a reference to it. But in order to implement the version of
    // untilCBCalled() that doesn't take explicit portal arguments, we do
    // need to move some portals around. We do this using a subclass,
    // MovableCBPortal, which uses this function to assert that it's OK
    // to move in this case because no callback has been handed out yet and
    // no one is awaiting us.
    bool canMove() const { return !callback_.portal_ && !parent_; }

  private /*methods*/:
    using Stored = std::tuple<Ts...>;

    Callback& callback() {
        callback_.portal_ = this;
        return callback_;
    }
    void wakeUp();
    bool hasResumedParent() const { return parent_ == nullptr; }

  private /*fields*/:
    std::optional<Stored> value_;
    // To keep CBPortalProxy compact, we stash a couple bits of state
    // in the bottom of the 'executor_' field for its first CBPortal.
    detail::PointerBits<Executor, detail::CBPortalProxyStatus, 2> executor_;
    Handle parent_;
    Callback callback_;

    template <class InitiateFn, class CancelFn, class... CBPortals>
    friend class detail::CBPortalProxy;
};


/// Generate one or more callbacks and wait for one of them to be invoked,
/// returning the arguments that were passed to it.
///
/// This is a variant of `untilCBCalled()` that supports a simpler interface
/// (no need to define any CBPortals) in exchange for only permitting a
/// single call to any of the bridge callbacks. The bridge callback signatures
/// are inferred from the parameters of the initiate function, one callback
/// per parameter. Each of the initiate function's parameter types must
/// be a callable object whose `operator()` has a signaute of the form
/// `void(Args...)`. The `Args...` determine the signature of the
/// corresponding bridge callback, which will be used to initialize the
/// initiate function parameter.
///
/// If the caller is cancelled after the initiate function has run
/// but before any of the callbacks are invoked, then the cancellation
/// function (2nd argument) will be invoked before the internal
/// CBPortals are destroyed; it must ensure that no further calls will
/// be made to any of the bridge callbacks. Note that a cancellation is
/// also possible before the initiate function runs, in which case the
/// cancellation function will not be called.
///
/// If there is just one bridge callback, generated to fill in one parameter
/// for the initiate function, then `co_await untilCBCalled()` evaluates
/// to the arguments that were passed to the bridge callback: a `std::tuple`
/// of multiple arguments, an unpacked single argument, or void if there are
/// no arguments. If there are multiple bridge callbacks, then the result is a
/// `std::tuple<std::optional<Rs>...>` where Rs are the return types that
/// would be used to represent the arguments of each callback option. At most
/// one of the returned `std::optional`s will be engaged.
///
/// Example usage:
///
///     auto [x, y] = co_await cbCalled([](std::function<void(int, int)> cb) {
///         doSmthAndThenCall(cb);
///     });
template <class InitiateFn, std::invocable<> CancelFn>
auto untilCBCalled(InitiateFn&& initiateFn, CancelFn&& cancelFn);

/// Variant of `untilCBCalled()` that does not provide a cancellation handler.
/// Since it is unsafe to destroy the CBPortal(s) until we know no
/// further call will be made, but they must be destroyed before
/// `untilCBCalled()` completes, this variant is uncancellable after
/// the initiate function has run. (It is still capable of propagating
/// a pending cancellation before it does any work.)
template <class InitiateFn> auto untilCBCalled(InitiateFn&& initiateFn);


//
// Implementation
//

template <class... Ts> void CBPortal<Ts...>::await_suspend(Handle h) & {
    CORRAL_TRACE("    ...CBPortal %p", this);
    parent_ = h;
}

template <class... Ts>
typename CBPortal<Ts...>::ReturnType CBPortal<Ts...>::await_resume() {
    CORRAL_ASSERT(await_ready());
    if constexpr (!IsVoid) {
        return detail::MaybeTuple<Ts...>::fromTuple(
                *std::exchange(value_, std::nullopt));
    } else {
        value_.reset();
    }
}

template <class... Ts>
void CBPortal<Ts...>::Callback::operator()(Ts... values) const {
    CORRAL_ASSERT(!portal_->value_.has_value() && "consumer not keeping up");
    portal_->value_.emplace(std::forward<Ts>(values)...);
    portal_->wakeUp(); // may destroy `portal_`
}

template <class... Ts> void CBPortal<Ts...>::wakeUp() {
    CORRAL_TRACE("awaking CBPortal %p", this);
    Handle h = std::exchange(parent_, Handle{});
    if (h) {
        // Since the callback might get called immediately again,
        // it is important to execute (not merely schedule) the parent task
        // before wakeUp() returns.
        executor_.ptr()->capture([h] { h.resume(); });
    }
}

namespace detail {

template <class... Ts> struct InlineCBPortalTag {};

template <class T> struct CBPortalTraits;
template <class... Ts> struct CBPortalTraits<CBPortal<Ts...>&> {
    static constexpr const bool IsExternal = true;
    using PortalType = CBPortal<Ts...>&;
    CBPortal<Ts...>& toPortal(CBPortal<Ts...>& p) { return p; }
};
template <class... Ts> struct CBPortalTraits<InlineCBPortalTag<Ts...>> {
    static constexpr const bool IsExternal = false;
    using PortalType = CBPortal<Ts...>;
    CBPortal<Ts...> toPortal(InlineCBPortalTag<Ts...>) { return {}; }
};

enum class CBPortalProxyStatus : uint8_t {
    // Initiator has not been called yet and can be safely skipped
    NotStarted = 0,

    // Initiator must execute but hasn't finished yet
    Scheduled = 1,

    // Scheduled, plus cancellation has been requested
    CancelPending = 2,

    // Initiator has finished execution
    Initiated = 3,
};

struct DoNothing {
    void operator()() const {}
};

template <class InitiateFn, class CancelFn, class... CBPortals>
class CBPortalProxy : private detail::Noncopyable {
    static_assert(sizeof...(CBPortals) > 0,
                  "at least one bridge callback is required");
    static constexpr bool Singular = sizeof...(CBPortals) == 1;

    static constexpr bool PortalsAreExternal =
            (CBPortalTraits<CBPortals>::IsExternal && ...);
    static constexpr bool PortalsAreInline =
            (!CBPortalTraits<CBPortals>::IsExternal && ...);
    static_assert(PortalsAreExternal || PortalsAreInline,
                  "all CBPortals must be either references or inline");

    using MergedPortals = std::conditional_t<
            Singular,
            std::tuple_element_t<0,
                                 std::tuple<typename CBPortalTraits<
                                         CBPortals>::PortalType...>>&,
            detail::AwaitableMaker<
                    detail::AnyOf<
                            typename CBPortalTraits<CBPortals>::PortalType&...>,
                    typename CBPortalTraits<CBPortals>::PortalType&...>>;

    using ProxyStatus = CBPortalProxyStatus;

  public:
    explicit CBPortalProxy(InitiateFn&& initiateFn,
                           CancelFn&& cancelFn,
                           CBPortals&... portals)
        requires(PortalsAreExternal)
      : initiateFn_(std::forward<InitiateFn>(initiateFn)),
        cancelFn_(std::forward<CancelFn>(cancelFn)),
        portals_(portals...),
        awaitable_(makeAwaitable()) {
        // With external portals, the user may have put a scope guard
        // in between the portal and the untilCBCalled() in order to
        // implement cancellation. We thus shouldn't propagate a
        // cancellation until the initiator has been called.
        setProxyStatus(ProxyStatus::Scheduled);
    }

    explicit CBPortalProxy(InitiateFn&& initiateFn, CancelFn&& cancelFn)
        requires(!PortalsAreExternal)
      : initiateFn_(std::forward<InitiateFn>(initiateFn)),
        cancelFn_(std::forward<CancelFn>(cancelFn)),
        awaitable_(makeAwaitable()) {
        // With internal portals, we own the portals and can propagate
        // cancellation immediately.
        setProxyStatus(ProxyStatus::NotStarted);
    }


    bool await_ready() const noexcept { return false; }

    auto await_early_cancel() noexcept {
        if constexpr (!PortalsAreExternal) {
            CORRAL_ASSERT(getProxyStatus() == ProxyStatus::NotStarted);
            return std::true_type{};
        } else {
            CORRAL_ASSERT(getProxyStatus() == ProxyStatus::Scheduled);
            setProxyStatus(ProxyStatus::CancelPending);
            return false;
        }
    }

    void await_set_executor(Executor* ex) noexcept {
        CORRAL_ASSERT(!awaitable_.await_ready());
        awaitable_.await_set_executor(ex);
    }

    void await_suspend(Handle h) {
        awaitable_.await_suspend(h);

        if (getProxyStatus() == ProxyStatus::NotStarted) {
            setProxyStatus(ProxyStatus::Scheduled);
        }
        auto getCBs = [](auto&... cbs) {
            return std::forward_as_tuple(cbs.callback()...);
        };
        auto cbs = std::apply(getCBs, portals_);

        bool completed = false;
        completed_ = &completed;
        std::apply(initiateFn_, cbs);

        // The initiateFn might have synchronously called one of the callbacks
        // that we passed to it; if so, this CBPortalProxy object has already
        // been destroyed and we shouldn't do the cancellation check below.
        if (completed) {
            return;
        }
        completed_ = nullptr;

        bool wasCancelled = (getProxyStatus() == ProxyStatus::CancelPending);
        setProxyStatus(ProxyStatus::Initiated);

        if (wasCancelled && !std::get<0>(portals_).hasResumedParent()) {
            if (await_cancel(h)) {
                h.resume();
            }
        }
    }

    bool await_cancel(Handle h) noexcept {
        switch (getProxyStatus()) {
            case ProxyStatus::NotStarted:
                break;
            case ProxyStatus::Scheduled:
            case ProxyStatus::CancelPending:
                setProxyStatus(ProxyStatus::CancelPending);
                return false;
            case ProxyStatus::Initiated:
                if (!PortalsAreExternal && std::same_as<CancelFn, DoNothing>) {
                    // This is the single-argument untilCBCalled();
                    // since no cancellation function was provided, we
                    // can't safely permit cancellation after the
                    // initiate function has run.
                    return false;
                }
                cancelFn_();
                return awaitable_.await_cancel(h);
        }
        CORRAL_ASSERT_UNREACHABLE();
        return true;
    }

    bool await_must_resume() const noexcept { return awaitable_.await_ready(); }

    decltype(auto) await_resume() && {
        if (completed_) {
            *completed_ = true;
        }
        return std::move(awaitable_).await_resume();
    }

  private /*methods*/:
    decltype(auto) makeAwaitable() {
        if constexpr (Singular) {
            return std::get<0>(portals_);
        } else {
            return getAwaitable(std::make_from_tuple<MergedPortals>(portals_));
        }
    }

    ProxyStatus getProxyStatus() const {
        return std::get<0>(portals_).executor_.bits();
    }

    void setProxyStatus(ProxyStatus st) {
        auto& cb = std::get<0>(portals_);
        cb.executor_.set(cb.executor_.ptr(), st);
    }

  private /*fields*/:
    InitiateFn initiateFn_;
    CancelFn cancelFn_;
    std::tuple<typename CBPortalTraits<CBPortals>::PortalType...> portals_;
    AwaitableType<MergedPortals> awaitable_;

    // If this is non-null, its pointee is set to true in await_resume().
    // This allows await_suspend() to notice that the CBPortalProxy object
    // may have already been destroyed.
    bool* completed_ = nullptr;
};

template <class Fn> struct CBPortalSignature : CallableSignature<Fn> {
    static_assert(!CallableSignature<Fn>::IsMemFunPtr,
                  "untilCBCalled() argument must not be a "
                  "member function pointer");
};


// MakePortalFor<CB>::Type is InlineCBPortalTag<Args...> where Args... are
// the parameter types of the callable object CB
template <class CB> struct MakePortalFor {
    using Sig = CallableSignature<CB>;
    using Type = typename Sig::template BindArgs<InlineCBPortalTag>;
    static_assert(std::is_void_v<typename Sig::Ret>,
                  "Bridge callbacks return void, so the parameters of the "
                  "initiate function to which they are bound must be "
                  "callback types that return void");
};

// MakePortalsFor<CBs...>::BindPortals<T> is T<InlineCBPortalTag<Args...>...>
// (T is any template)
template <class... CBs> struct MakePortalsFor {
    template <template <class...> class T>
    using BindPortals = T<typename MakePortalFor<CBs>::Type...>;
};

template <class InitiateFn, class CancelFn> struct MakePortalProxyTypeFor {
    using InitSig = CallableSignature<InitiateFn>;
    static_assert(std::is_void_v<typename InitSig::Ret>,
                  "Return value of initiate function will be ignored");
    static_assert(std::is_void_v<std::invoke_result_t<CancelFn>>,
                  "Return value of cancel function will be ignored");
    using MakePortals = typename InitSig::template BindArgs<MakePortalsFor>;
    template <class... Portals>
    using ProxyFor = CBPortalProxy<InitiateFn, CancelFn, Portals...>;
    using Type = typename MakePortals::template BindPortals<ProxyFor>;
};

template <class InitiateFn, class CancelFn>
using PortalProxyTypeFor =
        typename MakePortalProxyTypeFor<InitiateFn, CancelFn>::Type;

} // namespace detail

template <class InitiateFn, class... PortalArgs, class... MorePortals>
auto untilCBCalled(InitiateFn&& initiateFn,
                   CBPortal<PortalArgs...>& portal,
                   MorePortals&... morePortals) {
    using Portal = CBPortal<PortalArgs...>;
    using Ret = detail::CBPortalProxy<InitiateFn, detail::DoNothing, Portal&,
                                      MorePortals&...>;
    return makeAwaitable<Ret>(std::forward<InitiateFn>(initiateFn),
                              detail::DoNothing{}, portal, morePortals...);
}

template <class InitiateFn, std::invocable<> CancelFn>
auto untilCBCalled(InitiateFn&& initiateFn, CancelFn&& cancelFn) {
    using Ret = detail::PortalProxyTypeFor<InitiateFn, CancelFn>;
    return makeAwaitable<Ret>(std::forward<InitiateFn>(initiateFn),
                              std::forward<CancelFn>(cancelFn));
}

template <class InitiateFn> auto untilCBCalled(InitiateFn&& initiateFn) {
    return untilCBCalled(std::forward<InitiateFn>(initiateFn),
                         detail::DoNothing{});
}

} // namespace corral
