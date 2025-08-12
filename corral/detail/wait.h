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
#include <algorithm>
#include <tuple>

#include "../Task.h"
#include "../concepts.h"
#include "../config.h"
#include "Optional.h"
#include "PointerBits.h"
#include "Promise.h"
#include "frames.h"
#include "utility.h"

namespace corral::detail {

//
// AnyOf
//

/// A utility helper accompanying an Awaitable with its own coroutine_handle<>,
/// so AnyOf/AllOf can figure out which awaitable has completed, and takes care
/// of not cancelling things twice.
template <class Policy, class MuxT, class Awaitable>
class MuxHelper : public ProxyFrame {
    using Ret = AwaitableReturnType<Awaitable>;

    enum class State {
        NotStarted,          // before await_suspend()
        CancellationPending, // NotStarted + await_early_cancel() returned false
        Running,             // after await_suspend()
        Cancelling,          // Running + await_cancel() returned false
        Cancelled,           // cancellation confirmed
        Ready,               // completed but value/exception not yet extracted
        Succeeded,           // awaitable completed and yielded a value
        Failed,              // awaitable completed and yielded an exception
    };
    static constexpr const size_t StateWidth = 3;

    static_assert(static_cast<size_t>(State::Failed) < (1 << StateWidth));

  public:
    // A type for semantic value returned by the awaitable, unboxed from any
    // error-carrying wrappers (for example, std::expected<T, E> might yield T).
    using ValueType = typename std::conditional_t<
            std::is_same_v<Ret, Void>,
            void,
            decltype(Policy::template unwrapValue<Ret>(std::declval<Ret>()))>;

    explicit MuxHelper(Awaitable&& awaitable)
      : mux_(nullptr, State::NotStarted),
        awaiter_(std::forward<Awaitable>(awaitable)) {}

    ~MuxHelper() {
        if (state() == State::Succeeded) {
            reinterpret_cast<StorageType*>(storage_)->~StorageType();
        }
    }

    MuxHelper(const MuxHelper& rhs) = delete;
    MuxHelper& operator=(const MuxHelper& rhs) = delete;

    static bool skippable() { return Skippable<AwaiterType<Awaitable>>; }

    void setExecutor(Executor* ex) noexcept {
        if (state() == State::NotStarted ||
            state() == State::CancellationPending) {
            awaiter_.await_set_executor(ex);
        }
    }

    bool ready() const noexcept {
        switch (state()) {
            case State::NotStarted:
                return awaiter_.await_ready();
            case State::Running:
            case State::Cancelling:
                CORRAL_ASSERT_UNREACHABLE();
            case State::CancellationPending:
                // If the parent task has pending cancellation, we want
                // to execute the more involved logic in kickOff().
                [[fallthrough]];
            case State::Cancelled:
                return false;
            case State::Ready:
            case State::Succeeded:
            case State::Failed:
                return true;
        }
        CORRAL_ASSERT_UNREACHABLE();
    }

    void bind(MuxT& mux) {
        mux_.set(&mux, mux_.bits());
        linkTo(mux.parent_);
    }

    bool isBound() const { return mux() != nullptr; }

    void suspend() {
        switch (state()) {
            case State::NotStarted:
            case State::CancellationPending:
                kickOff();
                break;
            case State::Cancelled:
                mux()->invoke({});
                break;
            case State::Running:
            case State::Cancelling:
            case State::Ready:
            case State::Succeeded:
            case State::Failed:
                CORRAL_ASSERT_UNREACHABLE();
        }
    }

    // Returns true if this awaitable is now cancelled and will not
    // be completing with a result or exception.
    bool cancel() {
        switch (state()) {
            case State::NotStarted:
                if (awaiter_.await_early_cancel()) {
                    setState(State::Cancelled);
                    if (MuxT* m = mux()) {
                        m->invoke({});
                    } else {
                        // If we don't have a mux yet, this is an early
                        // cancel (before suspend()) and we'll delay
                        // the invoke() call until either suspend() or
                        // reportImmediateResult() (at most one of these
                        // two will be called).
                    }
                    return true;
                }
                setState(State::CancellationPending);
                break;
            case State::Running:
                setState(State::Cancelling);
                if (awaiter_.await_cancel(this->toHandle())) {
                    setState(State::Cancelled);
                    mux()->invoke({});
                    return true;
                }
                if (state() == State::Cancelled) {
                    // Perhaps await_cancel() did wind up synchronously
                    // resuming the handle, even though it returned false.
                    return true;
                }
                break;
            case State::Cancelled:
                return true;
            case State::CancellationPending:
            case State::Cancelling:
            case State::Ready:
            case State::Succeeded:
            case State::Failed:
                break;
        }
        return false;
    }

    // Handle the mux await_resume() having been called without await_suspend().
    // The precondition for this is the mux await_ready(): enough children
    // are ready and the rest are skippable.
    void reportImmediateResult() {
        if (state() == State::CancellationPending) {
            // Early-cancel failed then awaitable was ready -> must check
            // await_must_resume. This would have happened in mustResume()
            // if the cancellation came from outside, but happens here
            // otherwise (e.g. AnyOf cancelling remaining children after
            // the first one completes).
            setState(awaiter_.await_must_resume() ? State::Ready
                                                  : State::Cancelled);
        }
        if (state() == State::Cancelled) {
            // Already cancelled, just need to notify the mux (we weren't
            // bound yet when the Cancelled state was entered).
            mux()->invoke({});
        } else if (state() == State::NotStarted && !awaiter_.await_ready()) {
            // Awaitable was not needed. await_ready() would not have
            // returned true unless it was Skippable, which we assert here;
            // cancel() will call mux()->invoke({}).
            [[maybe_unused]] bool cancelled = cancel();
            CORRAL_ASSERT(cancelled);
        } else {
            // Awaitable is ready; get its result. reportResult() will call
            // mux()->invoke().
            CORRAL_ASSERT(state() == State::NotStarted ||
                          state() == State::Ready);
            reportResult();
        }
    }

    InhabitedType<ValueType> result() && {
        CORRAL_ASSERT(state() == State::Succeeded);
        return Storage<InhabitedType<ValueType>>::unwrap(
                std::move(*reinterpret_cast<StorageType*>(storage_)));
    }

    Optional<InhabitedType<ValueType>> asOptional() && {
        switch (state()) {
            case State::Succeeded:
                return std::move(*this).result();
            case State::Cancelled:
                return std::nullopt;
            case State::NotStarted:
            case State::CancellationPending:
            case State::Running:
            case State::Cancelling:
            case State::Ready:
            case State::Failed:
                CORRAL_ASSERT_UNREACHABLE();
        }
        CORRAL_ASSERT_UNREACHABLE();
    }

    bool mustResume() const noexcept {
        // This is called from the mux's await_must_resume(), which runs
        // in two situations:
        //
        // - After parent resumption when a past await_cancel() or
        //   await_early_cancel() didn't complete synchronously: this
        //   occurs after every awaitable in the mux has resumed its
        //   parent, and the outcome of each awaitable (cancelled vs
        //   completed) was already decided in invoke().
        //
        // - After await_early_cancel() returned false but
        //   await_ready() returned true, with no suspension involved:
        //   no one has called await_must_resume() yet, so we
        //   shall. To avoid calling await_must_resume() multiple
        //   times, we will check it here and change
        //   CancellationPending to either Cancelled or Ready,
        //   and then reportImmediateResult() will know which path to take.
        switch (state()) {
            // No-suspension-yet cases, reportImmediateResult() about to
            // be invoked:
            case State::NotStarted:
                return true;
            case State::CancellationPending: {
                bool shouldResume = awaiter_.await_must_resume();
                const_cast<MuxHelper*>(this)->setState(
                        shouldResume ? State::Ready : State::Cancelled);
                return shouldResume;
            }

            // Could be either path:
            case State::Cancelled:
                return false;

            // Already-suspended-and-resumed cases:
            case State::Ready:
            case State::Succeeded:
            case State::Failed:
                return true;

            case State::Running:
            case State::Cancelling:
                CORRAL_ASSERT_UNREACHABLE();
        }
        CORRAL_ASSERT_UNREACHABLE();
    }

    void introspect(auto& c) const noexcept {
        switch (state()) {
            case State::Cancelling:
                c.footnote("(cancelling:)");
                [[fallthrough]];
            case State::Running:
                c.child(awaiter_);
                break;

            case State::NotStarted:
            case State::CancellationPending:
            case State::Cancelled:
            case State::Ready:
            case State::Succeeded:
            case State::Failed:
                break;
        }
    }

  private:
    MuxT* mux() const noexcept { return mux_.ptr(); }
    State state() const noexcept { return mux_.bits(); }
    void setState(State st) noexcept { mux_.set(mux(), st); }

    void kickOff() {
        // Called from suspend(); state is NotStarted or CancellationPending
        bool cancelRequested = (state() == State::CancellationPending);
        setState(cancelRequested ? State::Cancelling : State::Running);
        if (awaiter_.await_ready()) {
            invoke();
        } else {
            resumeFn = +[](CoroutineFrame* frame) {
                static_cast<MuxHelper*>(frame)->invoke();
            };
#if __cpp_exceptions
            try {
#endif
                awaiter_.await_suspend(this->toHandle()).resume();
#if __cpp_exceptions
            } catch (...) {
                setState(State::Failed);
                mux()->invoke(Policy::fromCurrentException());
            }
#endif
        }
    }

    void reportResult() {
        typename Policy::ErrorType err;
#if __cpp_exceptions
        try {
#endif
            setState(State::Succeeded);
            Ret ret = awaiter_.await_resume();
            if constexpr (!std::is_same_v<Ret, Void>) {
                err = Policy::unwrapError(ret);
                if (!Policy::hasError(err)) {
                    new (storage_) StorageType(Storage<ValueType>::wrap(
                            Policy::template unwrapValue<Ret>(
                                    std::forward<Ret>(ret))));
                } else {
                    setState(State::Failed);
                }
            } else { // infallible awaitable
                new (storage_) StorageType(Void{});
            }
#if __cpp_exceptions
        } catch (...) {
            setState(State::Failed);
            err = Policy::fromCurrentException();
        }
#endif
        mux()->invoke(err);
    }

    void invoke() {
        switch (state()) {
            case State::Cancelling:
                if (!awaiter_.await_must_resume()) {
                    setState(State::Cancelled);
                    mux()->invoke({});
                    return;
                }
                [[fallthrough]];
            case State::Running:
            case State::Ready:
                reportResult();
                break;
            case State::CancellationPending:
            case State::NotStarted:
            case State::Cancelled:
            case State::Succeeded:
            case State::Failed:
                CORRAL_ASSERT_UNREACHABLE();
        }
    }

  private:
    // Note: we cannot use alignof(MuxT) here because MuxT is incomplete
    // yet, so use coroutine_handle for alignment.
    PointerBits<MuxT, State, StateWidth, alignof(Handle)> mux_;

    SanitizedAwaiter<Awaitable> awaiter_;

    using StorageType = typename Storage<InhabitedType<ValueType>>::Type;
    alignas(StorageType) char storage_[sizeof(StorageType)];
};


template <class Policy, class Self> class MuxBase {
  public:
    MuxBase() = default;

    // If all the Awaitables are Skippable, then the whole mux is too.
    auto await_early_cancel() noexcept {
        bool allNowCancelled = self().internalCancel();
        if constexpr (Self::muxIsSkippable()) {
            CORRAL_ASSERT(allNowCancelled);
            return std::true_type{};
        } else {
            return allNowCancelled;
        }
    }

    auto await_cancel(Handle h) noexcept {
        bool allNowCancelled = [&] {
            // Avoid resuming our parent while we cancel things; we might
            // want to return true instead.
            Handle origParent = std::exchange(parent_, std::noop_coroutine());
            auto guard =
                    ScopeGuard([this, origParent] { parent_ = origParent; });
            return self().internalCancel();
        }();

        // For muxes that are ready when one awaitable is ready (AnyOf/OneOf):
        // If all the Awaitables are Abortable, then the completion of the
        // first one will immediately cancel the rest, so if the mux hasn't
        // completed yet then none of the individual children have completed,
        // so await_cancel() of the mux will always succeed synchronously too.
        // This is not true for AllOf/MostOf, because some of the children
        // might have already completed before the cancellation occurs.
        if constexpr (Self::muxIsAbortable()) {
            CORRAL_ASSERT(allNowCancelled);
            return std::true_type{};
        } else {
            if (allNowCancelled) {
                return true;
            }
            if (count_ == self().size()) {
                // We synchronously cancelled the remaining children, but
                // some had already completed so this doesn't count as
                // a sync-cancel of the overall mux.
                h.resume();
            }
            return false;
        }
    }

    static constexpr bool doneOnFirstReady = false;

  protected:
    Self& self() { return *static_cast<Self*>(this); }
    const Self& self() const { return *static_cast<const Self*>(this); }

    bool doSuspend(Handle h) {
        if (self().size() == 0) {
            return false;
        }
        CORRAL_TRACE("   ...on Mux<%lu/%lu> %p", self().minReady(),
                     self().size(), this);
        parent_ = h;
        return true;
    }

    bool hasError() const noexcept { return Policy::hasError(error_); }
    decltype(auto) wrapError() { return Policy::wrapError(std::move(error_)); }

  private:
    void invoke(typename Policy::ErrorType err) {
        long i = ++count_;
        bool fail = Policy::hasError(err);
        bool firstFail = (fail && !Policy::hasError(error_));
        if (firstFail) {
            error_ = std::move(err);
        }
        CORRAL_TRACE("Mux<%lu/%lu> %p invocation %lu%s", self().minReady(),
                     self().size(), this, i, (fail ? " with exception" : ""));
        if (i == self().size()) {
            parent_.resume();
        } else if (firstFail || i == self().minReady()) {
            --count_;
            self().internalCancel();
            if (++count_ == self().size()) {
                parent_.resume();
            }
        }
    }

  private:
    long count_ = 0;
    Handle parent_;
    typename Policy::ErrorType error_{};

    template <class, class, class> friend class MuxHelper;
};


//
// Tuple-based combiners
//

/// Implementation of anyOf(), mostOf(), and a building block for allOf().
template <class Policy, class Self, class... Awaitables>
class MuxTuple : public MuxBase<Policy, Self> {
  public:
    explicit MuxTuple(Awaitables&&... awaitables)
      : children_(std::forward<Awaitables>(awaitables)...) {}

    // See note in MuxBase::await_cancel() regarding why we only can propagate
    // Abortable if the mux completes when its first awaitable does
    static constexpr bool muxIsAbortable() {
        return Self::minReady() == 1 &&
               (Abortable<AwaiterType<Awaitables>> && ...);
    }
    static constexpr bool muxIsSkippable() {
        return (Skippable<AwaiterType<Awaitables>> && ...);
    }
    static constexpr long size() noexcept { return sizeof...(Awaitables); }

    void await_set_executor(Executor* ex) noexcept {
        auto impl = [ex](auto&... children) {
            (children.setExecutor(ex), ...);
        };
        std::apply(impl, children_);
    }

    bool await_ready() const noexcept {
        auto impl = [](const auto&... aws) {
            long nReady = (static_cast<long>(aws.ready()) + ...);
            long nSkipKickoff =
                    (static_cast<long>(aws.ready() || aws.skippable()) + ...);
            return nReady >= Self::minReady() &&
                   nSkipKickoff == sizeof...(Awaitables);
        };
        return std::apply(impl, children_);
    }

    bool await_suspend(Handle h) {
        bool ret = this->doSuspend(h);
        auto impl = [this](auto&... children) {
            (children.bind(*static_cast<Self*>(this)), ...);
            (children.suspend(), ...);
        };
        std::apply(impl, children_);
        return ret;
    }

    auto await_resume() && {
        handleResumeWithoutSuspend();
        auto impl = [](auto&&... children) {
            return std::make_tuple(std::move(children).asOptional()...);
        };
        using Ret = decltype(Policy::wrapValue(
                std::apply(impl, std::move(children_))));

        if (!this->hasError()) {
            return Policy::wrapValue(std::apply(impl, std::move(children_)));
        } else if constexpr (PolicyUsesErrorCodes<Policy>) {
            return Ret{this->wrapError()};
        } else {
            this->wrapError();
            CORRAL_ASSERT_UNREACHABLE();
        }
    }

    // This is called in two situations:
    // - enough children have completed and we want to cancel the rest
    // - we get a cancellation from the outside (await_cancel)
    // Returns true if every awaitable is now cancelled (i.e., we
    // definitely have no results to report to our parent), which is
    // only relevant to await_cancel().
    bool internalCancel() {
        auto impl = [](auto&... children) {
            // Don't short-circuit; we should try to cancel every remaining
            // awaitable, even if some have completed already.
            return (... & int(children.cancel()));
        };
        return std::apply(impl, children_);
    }

    auto await_must_resume() const noexcept {
        auto impl = [](const auto&... children) {
            // Don't short-circuit; we should check mustResume of every
            // awaitable, since it can have side effects.
            return (... | int(children.mustResume()));
        };
        bool anyMustResume = std::apply(impl, children_);

        // It is not generally true that CancelAlwaysSucceeds for a
        // mux even if it is true for each of the constituents. If one
        // awaitable completes, it might take some time for the others
        // to finish cancelling, and a cancellation of the overall mux
        // during this time would fail (because the mux already has a
        // value to report to its parent).
        //
        // If all cancels for this mux complete synchronously, though,
        // we know we'll never have a must-resume situation, because
        // the length of the time interval described in the previous
        // paragraph is zero.
        if constexpr (Skippable<Self> && Abortable<Self>) {
            CORRAL_ASSERT(!anyMustResume);
            return std::false_type{};
        } else {
            return anyMustResume;
        }
    }

  protected:
    auto& children() { return children_; }
    const auto& children() const { return children_; }

    void handleResumeWithoutSuspend() {
        if (!std::get<0>(children_).isBound()) {
            // We skipped await_suspend because all children were
            // ready or sync-early-cancellable. (All of the MuxHelpers
            // have bind() called at the same time; we check the first
            // one for convenience only.)
            this->doSuspend(std::noop_coroutine());

            std::apply(
                    [this](auto&... children) {
                        (children.bind(*static_cast<Self*>(this)), ...);
                    },
                    children_);

            // Whichever child resumes first may get to cancel its siblings
            // if we're in anyOf(), so we need to make sure we first
            // collect the result of whichever child is ready.

            size_t readyIndex = sizeof...(Awaitables);
            auto checkIfReady = [&](size_t i, auto& child) {
                if (child.ready()) {
                    child.reportImmediateResult();
                    readyIndex = i;
                    return true;
                } else {
                    return false;
                }
            };
            [&]<size_t... I>(std::index_sequence<I...>) {
                (checkIfReady(I, std::get<I>(children_)) || ...);
            }(std::make_index_sequence<sizeof...(Awaitables)>{});
            CORRAL_ASSERT(readyIndex != sizeof...(Awaitables));

            // Now collect the results of the remaining children.
            [&]<size_t... I>(std::index_sequence<I...>) {
                ((I != readyIndex
                          ? std::get<I>(children_).reportImmediateResult()
                          : void()),
                 ...);
            }(std::make_index_sequence<sizeof...(Awaitables)>{});
        }
    }

    void introspect(const char* name, auto& c) const noexcept {
        c.node(name, this);
        auto impl = [&c](const auto&... children) {
            (children.introspect(c), ...);
        };
        std::apply(impl, children_);
    }

  private:
    std::tuple<MuxHelper<Policy, Self, Awaitables>...> children_;
};

// Specialization for zero awaitables:
template <class Policy, class Self>
class MuxTuple<Policy, Self> : public MuxBase<Policy, Self> {
  public:
    static constexpr bool muxIsAbortable() { return true; }
    static constexpr bool muxIsSkippable() { return true; }
    static constexpr long size() noexcept { return 0; }
    bool await_ready() const noexcept { return true; }
    bool await_suspend(Handle) { return false; }
    PolicyReturnTypeFor<Policy, std::tuple<>> await_resume() && {
        return Policy::wrapValue(std::tuple<>{});
    }
    bool internalCancel() noexcept { return true; }

  protected:
    std::tuple<> children() const { return {}; }
    void handleResumeWithoutSuspend() {}
    void introspect(const char* name, auto& c) const { c.node(name); }
};

template <class Policy, class... Awaitables>
class AnyOf
  : public MuxTuple<Policy, AnyOf<Policy, Awaitables...>, Awaitables...> {
  public:
    using AnyOf::MuxTuple::MuxTuple;
    static constexpr long minReady() noexcept { return 1; }
    void await_introspect(auto& c) const noexcept {
        this->introspect("AnyOf", c);
    }
};

template <class Policy, class... Awaitables>
class MostOf
  : public MuxTuple<Policy, MostOf<Policy, Awaitables...>, Awaitables...> {
  public:
    using MostOf::MuxTuple::MuxTuple;
    static constexpr long minReady() noexcept { return sizeof...(Awaitables); }
    void await_introspect(auto& c) const noexcept {
        this->introspect("MostOf", c);
    }
};

template <class Policy, class... Awaitables>
class AllOf
  : public MuxTuple<Policy, AllOf<Policy, Awaitables...>, Awaitables...> {
  public:
    using AllOf::MuxTuple::MuxTuple;

    static constexpr long minReady() noexcept { return sizeof...(Awaitables); }

    auto await_must_resume() const noexcept {
        // We only require the parent to be resumed if *all* children
        // have a value to report, so we'd be able to construct a tuple of
        // results (or if any awaitable failed, so we'd return an error and not
        // bother with return value construction).
        auto impl = [](const auto&... children) {
            if constexpr (sizeof...(children) == 0) {
                return false;
            } else {
                return (... & int(children.mustResume()));
            }
        };
        bool ret = this->hasError() || std::apply(impl, this->children());

        // See note in MuxTuple::await_must_resume(). Note there is no way
        // for AllOf to satisfy Abortable in nontrivial cases (only with zero
        // or one awaitable).
        if constexpr (Skippable<AllOf> && Abortable<AllOf>) {
            CORRAL_ASSERT(!ret);
            return std::false_type{};
        } else {
            return ret;
        }
    }

    auto await_resume() && {
        this->handleResumeWithoutSuspend();
        auto impl = [](auto&&... children) {
            return std::make_tuple(std::move(children).result()...);
        };
        using Ret = decltype(Policy::wrapValue(
                std::apply(impl, std::move(this->children()))));

        if (!this->hasError()) {
            return Policy::wrapValue(
                    std::apply(impl, std::move(this->children())));
        } else if constexpr (PolicyUsesErrorCodes<Policy>) {
            return Ret{this->wrapError()};
        } else {
            this->wrapError();
            CORRAL_ASSERT_UNREACHABLE();
        }
    }

    void await_introspect(auto& c) const noexcept {
        this->introspect("AllOf", c);
    }
};


//
// Range-based combiners
//

template <class Policy, class Self, class Range>
class MuxRange : public MuxBase<Policy, Self> {
    using Awaitable = decltype(*std::declval<Range>().begin());
    using Helper = MuxHelper<Policy, MuxRange, Awaitable>;

    struct ChildWithoutAwaitable {
        Helper helper;
        explicit ChildWithoutAwaitable(Awaitable&& awaitable)
          : helper(std::forward<Awaitable>(awaitable)) {}
    };

    struct ChildWithAwaitable {
        Awaitable awaitable;
        Helper helper;
        explicit ChildWithAwaitable(Awaitable&& aw)
          : awaitable(std::forward<Awaitable>(aw)),
            helper(std::forward<Awaitable>(awaitable)) {}
    };

  protected:
    using Child = std::conditional_t<std::is_reference_v<Awaitable>,
                                     ChildWithoutAwaitable,
                                     ChildWithAwaitable>;
    using ChildValueType = typename Helper::ValueType;

  public:
    // See note in MuxBase::await_cancel() regarding why we only can propagate
    // Abortable if the mux completes when its first awaitable does
    static constexpr bool muxIsAbortable() {
        return Self::doneOnFirstReady && Abortable<AwaiterType<Awaitable>>;
    }
    static constexpr bool muxIsSkippable() {
        return Skippable<AwaiterType<Awaitable>>;
    }

    long size() const { return count_; }

    explicit MuxRange(Range&& range) {
        children_ = reinterpret_cast<Child*>(operator new(
                sizeof(Child) * range.size(),
                std::align_val_t{alignof(Child)}));

        Child* p = children_;
#if __cpp_exceptions
        try {
#endif
            for (Awaitable&& awaitable : range) {
                new (p) Child(std::forward<Awaitable>(awaitable));
                ++p;
            }
#if __cpp_exceptions
        } catch (...) {
            while (p != children_) {
                (--p)->~Child();
            }
            operator delete(children_, std::align_val_t{alignof(Child)});
            throw;
        }
#endif

        count_ = p - children_;
    }

    ~MuxRange() {
        for (Child* p = children_ + count_; p != children_;) {
            (--p)->~Child();
        }
        operator delete(children_, std::align_val_t{alignof(Child)});
    }

    MuxRange(MuxRange&&) = delete;
    MuxRange(const MuxRange&) = delete;

    void await_set_executor(Executor* ex) noexcept {
        for (auto& child : children()) {
            child.helper.setExecutor(ex);
        }
    }

    bool await_ready() const noexcept {
        if (count_ == 0) {
            return true;
        }
        long nReady = 0;
        bool allCanSkipKickoff = true;
        for (auto& child : children()) {
            if (child.helper.ready()) {
                ++nReady;
            } else if (!child.helper.skippable()) {
                allCanSkipKickoff = false;
            }
        }
        return nReady >= this->self().minReady() && allCanSkipKickoff;
    }

    bool await_suspend(Handle h) {
        bool ret = this->doSuspend(h);
        for (auto& child : children()) {
            child.helper.bind(*this);
        }
        for (auto& child : children()) {
            child.helper.suspend();
        }
        return ret;
    }

    auto await_resume() && {
        handleResumeWithoutSuspend();
        std::vector<Optional<InhabitedType<ChildValueType>>> ret;
        using Ret = decltype(Policy::wrapValue(std::move(ret)));

        if (!this->hasError()) {
            ret.reserve(count_);
            for (auto& child : children()) {
                ret.emplace_back(std::move(child.helper).asOptional());
            }
            return Policy::wrapValue(std::move(ret));
        } else if constexpr (PolicyUsesErrorCodes<Policy>) {
            return Ret{this->wrapError()};
        } else {
            this->wrapError();
            CORRAL_ASSERT_UNREACHABLE();
        }
    }

    bool internalCancel() noexcept {
        bool allNowCancelled = true;
        for (auto& child : children()) {
            allNowCancelled &= child.helper.cancel();
        }
        return allNowCancelled;
    }

    auto await_must_resume() const noexcept {
        bool anyMustResume = false;
        for (auto& child : children()) {
            anyMustResume |= child.helper.mustResume();
        }

        // See note in MuxTuple::await_must_resume()
        if constexpr (Skippable<Self> && Abortable<Self>) {
            CORRAL_ASSERT(!anyMustResume);
            return std::false_type{};
        } else {
            return anyMustResume;
        }
    }

    static constexpr bool doneOnFirstReady = false;

  protected:
    std::span<Child> children() { return std::span(children_, count_); }
    std::span<const Child> children() const {
        return std::span(children_, count_);
    }

    void handleResumeWithoutSuspend() {
        if (count_ != 0 && !children_[0].helper.isBound()) {
            // We skipped await_suspend because all children were
            // ready or sync-cancellable. (All of the MuxHelpers have
            // bind() called at the same time; we check the first one
            // for convenience only.)
            this->doSuspend(std::noop_coroutine());
            for (auto& child : children()) {
                child.helper.bind(*static_cast<Self*>(this));
            }

            // Whichever child resumes first may get to cancel its siblings
            // if we're in anyOf(), so we need to make sure we first
            // collect the result of whichever child is ready.

            Child* readyChild = children_;
            Child* end = children_ + count_;
            while (readyChild != end && !readyChild->helper.ready()) {
                ++readyChild;
            }
            CORRAL_ASSERT(readyChild != end);
            readyChild->helper.reportImmediateResult();

            // Now collect the results of the remaining children.
            for (Child* p = children_; p != readyChild; ++p) {
                p->helper.reportImmediateResult();
            }
            for (Child* p = readyChild + 1; p != end; ++p) {
                p->helper.reportImmediateResult();
            }
        }
    }

    void introspect(const char* name, auto& c) const noexcept {
        c.node(name, this);
        for (auto& child : children()) {
            child.helper.introspect(c);
        }
    }

  private:
    Child* children_;
    long count_;
};

template <class Policy, class Range>
class AnyOfRange : public MuxRange<Policy, AnyOfRange<Policy, Range>, Range> {
    using Child = decltype(*std::declval<Range>().begin());
    static_assert(Cancellable<AwaiterType<Child>>,
                  "anyOf() makes no sense for non-cancellable awaitables");

  public:
    using AnyOfRange::MuxRange::MuxRange;

    long minReady() const noexcept { return std::min<long>(1, this->size()); }
    static constexpr bool doneOnFirstReady = true;

    void await_introspect(auto& c) const noexcept {
        this->introspect("AnyOf (range)", c);
    }
};

template <class Policy, class Range>
class MostOfRange : public MuxRange<Policy, MostOfRange<Policy, Range>, Range> {
  public:
    using MostOfRange::MuxRange::MuxRange;
    long minReady() const noexcept { return this->size(); }
    void await_introspect(auto& c) const noexcept {
        this->introspect("MostOf (range)", c);
    }
};

template <class Policy, class Range>
class AllOfRange : public MuxRange<Policy, AllOfRange<Policy, Range>, Range> {
    using Awaitable = decltype(*std::declval<Range>().begin());

  public:
    using AllOfRange::MuxRange::MuxRange;
    long minReady() const noexcept { return this->size(); }

    bool await_must_resume() const noexcept {
        bool allMustResume = (this->size() > 0);
        for (auto& child : this->children()) {
            allMustResume &= child.helper.mustResume();
        }
        return this->hasError() || allMustResume;
    }

    auto await_resume() && {
        this->handleResumeWithoutSuspend();
        std::vector<InhabitedType<typename AllOfRange::ChildValueType>> ret;
        using Ret = decltype(Policy::wrapValue(std::move(ret)));

        if (!this->hasError()) {
            ret.reserve(this->children().size());
            for (auto& child : this->children()) {
                ret.emplace_back(std::move(child.helper).result());
            }
            return Policy::wrapValue(ret);
        } else if constexpr (PolicyUsesErrorCodes<Policy>) {
            return Ret{this->wrapError()};
        } else {
            this->wrapError();
            CORRAL_ASSERT_UNREACHABLE();
        }
    }

    void await_introspect(auto& c) const noexcept {
        this->introspect("AllOf (range)", c);
    }
};

} // namespace corral::detail
