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

#include "../concepts.h"
#include "../config.h"
#include "frames.h"
#include "utility.h"

namespace corral::detail {

template <Awaitable First, class ThenFn>
class Sequence : private ProxyFrame, private Noncopyable {
    static decltype(auto) getSecond(ThenFn& fn,
                                    AwaitableReturnType<First>& first) {
        if constexpr (requires { fn(std::move(first)); }) {
            return fn(std::move(first));
        } else if constexpr (requires { fn(first); }) {
            return fn(first);
        } else {
            return fn();
        }
    }

    using Second =
            decltype(getSecond(std::declval<ThenFn&>(),
                               std::declval<AwaitableReturnType<First>&>()));

    enum class Stage : uint8_t { None, FirstStage, SecondStage, Exception };

  public:
    Sequence(First first, ThenFn thenFn)
      : first_(std::move(first)), thenFn_(std::move(thenFn)) {}

    ~Sequence() { switchTo<Stage::None>(); }

    bool await_ready() const noexcept { return false; }

    void await_set_executor(Executor* e) noexcept {
        switchTo<Stage::FirstStage>(e);
        first_.awaiter.await_set_executor(e);
    }

    auto await_early_cancel() noexcept {
        cancelling_ = true;
        return first_.awaiter.await_early_cancel();
    }

    void await_suspend(Handle h) {
        CORRAL_TRACE("   ...sequence %p yielding to...", this);
        parent_ = h;
        if (first_.awaiter.await_ready()) {
            kickOffSecond();
        } else {
            this->resumeFn = +[](CoroutineFrame* frame) {
                auto* self = static_cast<Sequence*>(frame);
                self->kickOffSecond();
            };
            first_.awaiter.await_suspend(this->toHandle()).resume();
        }
    }

    bool await_cancel(Handle h) noexcept {
        CORRAL_TRACE("sequence %p (%s stage) cancellation requested", this,
                     inFirstStage() ? "first" : "second");
        cancelling_ = true;
        if (inFirstStage()) {
            return first_.awaiter.await_cancel(this->toHandle());
        } else if (inSecondStage()) {
            return secondStage_.awaiter.await_cancel(h);
        } else {
            return false; // will carry out cancellation later
        }
    }

    bool await_must_resume() const noexcept {
        // Note that await_must_resume() is called by our parent when we
        // resume them after a cancellation that did not complete synchronously.
        // To understand the logic in this method, consider all the places
        // where we call parent_.resume(). In particular, if we're still
        // inFirstStage(), we must have hit the cancellation check at the
        // beginning of kickOffSecond(), which means we've already verified
        // that the first stage await_must_resume() returned false, and we
        // should return false here without consulting the awaitable further.
        // Similarly, if we're in neither the first nor the second stage,
        // the second stage must have completed via early cancellation.
        bool ret =
                (stage_ == Stage::Exception) ||
                (inSecondStage() && secondStage_.awaiter.await_must_resume());
        if (!ret && inSecondStage()) {
            // Destroy the second stage, which will release any resources
            // it might have held
            stage_ = Stage::None;
            secondStage_.~SecondStage();
        }
        return ret;
    }

    decltype(auto) await_resume() {
        ScopeGuard guard([this] {
            // Destroy the second stage and the return value of the first stage
            switchTo<Stage::None>();
        });

        if (stage_ == Stage::Exception) {
            rethrow_exception(exception_);
        } else {
            CORRAL_ASSERT(inSecondStage());
            return secondStage_.awaiter.await_resume();
        }
    }

    void await_introspect(auto& c) const noexcept {
        if (inFirstStage()) {
            first_.awaiter.await_introspect(c);
        } else if (inSecondStage()) {
            secondStage_.awaiter.await_introspect(c);
        } else {
            c.node("sequence (degenerate)", this);
        }
    }

  private:
    struct FirstStage {
        CORRAL_NO_UNIQUE_ADDR First awaitable;
        CORRAL_NO_UNIQUE_ADDR SanitizedAwaiter<First> awaiter;

        explicit FirstStage(First&& aw)
          : awaitable(std::forward<First>(aw)),
            awaiter(std::forward<First>(awaitable)) {}
    };

    // Explicitly provide a template argument, so immediate awaitables
    // would resolve to Second&& instead of Second.
    // For the same reason, don't use AwaitableType<> here.
    using SecondAwaiter =
            decltype(getAwaiter<Second&&>(std::declval<Second>()));

    struct SecondStage {
        CORRAL_NO_UNIQUE_ADDR AwaitableReturnType<First> firstValue;
        CORRAL_NO_UNIQUE_ADDR Second awaitable;
        CORRAL_NO_UNIQUE_ADDR SanitizedAwaiter<Second&&, SecondAwaiter> awaiter;

        explicit SecondStage(Sequence* c)
          : firstValue(std::move(c->first_.awaiter).await_resume()),
            awaitable(getSecond(c->thenFn_, firstValue)),
            awaiter(std::forward<Second>(awaitable)) {}
    };

    bool inFirstStage() const noexcept { return stage_ == Stage::FirstStage; }
    bool inSecondStage() const noexcept { return stage_ == Stage::SecondStage; }

    void kickOffSecond() noexcept {
        if (cancelling_ && !first_.awaiter.await_must_resume()) {
            CORRAL_TRACE("sequence %p (cancelling) first stage completed, "
                         "confirming cancellation",
                         this);
            parent_.resume();
            return;
        }

        CORRAL_TRACE("sequence %p%s first stage completed, continuing with...",
                     this, cancelling_ ? " (cancelling)" : "");
        CORRAL_ASSERT(inFirstStage());
        Executor* ex = executor_;

#if __cpp_exceptions
        try {
#endif
            switchTo<Stage::SecondStage>(this);

#if __cpp_exceptions
        } catch (...) {
            switchTo<Stage::Exception>(current_exception());
            parent_.resume();
            return;
        }
#endif

        if (cancelling_) {
            if (secondStage_.awaiter.await_early_cancel()) {
                switchTo<Stage::None>();

                parent_.resume();
                return;
            }
        }

        if (secondStage_.awaiter.await_ready()) {
            parent_.resume();
        } else {
            secondStage_.awaiter.await_set_executor(ex);
            secondStage_.awaiter.await_suspend(parent_).resume();
        }
    }

    template <Stage NewStage, class... Args> void switchTo(Args&&... args) {
        Stage oldStage = std::exchange(stage_, Stage::None);

        if (oldStage == Stage::SecondStage) {
            secondStage_.~SecondStage();
        } else if (oldStage == Stage::Exception) {
            exception_.~exception_ptr();
        }

        if constexpr (NewStage == Stage::FirstStage) {
            new (&executor_) Executor*(std::forward<Args>(args)...);
        } else if constexpr (NewStage == Stage::SecondStage) {
            new (&secondStage_) SecondStage(std::forward<Args>(args)...);
        } else if constexpr (NewStage == Stage::Exception) {
            new (&exception_) std::exception_ptr(std::forward<Args>(args)...);
        }

        stage_ = NewStage;
    }

  private:
    Handle parent_;
    CORRAL_NO_UNIQUE_ADDR FirstStage first_;

    CORRAL_NO_UNIQUE_ADDR ThenFn thenFn_;

    union {
        Executor* executor_;              // valid if stage_ == FirstStage
        mutable SecondStage secondStage_; // valid if stage_ == SecondStage
        std::exception_ptr exception_;    // valid if stage_ == Exception
    };
    bool cancelling_ = false;
    mutable Stage stage_ = Stage::None;
};

template <class ThenFn> class SequenceBuilder {
  public:
    explicit SequenceBuilder(ThenFn fn) : fn_(std::move(fn)) {}

    template <Awaitable First>
        requires(std::invocable<ThenFn, AwaitableReturnType<First>&> ||
                 std::invocable<ThenFn, AwaitableReturnType<First> &&> ||
                 std::invocable<ThenFn>)
    friend auto operator|(First&& first, SequenceBuilder&& builder) {
        return makeAwaitable<Sequence<First, ThenFn>, First, ThenFn>(
                std::forward<First>(first), std::forward<ThenFn>(builder.fn_));
    }

    // Allow right associativity of SequenceBuilder's
    template <class ThirdFn>
    auto operator|(SequenceBuilder<ThirdFn>&& next) && {
        return corral::detail::SequenceBuilder(
                [fn = std::move(fn_),
                 next = std::move(next)]<class T>(T&& value) mutable {
                    return fn(std::forward<T>(value)) | std::move(next);
                });
    }

  private:
    ThenFn fn_;
};

} // namespace corral::detail
