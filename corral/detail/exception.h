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

#include "../Task.h"
#include "../concepts.h"
#include "../config.h"
#include "Promise.h"
#include "frames.h"
#include "utility.h"

namespace corral::detail {

struct RethrowCurrentException;

class TryBlockBase : public TaskParent<void> {
  protected:
    std::exception_ptr exception_;
    friend RethrowCurrentException;
};

struct Ellipsis {};

template <class Try, class Finally, class... Catch>
class TryBlock final : public TryBlockBase, public NurseryScopeBase {
    enum class Stage { TRY, CATCH, FINALLY };

  public:
    TryBlock(Try try_, Finally finally, std::tuple<Catch...> catch_)
      : catchLambdas_(std::move(catch_)),
        finallyLambda_(std::forward<Finally>(finally)) {
        new (&tryLambda_) Try(std::forward<Try>(try_));
    }

    TryBlock(TryBlock&&) = default;

    ~TryBlock() { CORRAL_ASSERT(!task_); }

    void await_set_executor(Executor* ex) noexcept { executor_ = ex; }
    bool await_ready() const noexcept { return false; }

    bool await_early_cancel() noexcept {
        earlyCancel_ = true;
        return false;
    }

    Handle await_suspend(Handle h) {
        parent_ = h;
        Handle ret = invoke(tryLambda_);
        if (stage_ != Stage::FINALLY && task_ && earlyCancel_) {
            task_->cancel();
        }
        return ret;
    }

    bool await_cancel(Handle) noexcept {
        if (stage_ != Stage::FINALLY && task_) {
            task_->cancel();
        } else {
            // Finally-block is not cancellable and must run to completion
        }
        return false;
    }

    bool await_must_resume() const noexcept {
        return completed_ || exception_ != nullptr;
    }

    void await_resume() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    void await_introspect(detail::TaskTreeCollector& c) const noexcept {
        if (task_) {
            task_->await_introspect(c);
        }
    }

  private:
    template <class Lambda, class... Args>
    Handle invoke(const Lambda& lambda, Args&&... args) {
        if constexpr (std::is_same_v<Lambda, Void>) {
            CORRAL_TRACE("   ...try-block %p, (%s; no-op)", this,
                         stageName(stage_));
            storeSuccess();
            return continuation(nullptr);
        } else {
            try {
                task_.reset(lambda(std::forward<Args>(args)...).release());
                CORRAL_TRACE("   ...try-block %p (%s; pr = %p)", this,
                             stageName(stage_), task_.get());
                task_->setExecutor(executor_);
                return task_->start(this, parent_);
            } catch (...) {
                CORRAL_TRACE("   ...try-block %p, (%s; early-failed)", this,
                             stageName(stage_));
                storeException();
                return continuation(nullptr);
            }
        }
    }

    void storeSuccess() override {
        if (stage_ == Stage::TRY || stage_ == Stage::CATCH) {
            completed_ = true;
            exception_ = nullptr;
        }
    }

    void storeException() noexcept override {
        if (stage_ == Stage::FINALLY && exception_) {
            std::terminate(); // multiple exceptions in flight
        } else {
            exception_ = std::current_exception();
        }
    }

    Handle continuation(BasePromise* p) noexcept override {
        CORRAL_TRACE("Try-block %p (stage %s, pr %p) %s, %s.", this,
                     stageName(stage_), p,
                     exception_ ? "raised an exception" : "exited normally",
                     stage_ != Stage::FINALLY ? "continuing with.."
                                              : "resuming parent");

        CORRAL_ASSERT(p == task_.get());
        task_.reset();

        if (stage_ == Stage::TRY) {
            tryLambda_.~Try();
            if (exception_) {
                stage_ = Stage::CATCH;
                return dispatchException();
            } else {
                stage_ = Stage::FINALLY;
                return invoke(finallyLambda_);
            }

        } else if (stage_ == Stage::CATCH) {
            stage_ = Stage::FINALLY;
            return invoke(finallyLambda_);

        } else if (stage_ == Stage::FINALLY) {
            return std::exchange(parent_, noopHandle());

        } else {
            CORRAL_ASSERT_UNREACHABLE();
        }
    }

    template <size_t I = 0> Handle dispatchException() {
        if constexpr (I == sizeof...(Catch)) {
            return continuation(nullptr);
        } else {
            using Handler = std::tuple_element_t<I, std::tuple<Catch...>>;
            using Exception =
                    typename CallableSignature<Handler>::template Arg<0>;
            if constexpr (std::is_same_v<Exception, Ellipsis>) {
                return invoke(std::get<I>(catchLambdas_), Ellipsis{});
            } else {
                try {
                    std::rethrow_exception(exception_);
                } catch (Exception ex) {
                    return invoke(std::get<I>(catchLambdas_),
                                  std::forward<Exception>(ex));
                } catch (...) { return dispatchException<I + 1>(); }
            }
        }
    }

    static const char* stageName(Stage s) {
        if (s == Stage::TRY) {
            return "try";
        } else if (s == Stage::CATCH) {
            return "catch";
        } else if (s == Stage::FINALLY) {
            return "finally";
        } else {
            return "unknown";
        }
    }

  private:
    Stage stage_ = Stage::TRY;
    bool earlyCancel_ = false;
    bool completed_ = false;
    PromisePtr<void> task_;

    union {
        // In scope iff stage_ == Stage::TRY
        [[no_unique_address]] Try tryLambda_;
    };
    [[no_unique_address]] std::tuple<Catch...> catchLambdas_;
    [[no_unique_address]] Finally finallyLambda_;

    Handle parent_;
    Executor* executor_ = nullptr;
};

/// A factory for creating a `try-finally` block through
/// `co_await corral::try_(...).finally(...)` syntax.
template <class Try, class... Catch> class TryBlockBuilder {
    [[no_unique_address]] Try m_try;
    [[no_unique_address]] std::tuple<Catch...> m_catch;

  public:
    explicit TryBlockBuilder(Try&& t, std::tuple<Catch...> c)
      : m_try(std::forward<Try>(t)), m_catch(std::move(c)) {}

    template <class C> auto catch_(C&& c) {
        using Sig = CallableSignature<C>;
        static_assert(Sig::Arity == 1 &&
                              std::is_same_v<typename Sig::Ret, Task<void>>,
                      "catch_() argument lambda must conform to "
                      "`Task<void>(const Ex&)` or `Task<void>(Ellipsis)`");
        return TryBlockBuilder<Try, Catch..., C>(
                std::forward<Try>(m_try),
                std::tuple_cat(std::move(m_catch),
                               std::forward_as_tuple(std::forward<C>(c))));
    }

    template <class Finally>
        requires(std::is_invocable_r_v<Task<void>, Finally>)
    auto finally(Finally&& finally) {
        return TryBlock<Try, Finally, Catch...>(std::forward<Try>(m_try),
                                                std::forward<Finally>(finally),
                                                std::move(m_catch));
    }

    auto operator co_await() {
        static_assert(sizeof...(Catch) != 0,
                      "Try-block without any catch- or finally-block?");
        return TryBlock<Try, Void, Catch...>(std::forward<Try>(m_try), Void{},
                                             std::move(m_catch));
    }
};

/// A factory for creating a `try-catch-finally` block through
/// `CORRAL_TRY { ... } CORRAL_CATCH(Ex&) { ... }  CORRAL_FINALLY { ... };`
/// syntax.
class TryBlockMacroFactory {
    template <class Try, class... Catch>
    class Builder : public NurseryScopeBase {
        Try try_;
        std::tuple<Catch...> catch_;

      public:
        Builder(Try t, std::tuple<Catch...> c)
          : try_(std::forward<Try>(t)), catch_(std::move(c)) {}

        template <class C> auto operator/(C&& c) {
            using Sig = CallableSignature<C>;
            static_assert(Sig::Arity == 1 &&
                                  std::is_same_v<typename Sig::Ret, Task<void>>,
                          "CORRAL_CATCH() argument must be a single exception "
                          "type or `corral::Ellipsis`");

            return Builder<Try, Catch..., C>(
                    std::forward<Try>(try_),
                    std::tuple_cat(std::move(catch_),
                                   std::forward_as_tuple(std::forward<C>(c))));
        }

        template <class Finally>
            requires(std::is_invocable_r_v<Task<void>, Finally>)
        auto operator%(Finally&& finally) {
            return TryBlock<Try, Finally, Catch...>(
                    std::forward<Try>(try_), std::forward<Finally>(finally),
                    std::move(catch_));
        }

        auto operator co_await() {
            static_assert(sizeof...(Catch) != 0,
                          "Try-block without any catch- or finally-block?");
            return TryBlock<Try, Void, Catch...>(std::forward<Try>(try_),
                                                 Void{}, std::move(catch_));
        }
    };

  public:
    template <class Try>
        requires(std::is_invocable_r_v<Task<void>, Try>)
    auto operator%(Try&& try_) {
        return Builder<Try>(std::forward<Try>(try_), std::make_tuple());
    }
};


struct RethrowCurrentException {
    struct Awaitable {
        bool await_ready() const noexcept { return false; }
        bool await_early_cancel() noexcept { return false; }
        bool await_must_resume() const noexcept { return true; }

        bool await_suspend(Handle h) {
            auto f = CoroutineFrame::fromHandle(h);
            while (f) {
                if (TaskFrame* task = frameCast<TaskFrame>(f)) {
                    BasePromise* p = static_cast<BasePromise*>(task);
                    auto parent = dynamic_cast<TryBlockBase*>(p->parent_);
                    if (parent && parent->exception_) {
                        exception_ = parent->exception_;
                        break;
                    }
                }

                if (ProxyFrame* proxy = detail::frameCast<ProxyFrame>(f)) {
                    f = CoroutineFrame::fromHandle(proxy->followLink());
                } else {
                    std::terminate();
                }
            }
            return false;
        }

        void await_resume() { std::rethrow_exception(exception_); }

      private:
        std::exception_ptr exception_;
    };

    auto operator co_await() const noexcept { return Awaitable{}; }

    constexpr RethrowCurrentException() = default;
};

} // namespace corral::detail
