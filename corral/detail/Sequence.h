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


template <Awaitable First, class ThenFn, class Xfer>
class SequenceBase : protected ProxyFrame, private Noncopyable {
    static decltype(auto) getSecond(ThenFn& fn, InhabitedType<Xfer>&& xf) {
        if constexpr (std::is_void_v<Xfer>) {
            return fn();
        } else if constexpr (requires { fn(std::forward<Xfer>(xf)); }) {
            return fn(std::forward<Xfer>(xf));
        } else if constexpr (requires { fn(xf); }) {
            return fn(xf);
        } else {
            return fn();
        }
    }

  public:
    ~SequenceBase() {
        switchTo<Stage::None>();
        first_.~FirstStageData();
    }

    void await_set_executor(Executor* e) noexcept {
        first_.awaiter.await_set_executor(e);
        if (inSecondStage()) {
            if (!secondStage_.awaiter.await_ready()) {
                secondStage_.awaiter.await_set_executor(e);
            }
        } else {
            CORRAL_ASSERT(stage_ == Stage::None);
            switchTo<Stage::FirstStage>(e);
        }
    }

  protected:
    enum class Stage : uint8_t { None, FirstStage, SecondStage, Exception };

    struct FirstStageData {
        CORRAL_NO_UNIQUE_ADDR First awaitable;
        CORRAL_NO_UNIQUE_ADDR SanitizedAwaiter<First> awaiter;

        explicit FirstStageData(First&& aw)
          : awaitable(std::forward<First>(aw)),
            awaiter(std::forward<First>(awaitable)) {}
    };

    using Second = decltype(getSecond(std::declval<ThenFn&>(),
                                      std::declval<InhabitedType<Xfer>>()));

    // Explicitly provide a template argument, so immediate awaitables
    // would resolve to Second&& instead of Second.
    // For the same reason, don't use AwaitableType<> here.
    using SecondAwaiter =
            decltype(getAwaiter<Second&&>(std::declval<Second>()));

    struct SecondStageData {
        CORRAL_NO_UNIQUE_ADDR InhabitedType<Xfer> xfer;
        CORRAL_NO_UNIQUE_ADDR Second awaitable;
        CORRAL_NO_UNIQUE_ADDR SanitizedAwaiter<Second&&, SecondAwaiter> awaiter;

        explicit SecondStageData(SequenceBase* c, InhabitedType<Xfer> xf)
          : xfer(std::forward<InhabitedType<Xfer>>(xf)),
            awaitable(getSecond(c->thenFn_,
                                std::forward<InhabitedType<Xfer>>(xfer))),
            awaiter(std::forward<Second>(awaitable)) {}
    };

    template <Stage NewStage, class... Args> void switchTo(Args&&... args) {
        Stage oldStage = std::exchange(stage_, Stage::None);

        if (oldStage == Stage::SecondStage) {
            secondStage_.~SecondStageData();
        } else if (oldStage == Stage::Exception) {
            exception_.~exception_ptr();
        }

        if constexpr (NewStage == Stage::FirstStage) {
            new (&executor_) Executor*(std::forward<Args>(args)...);
        } else if constexpr (NewStage == Stage::SecondStage) {
            new (&secondStage_) SecondStageData(std::forward<Args>(args)...);
        } else if constexpr (NewStage == Stage::Exception) {
            new (&exception_) std::exception_ptr(std::forward<Args>(args)...);
        }

        stage_ = NewStage;
    }

    SequenceBase(First first, ThenFn thenFn)
      : first_(std::move(first)), thenFn_(std::move(thenFn)) {}

    // A utility constructor, not initializing `firstFn_`; it's up
    // to the derived class's constructor to do `new (&first_)
    // FirstStageData(...)`. Note that SequenceBase's destructor will
    // unconditionally destroy `first_`, so any derived object's constructor
    // must initialize `first_` regardless of whether it's going to use it.
    SequenceBase(std::nullptr_t, ThenFn thenFn) : thenFn_(std::move(thenFn)) {}

    bool inFirstStage() const noexcept { return stage_ == Stage::FirstStage; }
    bool inSecondStage() const noexcept { return stage_ == Stage::SecondStage; }

  protected:
    Handle parent_ = noopHandle();

    union {
        CORRAL_NO_UNIQUE_ADDR FirstStageData first_;
    };

    CORRAL_NO_UNIQUE_ADDR ThenFn thenFn_;

    union {
        Executor* executor_;                  // valid if stage_ == FirstStage
        mutable SecondStageData secondStage_; // valid if stage_ == SecondStage
        std::exception_ptr exception_;        // valid if stage_ == Exception
    };
    mutable Stage stage_ = Stage::None;
};


//
// Sequence
//

template <Awaitable First, class ThenFn>
class Sequence
  : public SequenceBase<First, ThenFn, AwaitableReturnType<First>> {
    using Ret = decltype(std::declval<typename Sequence::SecondAwaiter>()
                                 .await_resume());

    static constexpr uint16_t Cancelling = 1;

  public:
    Sequence(First first, ThenFn thenFn)
      : Sequence::SequenceBase(std::forward<First>(first),
                               std::forward<ThenFn>(thenFn)) {}

    bool await_ready() const noexcept { return false; }

    auto await_early_cancel() noexcept {
        flags_ |= Cancelling;
        return this->first_.awaiter.await_early_cancel();
    }

    void await_suspend(Handle h) {
        CORRAL_TRACE("   ...sequence %p yielding to...", this);
        this->parent_ = h;

        if (this->first_.awaiter.await_ready()) {
            kickOffSecond();
        } else {
            this->resumeFn = +[](CoroutineFrame* frame) {
                auto* self = static_cast<Sequence*>(frame);
                self->kickOffSecond();
            };
            this->first_.awaiter.await_suspend(this->toHandle()).resume();
        }
    }

    bool await_cancel(Handle h) noexcept {
        CORRAL_TRACE("sequence %p (%s stage) cancellation requested", this,
                     this->inFirstStage() ? "first" : "second");
        flags_ |= Cancelling;
        if (this->inFirstStage()) {
            return this->first_.awaiter.await_cancel(this->toHandle());
        } else if (this->inSecondStage()) {
            return this->secondStage_.awaiter.await_cancel(h);
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
        bool ret = (this->stage_ == Sequence::Stage::Exception) ||
                   (this->inSecondStage() &&
                    this->secondStage_.awaiter.await_must_resume());
        if (!ret && this->inSecondStage()) {
            // Destroy the second stage, which will release any resources
            // it might have held
            this->stage_ = Sequence::Stage::None;
            this->secondStage_.~SecondStageData();
        }
        return ret;
    }

    Ret await_resume() {
        ScopeGuard guard([this] {
            // Destroy the second stage and the return value of the first stage
            this->template switchTo<Sequence::Stage::None>();
        });

        if (this->stage_ == Sequence::Stage::Exception) {
            rethrow_exception(this->exception_);
        } else {
            CORRAL_ASSERT(this->inSecondStage());
            if constexpr (std::is_void_v<Ret>) {
                this->secondStage_.awaiter.await_resume();
            } else {
                return this->secondStage_.awaiter.await_resume();
            }
        }
    }

    void await_introspect(auto& c) const noexcept {
        if (this->inFirstStage()) {
            this->first_.awaiter.await_introspect(c);
        } else if (this->inSecondStage()) {
            this->secondStage_.awaiter.await_introspect(c);
        } else {
            c.node("sequence (degenerate)", this);
        }
    }


  private:
    void kickOffSecond() noexcept {
        if ((flags_ & Cancelling) &&
            !this->first_.awaiter.await_must_resume()) //
        {
            CORRAL_TRACE("sequence %p (cancelling) first stage completed, "
                         "confirming cancellation",
                         this);
            this->parent_.resume();
            return;
        }

        CORRAL_TRACE("sequence %p%s first stage completed, continuing with...",
                     this, (flags_ & Cancelling) ? " (cancelling)" : "");
        CORRAL_ASSERT(this->inFirstStage());
        Executor* ex = this->executor_;

#if __cpp_exceptions
        try {
#endif
            this->template switchTo<Sequence::Stage::SecondStage>(
                    this, std::move(this->first_.awaiter).await_resume());

#if __cpp_exceptions
        } catch (...) {
            this->template switchTo<Sequence::Stage::Exception>(
                    current_exception());
            this->parent_.resume();
            return;
        }
#endif

        if (flags_ & Cancelling) {
            if (this->secondStage_.awaiter.await_early_cancel()) {
                this->template switchTo<Sequence::Stage::None>();

                this->parent_.resume();
                return;
            }
        }

        if (this->secondStage_.awaiter.await_ready()) {
            this->parent_.resume();
        } else {
            this->secondStage_.awaiter.await_set_executor(ex);
            this->secondStage_.awaiter.await_suspend(this->parent_).resume();
        }
    }

  private:
    uint16_t flags_ = 0;
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
