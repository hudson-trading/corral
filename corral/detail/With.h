// This file is part of corral, a lightweight C++20 coroutine library.
//
// Copyright (c) 2024-2026 Hudson River Trading LLC
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

#include "../ErrorPolicy.h"
#include "../Nursery.h"
#include "Sequence.h"
#include "frames.h"
#include "utility.h"

namespace corral::detail {

//
// With
//

template <class Callable> decltype(auto) intermediateResultImpl() {
    if constexpr (std::is_invocable_v<Callable, TaskStarted<void>>) {
        // Support template callables (whose signature cannot be
        // evaluated through `CallableSignature<>`) which happen
        // to take `TaskStarted<void>`.
        return std::type_identity<void>{};
    } else {
        return std::type_identity<typename CallableSignature<
                Callable>::template Arg<0>::ResultType>{};
    }
}

template <class Callable>
using IntermediateResult =
        typename decltype(intermediateResultImpl<Callable>())::type;


template <class Fn, class Arg> struct ChainableForWithImpl {
    using ResultType = std::invoke_result_t<Fn, Arg>;
};
template <class Fn> struct ChainableForWithImpl<Fn, void> {
    using ResultType = std::invoke_result_t<Fn>;
};
template <class FirstFn, class ThenFn>
using ChainableForWith =
        ChainableForWithImpl<ThenFn, IntermediateResult<FirstFn>>;

// With class requires two ProxyFrames; this class serves as a second base
// to avoid inheritance ambiguity.
class WithBase : protected ProxyFrame {};

template <class PolicyArg, class FirstFn, class ThenFn>
class With : private TaskStartedSink<IntermediateResult<FirstFn>>,
             private WithBase,
             public SequenceBase<std::invoke_result_t<FirstFn, TaskStartedTag>,
                                 ThenFn,
                                 IntermediateResult<FirstFn>>,
             public YieldsLikeAwaitTag {
    using Base = SequenceBase<std::invoke_result_t<FirstFn, TaskStartedTag>,
                              ThenFn,
                              IntermediateResult<FirstFn>>;

    using First = std::invoke_result_t<FirstFn, TaskStartedTag>;
    using Policy = ChooseErrorPolicyForAwaitables<PolicyArg,
                                                  First,
                                                  typename With::Second>;

    using Ret = decltype(std::declval<typename With::SecondAwaiter>()
                                 .await_resume());

    using Xfer = InhabitedType<IntermediateResult<FirstFn>>;
    using RetValue = InhabitedType<Ret>;


    //
    // Per-stage flags
    //

    // Set if the stage's `await_suspend()` has been called.
    static constexpr uint16_t StageStarted = 1;

    // Set if the stage's `await_cancel()` or `await_early_cancel()` has been
    // called. Not set if the stage completed normally, or has not been yet
    // constructed.
    static constexpr uint16_t StageCancelling = 2;

    // Set if the stage invoked its continuation handle.
    static constexpr uint16_t StageDone = 4;

    // Set if cancellation completed synchronously, or if await_must_resume()
    // returned false.
    static constexpr uint16_t StageMustNotResume = 8;


    //
    // Global (not per-stage) flags
    //

    // Set if With's `await_cancel()` or `await_early_cancel()` has been called.
    static constexpr uint16_t Cancelling = 1;

    // Set if `result_` has been constructed.
    static constexpr uint16_t HasSecondResult = 2;


    // TMP helpers for handling stages uniformly
    struct FirstStage {
        static constexpr uint16_t Shift = 2;

        static auto exists(With&) noexcept { return std::true_type{}; }
        static auto& get(With& obj) noexcept { return obj.first_; }
        static CoroutineFrame* frame(With& obj) noexcept {
            return static_cast<WithBase*>(&obj);
        }
        static With& fromFrame(CoroutineFrame* frame) noexcept {
            return *static_cast<With*>(static_cast<WithBase*>(frame));
        }
    };

    struct SecondStage {
        static constexpr uint16_t Shift = 6;

        static bool exists(With& obj) noexcept { return obj.inSecondStage(); }
        static auto& get(With& obj) noexcept { return obj.secondStage_; }
        static CoroutineFrame* frame(With& obj) noexcept {
            return static_cast<Base*>(&obj);
        }
        static With& fromFrame(CoroutineFrame* frame) noexcept {
            return *static_cast<With*>(static_cast<Base*>(frame));
        }
    };

    template <class S> bool isStageStarted() const noexcept {
        return flags_ & (StageStarted << S::Shift);
    }
    template <class S> bool isStageCancelling() const noexcept {
        return flags_ & (StageCancelling << S::Shift);
    }
    template <class S> bool isStageDone() const noexcept {
        return flags_ & (StageDone << S::Shift);
    }
    template <class S> bool mustStageResume() const noexcept {
        return !(flags_ & (StageMustNotResume << S::Shift));
    }
    template <class S> void markStageStarted() noexcept {
        flags_ |= (StageStarted << S::Shift);
    }
    template <class S> void markStageDone() noexcept {
        flags_ |= (StageDone << S::Shift);
    }
    template <class S> void markStageCancelling() noexcept {
        flags_ |= (StageCancelling << S::Shift);
    }
    template <class S> void markStageMustNotResume() noexcept {
        flags_ |= (StageMustNotResume << S::Shift);
    }

    template <class S>
    using OtherStage = std::conditional_t<std::is_same_v<S, FirstStage>,
                                          SecondStage,
                                          FirstStage>;

  public:
    With(FirstFn firstFn, ThenFn thenFn)
      : With::SequenceBase(nullptr, std::forward<ThenFn>(thenFn)),
        firstFn_(std::forward<FirstFn>(firstFn)) //
    {
        // Note: this may immediately call markStarted(), which requires
        // fully initialized `this`.
        new (&this->first_) typename With::SequenceBase::FirstStageData(
                std::invoke(std::forward<FirstFn>(firstFn_),
                            With::TaskStartedSink::make()));
    }

    ~With() {
        if (flags_ & HasSecondResult) {
            using T = typename Storage<RetValue>::Type;
            result_.~T();
        }
    }

    bool await_ready() const noexcept {
        return isStageDone<FirstStage>() && isStageDone<SecondStage>();
    }

    Handle await_suspend(Handle h) {
        CORRAL_TRACE("   ...with-scope %p yielding to...", this);
        this->parent_ = h;

        if (this->inSecondStage()) {
            // `taskStated()` invoked before first stage's await_suspend();
            // fire both stages simulateneously.
            beginStage<FirstStage>().resume();
            return beginStage<SecondStage>();
        } else {
            return beginStage<FirstStage>();
        }
    }

    bool await_cancel(Handle) noexcept {
        CORRAL_TRACE("with-scope %p (%s stage) cancellation requested", this,
                     this->inFirstStage() ? "first" : "second");
        return doCancel();
    }

    bool await_early_cancel() noexcept { return doCancel(); }

    bool await_must_resume() const noexcept {
        CORRAL_ASSERT(!this->inSecondStage());

        return (this->stage_ == With::Stage::Exception) ||
               (mustStageResume<FirstStage>()) || (flags_ & HasSecondResult);
    }

    Ret await_resume() && {
        if (this->stage_ == With::Stage::Exception) {
            rethrow_exception(this->exception_);
        }

        if (mustStageResume<FirstStage>()) {
            // First stage completed. If any value was produced, it must have
            // been an error, so propagate it further up.
            if constexpr (std::is_same_v<AwaitableReturnType<First>, Void>) {
                std::move(this->first_.awaiter).await_resume();
                CORRAL_ASSERT(
                        !"Background awaitable completed without an error");
            } else {
                auto err = Policy::unwrapError(
                        std::move(this->first_.awaiter).await_resume());
                CORRAL_ASSERT(
                        Policy::hasError(err) &&
                        "Background awaitable completed without an error");

                if constexpr (PolicyUsesErrorCodes<Policy>) {
                    return Policy::wrapError(std::move(err));
                }
            }
        }

        if constexpr (!std::is_void_v<Ret>) {
            return Storage<RetValue>::unwrap(std::move(result_));
        }
    }

    void await_introspect(auto& c) const noexcept {
        c.node(this->inFirstStage() ? "with-scope (initializing)"
                                    : "with-scope (running)");
        if (!isStageDone<FirstStage>()) {
            c.child(this->first_.awaiter);
        }
        if (!isStageDone<SecondStage>()) {
            c.child(this->secondStage_.awaiter);
        }
    }

  private:
    template <class S> [[nodiscard]] Handle beginStage() {
        auto& awaiter = S::get(*this).awaiter;
        if (isStageDone<S>()) {
            return noopHandle();
        } else if (awaiter.await_ready()) {
            return onStageDone<S>();
        } else {
            if (flags_ & Cancelling) {
                markStageCancelling<S>();
                if (awaiter.await_early_cancel()) {
                    return onStageDone<S>();
                }
            }

            markStageStarted<S>();
            CoroutineFrame* frame = S::frame(*this);
            frame->resumeFn = +[](CoroutineFrame* fr) {
                S::fromFrame(fr).template onStageDone<S>().resume();
            };

#if __cpp_exceptions
            try {
#endif
                return awaiter.await_suspend(frame->toHandle());
#if __cpp_exceptions
            } catch (...) {
                this->template switchTo<With::Stage::Exception>(
                        std::current_exception());
                return onStageDone<S>();
            }
#endif
        }
    }

    template <class S> bool cancelStage() noexcept {
        if (isStageCancelling<S>()) {
            return false; // cancellation already in progress
        }

        bool cancelled = false;
        if (!S::exists(*this)) {
            return true; // stage not yet created
        } else if (isStageDone<S>()) {
            return true;
        } else if (isStageStarted<S>()) {
            markStageCancelling<S>();
            cancelled = S::get(*this).awaiter.await_cancel(
                    S::frame(*this)->toHandle());
        } else {
            markStageCancelling<S>();
            cancelled = S::get(*this).awaiter.await_early_cancel();
        }

        if (cancelled) {
            markStageMustNotResume<S>();
            markStageDone<S>();
        }
        return cancelled;
    }

    template <class S> [[nodiscard]] Handle onStageDone() noexcept {
        CORRAL_TRACE("with-scope %p %s stage done", this,
                     std::is_same_v<S, FirstStage> ? "first" : "second");

        markStageDone<S>();
        if (mustStageResume<S>() && isStageCancelling<S>() &&
            !S::get(*this).awaiter.await_must_resume()) //
        {
            // await_must_resume() cannot be called multiple times,
            // so memorize the result
            markStageMustNotResume<S>();
        }

        if constexpr (std::is_same_v<S, SecondStage>) {
            if (this->inSecondStage() && mustStageResume<S>()) {
                // We're going to need the second stage's result later
                // in await_resume(), but we need to destroy it right away
                // (see below), so stash the result away.

#if __cpp_exceptions
                try {
#endif
                    if constexpr (std::is_void_v<AwaitableReturnType<
                                          typename With::Second>>) {
                        S::get(*this).awaiter.await_resume();
                        new (&result_) typename Storage<void>::Type();
                    } else {
                        new (&result_) typename Storage<RetValue>::Type(
                                Storage<RetValue>::wrap(
                                        S::get(*this).awaiter.await_resume()));
                    }
                    flags_ |= HasSecondResult;
#if __cpp_exceptions
                } catch (...) {
                    this->template switchTo<With::Stage::Exception>(
                            std::current_exception());
                }
#endif
            }
            if (this->inSecondStage()) {
                // Destroy the second stage (to invoke any destructors / scope
                // guards) before commencing cancellation of the first stage
                this->template switchTo<With::Stage::None>();
            }
        }

        using O = OtherStage<S>;
        if (isStageDone<O>() || cancelStage<O>()) {
            CORRAL_TRACE("with-scope %p all stages done, resuming parent",
                         this);
            return this->parent_;
        } else {
            return noopHandle();
        }
    }

    bool doCancel() {
        flags_ |= Cancelling;
        if (this->inSecondStage()) {
            return cancelStage<SecondStage>() && cancelStage<FirstStage>();
            // Note: bool eval short-circuiting is essential here
            // (first stage's cancellation should not commence until second
            // stage completes)
        } else if (!this->inFirstStage()) {
            return true;
        } else {
            return (cancelStage<FirstStage>() && !this->inSecondStage());
        }
    }

    void markStarted(Xfer value) noexcept override {
        CORRAL_TRACE("with-scope %p first stage ready", this);
        CORRAL_ASSERT(!isStageDone<FirstStage>() &&
                      "markStarted() called after the stage is done");

        if (isStageCancelling<FirstStage>()) {
            // The first stage still runs, but has a pending cancellation,
            // and can cease to execute at any moment; so we cannot safely
            // proceed to the second stage.
            markStageDone<SecondStage>();
            return;
        }

        // Note: this can get invoked before `await_suspend()`
        Executor* executor = (this->inFirstStage() ? this->executor_ : nullptr);

#if __cpp_exceptions
        try {
#endif
            this->template switchTo<With::Stage::SecondStage>(
                    this, std::forward<Xfer>(value));
#if __cpp_exceptions
        } catch (...) {
            this->template switchTo<With::Stage::Exception>(
                    std::current_exception());
            if (cancelStage<FirstStage>()) {
                this->parent_.resume();
            }
            return;
        }
#endif

        if ((flags_ & Cancelling) && cancelStage<SecondStage>()) {
            onStageDone<SecondStage>().resume();
        } else if (executor) {
            this->secondStage_.awaiter.await_set_executor(executor);
            beginStage<SecondStage>().resume();
        } else {
            // second stage will get kicked off from await_suspend()
        }
    }

  private:
    FirstFn firstFn_;
    union {
        CORRAL_NO_UNIQUE_ADDR typename Storage<RetValue>::Type result_;
    };
    uint16_t flags_ = 0;
};

template <class FirstFn> class WithBuilder {
  public:
    explicit WithBuilder(FirstFn firstFn) : firstFn_(std::move(firstFn)) {}

    template <class SecondFn> auto operator%(SecondFn secondFn) && {
        return With<Unspecified, FirstFn, SecondFn>(std::move(firstFn_),
                                                    std::move(secondFn));
    }

  private:
    FirstFn firstFn_;
};

} // namespace corral::detail
