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
#include <algorithm>
#include <tuple>
#include <variant>

#include "../Task.h"
#include "../concepts.h"
#include "../config.h"
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
template <class MuxT, class Aw> class MuxHelper : public ProxyFrame {
    using Self = MuxHelper<MuxT, Aw>;
    using Ret = AwaitableReturnType<Aw>;
    using StorageType = typename Storage<Ret>::Type;

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
    MuxHelper(Aw&& aw)
      : mux_(nullptr, State::NotStarted), awaitable_(std::forward<Aw>(aw)) {}

    MuxHelper(MuxHelper&& rhs) noexcept(
            // NOLINTNEXTLINE(performance-noexcept-move-constructor)
            std::is_nothrow_move_constructible_v<Aw>)
      : mux_(rhs.mux_), awaitable_(std::move(rhs.awaitable_)) {
        CORRAL_ASSERT(state() == State::NotStarted);
    }
    MuxHelper& operator=(MuxHelper rhs) noexcept(
            // NOLINTNEXTLINE(performance-noexcept-move-constructor)
            std::is_nothrow_move_constructible_v<Aw>) {
        CORRAL_ASSERT(state() == State::NotStarted);
        std::swap(mux_, rhs.mux_);
        std::swap(awaitable_, rhs.awaitable_);
    }
    ~MuxHelper() {
        if (state() == State::Succeeded) {
            reinterpret_cast<StorageType*>(storage_)->~StorageType();
        }
    }

    static bool skippable() { return Skippable<Aw>; }

    void setExecutor(Executor* ex) noexcept {
        if (state() == State::NotStarted ||
            state() == State::CancellationPending) {
            awaitable_.await_set_executor(ex);
        }
    }

    bool ready() const noexcept {
        switch (state()) {
            case State::NotStarted:
                return awaitable_.await_ready();
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
                mux()->invoke(nullptr);
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
                if (awaitable_.await_early_cancel()) {
                    setState(State::Cancelled);
                    if (MuxT* m = mux()) {
                        m->invoke(nullptr);
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
                if (awaitable_.await_cancel(this->toHandle())) {
                    setState(State::Cancelled);
                    mux()->invoke(nullptr);
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
    // The precondition for this is the mux await_ready(): enough awaitables
    // are ready and the rest are skippable.
    void reportImmediateResult() {
        if (state() == State::CancellationPending) {
            // Early-cancel failed then awaitable was ready -> must check
            // await_must_resume. This would have happened in mustResume()
            // if the cancellation came from outside, but happens here
            // otherwise (e.g. AnyOf cancelling remaining awaitables after
            // the first one completes).
            setState(awaitable_.await_must_resume() ? State::Ready
                                                    : State::Cancelled);
        }
        if (state() == State::Cancelled) {
            // Already cancelled, just need to notify the mux (we weren't
            // bound yet when the Cancelled state was entered).
            mux()->invoke(nullptr);
        } else if (state() == State::NotStarted && !awaitable_.await_ready()) {
            // Awaitable was not needed. await_ready() would not have
            // returned true unless it was Skippable, which we assert here;
            // cancel() will call mux()->invoke(nullptr).
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

    Ret result() && {
        CORRAL_ASSERT(state() == State::Succeeded);
        return Storage<Ret>::unwrap(
                std::move(*reinterpret_cast<StorageType*>(storage_)));
    }

    Optional<Ret> asOptional() && {
        switch (state()) {
            case State::Succeeded:
                return Storage<Ret>::unwrap(
                        std::move(*reinterpret_cast<StorageType*>(storage_)));
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
                bool shouldResume = awaitable_.await_must_resume();
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
                c.child(awaitable_);
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
        if (awaitable_.await_ready()) {
            invoke();
        } else {
            resumeFn = +[](CoroutineFrame* frame) {
                static_cast<Self*>(frame)->invoke();
            };
            try {
                awaitable_.await_suspend(this->toHandle()).resume();
            } catch (...) {
                std::exception_ptr ex = std::current_exception();
                CORRAL_ASSERT(ex && "foreign exceptions and forced unwinds are "
                                    "not supported");
                setState(State::Failed);
                mux()->invoke(ex);
            }
        }
    }

    void reportResult() {
        std::exception_ptr ex = nullptr;
        try {
            setState(State::Succeeded);
            new (storage_)
                    StorageType(Storage<Ret>::wrap(awaitable_.await_resume()));
        } catch (...) {
            setState(State::Failed);
            ex = std::current_exception();
            CORRAL_ASSERT(
                    ex &&
                    "foreign exceptions and forced unwinds are not supported");
        }
        mux()->invoke(ex);
    }

    void invoke() {
        switch (state()) {
            case State::Cancelling:
                if (!awaitable_.await_must_resume()) {
                    setState(State::Cancelled);
                    mux()->invoke(nullptr);
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

    AwaitableAdapter<Aw> awaitable_;
    alignas(StorageType) char storage_[sizeof(StorageType)];
};


template <class Self> class MuxBase {
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
        // completed yet then none of the individual awaitables have completed,
        // so await_cancel() of the mux will always succeed synchronously too.
        // This is not true for AllOf/MostOf, because some of the awaitables
        // might have already completed before the cancellation occurs.
        if constexpr (Self::muxIsAbortable()) {
            CORRAL_ASSERT(allNowCancelled);
            return std::true_type{};
        } else {
            if (allNowCancelled) {
                return true;
            }
            if (count_ == self().size()) {
                // We synchronously cancelled the remaining awaitables, but
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

    void reraise() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    bool hasException() const noexcept { return exception_ != nullptr; }

  private:
    void invoke(std::exception_ptr ex) {
        long i = ++count_;
        bool firstFail = (ex && !exception_);
        if (firstFail) {
            exception_ = ex;
        }
        CORRAL_TRACE("Mux<%lu/%lu> %p invocation %lu%s", self().minReady(),
                     self().size(), this, i, (ex ? " with exception" : ""));
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
    std::exception_ptr exception_;

    template <class, class> friend class MuxHelper;
};


//
// Tuple-based combiners
//

/// Implementation of anyOf(), mostOf(), and a building block for allOf().
template <class Self, class... Awaitables>
class MuxTuple : public MuxBase<Self> {
  public:
    explicit MuxTuple(Awaitables&&... awaitables)
      : awaitables_(MuxHelper<Self, Awaitables>(
                std::forward<Awaitables>(awaitables))...) {}

    // See note in MuxBase::await_cancel() regarding why we only can propagate
    // Abortable if the mux completes when its first awaitable does
    static constexpr bool muxIsAbortable() {
        return Self::minReady() == 1 && (Abortable<Awaitables> && ...);
    }
    static constexpr bool muxIsSkippable() {
        return (Skippable<Awaitables> && ...);
    }
    static constexpr long size() noexcept { return sizeof...(Awaitables); }

    void await_set_executor(Executor* ex) noexcept {
        auto impl = [ex](auto&... awaitables) {
            (awaitables.setExecutor(ex), ...);
        };
        std::apply(impl, awaitables_);
    }

    bool await_ready() const noexcept {
        auto impl = [](const auto&... aws) {
            long nReady = (static_cast<long>(aws.ready()) + ...);
            long nSkipKickoff =
                    (static_cast<long>(aws.ready() || aws.skippable()) + ...);
            return nReady >= Self::minReady() &&
                   nSkipKickoff == sizeof...(Awaitables);
        };
        return std::apply(impl, awaitables_);
    }

    bool await_suspend(Handle h) {
        bool ret = this->doSuspend(h);
        auto impl = [this](auto&... awaitables) {
            (awaitables.bind(*static_cast<Self*>(this)), ...);
            (awaitables.suspend(), ...);
        };
        std::apply(impl, awaitables_);
        return ret;
    }

    std::tuple<Optional<AwaitableReturnType<Awaitables>>...> await_resume() && {
        handleResumeWithoutSuspend();
        this->reraise();
        auto impl = [](auto&&... awaitables) {
            return std::make_tuple(std::move(awaitables).asOptional()...);
        };
        return std::apply(impl, std::move(awaitables_));
    }

    // This is called in two situations:
    // - enough awaitables have completed and we want to cancel the rest
    // - we get a cancellation from the outside (await_cancel)
    // Returns true if every awaitable is now cancelled (i.e., we
    // definitely have no results to report to our parent), which is
    // only relevant to await_cancel().
    bool internalCancel() {
        auto impl = [](auto&... awaitables) {
            // Don't short-circuit; we should try to cancel every remaining
            // awaitable, even if some have completed already.
            return (... & int(awaitables.cancel()));
        };
        return std::apply(impl, awaitables_);
    }

    auto await_must_resume() const noexcept {
        auto impl = [](const auto&... awaitables) {
            // Don't short-circuit; we should check mustResume of every
            // awaitable, since it can have side effects.
            return (... | int(awaitables.mustResume()));
        };
        bool anyMustResume = std::apply(impl, awaitables_);

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
    auto& awaitables() { return awaitables_; }
    const auto& awaitables() const { return awaitables_; }

    void handleResumeWithoutSuspend() {
        if (!std::get<0>(awaitables_).isBound()) {
            // We skipped await_suspend because all awaitables were
            // ready or sync-early-cancellable. (All of the MuxHelpers
            // have bind() called at the same time; we check the first
            // one for convenience only.)
            this->doSuspend(std::noop_coroutine());
            auto extractResults = [this](auto&&... awaitables) {
                (awaitables.bind(*static_cast<Self*>(this)), ...);
                (awaitables.reportImmediateResult(), ...);
            };
            std::apply(extractResults, awaitables_);
        }
    }

    void introspect(const char* name, auto& c) const noexcept {
        c.node(name);
        auto impl = [&c](const auto&... awaitables) {
            (awaitables.introspect(c), ...);
        };
        std::apply(impl, awaitables_);
    }

  private:
    std::tuple<MuxHelper<Self, Awaitables>...> awaitables_;
};

// Specialization for zero awaitables:
template <class Self> class MuxTuple<Self> : public MuxBase<Self> {
  public:
    static constexpr bool muxIsAbortable() { return true; }
    static constexpr bool muxIsSkippable() { return true; }
    static constexpr long size() noexcept { return 0; }
    bool await_ready() const noexcept { return true; }
    bool await_suspend(Handle) { return false; }
    std::tuple<> await_resume() && { return {}; }
    bool internalCancel() noexcept { return true; }

  protected:
    std::tuple<> awaitables() const { return {}; }
    void handleResumeWithoutSuspend() {}
    void introspect(const char* name, auto& c) const { c.node(name); }
};

template <class... Awaitables>
class AnyOf : public MuxTuple<AnyOf<Awaitables...>, Awaitables...> {
  public:
    using AnyOf::MuxTuple::MuxTuple;
    static constexpr long minReady() noexcept { return 1; }
    void await_introspect(auto& c) const noexcept {
        this->introspect("AnyOf", c);
    }
};

template <class... Awaitables>
class MostOf : public MuxTuple<MostOf<Awaitables...>, Awaitables...> {
  public:
    using MostOf::MuxTuple::MuxTuple;
    static constexpr long minReady() noexcept { return sizeof...(Awaitables); }
    void await_introspect(auto& c) const noexcept {
        this->introspect("MostOf", c);
    }
};

template <class... Awaitables>
class AllOf : public MuxTuple<AllOf<Awaitables...>, Awaitables...> {
  public:
    using AllOf::MuxTuple::MuxTuple;

    static constexpr long minReady() noexcept { return sizeof...(Awaitables); }

    auto await_must_resume() const noexcept {
        // We only require the parent to be resumed if *all* awaitables
        // have a value to report, so we'd be able to construct a tuple of
        // results (or if any awaitable failed, so we'd reraise and not
        // bother with return value construction).
        auto impl = [](const auto&... awaitables) {
            if constexpr (sizeof...(awaitables) == 0) {
                return false;
            } else {
                return (... & int(awaitables.mustResume()));
            }
        };
        bool ret = this->hasException() || std::apply(impl, this->awaitables());

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

    std::tuple<AwaitableReturnType<Awaitables>...> await_resume() && {
        this->handleResumeWithoutSuspend();
        this->reraise();
        auto impl = [](auto&&... awaitables) {
            return std::make_tuple(std::move(awaitables).result()...);
        };
        return std::apply(impl, std::move(this->awaitables()));
    }

    void await_introspect(auto& c) const noexcept {
        this->introspect("AllOf", c);
    }
};


//
// Range-based combiners
//

template <class Self, class Range> class MuxRange : public MuxBase<Self> {
    using Item = decltype(*std::declval<Range>().begin());

  public:
    // See note in MuxBase::await_cancel() regarding why we only can propagate
    // Abortable if the mux completes when its first awaitable does
    static constexpr bool muxIsAbortable() {
        return Self::doneOnFirstReady && Abortable<AwaitableType<Item>>;
    }
    static constexpr bool muxIsSkippable() {
        return Skippable<AwaitableType<Item>>;
    }

    long size() const { return static_cast<long>(awaitables_.size()); }

    explicit MuxRange(Range&& range) {
        awaitables_.reserve(range.size());
        for (auto& awaitable : range) {
            awaitables_.emplace_back(getAwaitable(std::move(awaitable)));
        }
    }

    void await_set_executor(Executor* ex) noexcept {
        for (auto& awaitable : awaitables_) {
            awaitable.setExecutor(ex);
        }
    }

    bool await_ready() const noexcept {
        if (awaitables_.empty()) {
            return true;
        }
        long nReady = 0;
        bool allCanSkipKickoff = true;
        for (auto& awaitable : awaitables_) {
            if (awaitable.ready()) {
                ++nReady;
            } else if (!awaitable.skippable()) {
                allCanSkipKickoff = false;
            }
        }
        return nReady >= this->self().minReady() && allCanSkipKickoff;
    }

    bool await_suspend(Handle h) {
        bool ret = this->doSuspend(h);
        for (auto& awaitable : awaitables_) {
            awaitable.bind(*this);
        }
        for (auto& awaitable : awaitables_) {
            awaitable.suspend();
        }
        return ret;
    }

    std::vector<Optional<AwaitableReturnType<Item>>> await_resume() && {
        handleResumeWithoutSuspend();
        this->reraise();
        std::vector<Optional<AwaitableReturnType<Item>>> ret;
        ret.reserve(awaitables_.size());
        for (auto& awaitable : awaitables_) {
            ret.emplace_back(std::move(awaitable).asOptional());
        }
        return ret;
    }

    bool internalCancel() noexcept {
        bool allNowCancelled = true;
        for (auto& awaitable : awaitables_) {
            allNowCancelled &= awaitable.cancel();
        }
        return allNowCancelled;
    }

    auto await_must_resume() const noexcept {
        bool anyMustResume = false;
        for (auto& awaitable : awaitables_) {
            anyMustResume |= awaitable.mustResume();
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
    auto& awaitables() { return awaitables_; }
    const auto& awaitables() const { return awaitables_; }

    void handleResumeWithoutSuspend() {
        if (!awaitables_.empty() && !awaitables_[0].isBound()) {
            // We skipped await_suspend because all awaitables were
            // ready or sync-cancellable. (All of the MuxHelpers have
            // bind() called at the same time; we check the first one
            // for convenience only.)
            this->doSuspend(std::noop_coroutine());
            for (auto& awaitable : awaitables_) {
                awaitable.bind(*static_cast<Self*>(this));
            }
            for (auto& awaitable : awaitables_) {
                awaitable.reportImmediateResult();
            }
        }
    }

    void introspect(const char* name, auto& c) const noexcept {
        c.node(name);
        for (auto& awaitable : awaitables_) {
            awaitable.introspect(c);
        }
    }

  private:
    std::vector<MuxHelper<MuxRange<Self, Range>, AwaitableType<Item>>>
            awaitables_;
};

template <class Range>
class AnyOfRange : public MuxRange<AnyOfRange<Range>, Range> {
    using Item = decltype(*std::declval<Range>().begin());
    static_assert(Cancellable<AwaitableType<Item>>,
                  "anyOf() makes no sense for non-cancellable awaitables");

  public:
    using AnyOfRange::MuxRange::MuxRange;

    long minReady() const noexcept { return std::min<long>(1, this->size()); }
    static constexpr bool doneOnFirstReady = true;

    void await_introspect(auto& c) const noexcept {
        this->introspect("AnyOf (range)", c);
    }
};

template <class Range>
class MostOfRange : public MuxRange<MostOfRange<Range>, Range> {
  public:
    using MostOfRange::MuxRange::MuxRange;
    long minReady() const noexcept { return this->size(); }
    void await_introspect(auto& c) const noexcept {
        this->introspect("MostOf (range)", c);
    }
};

template <class Range>
class AllOfRange : public MuxRange<AllOfRange<Range>, Range> {
    using Item = decltype(*std::declval<Range>().begin());

  public:
    using AllOfRange::MuxRange::MuxRange;
    long minReady() const noexcept { return this->size(); }

    bool await_must_resume() const noexcept {
        bool allMustResume = (this->size() > 0);
        for (auto& awaitable : this->awaitables()) {
            allMustResume &= awaitable.mustResume();
        }
        return this->hasException() || allMustResume;
    }

    std::vector<AwaitableReturnType<Item>> await_resume() && {
        this->handleResumeWithoutSuspend();
        this->reraise();
        std::vector<AwaitableReturnType<Item>> ret;
        ret.reserve(this->awaitables().size());
        for (auto& awaitable : this->awaitables()) {
            ret.emplace_back(std::move(awaitable).result());
        }
        return ret;
    }

    void await_introspect(auto& c) const noexcept {
        this->introspect("AllOf (range)", c);
    }
};


} // namespace corral::detail
