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

#include <exception>
#include <tuple>
#include <variant>

#include "../config.h"
#include "Promise.h"
#include "utility.h"

namespace corral::detail {

template <class T, class Self> class TaskAwaitableBase {
  public:
    TaskAwaitableBase() : promise_(nullptr) {}
    explicit TaskAwaitableBase(Promise<T>* promise) : promise_(promise) {}

    void await_set_executor(Executor* ex) noexcept {
        if constexpr (std::is_same_v<T, void>) {
            if (promise_ == detail::noopPromise()) [[unlikely]] {
                return;
            }
        }
        promise_->setExecutor(ex);
    }

    bool await_early_cancel() noexcept {
        if constexpr (std::is_same_v<T, void>) {
            if (promise_ == detail::noopPromise()) [[unlikely]] {
                return true;
            }
        }
        promise_->cancel();
        return false;
    }

    bool await_ready() const noexcept {
        if constexpr (std::is_same_v<T, void>) {
            if (promise_ == detail::noopPromise()) [[unlikely]] {
                return true;
            }
        }
        return false;
    }

    Handle await_suspend(Handle h) {
        CORRAL_TRACE("    ...pr %p", promise_);
        if constexpr (std::is_same_v<T, void>) {
            if (promise_ == detail::noopPromise()) [[unlikely]] {
                return h;
            }
        }
        CORRAL_ASSERT(promise_);
        continuation_ = h;
        return promise_->start(static_cast<Self*>(this), h);
    }

    bool await_cancel(Handle) noexcept {
        if constexpr (std::is_same_v<T, void>) {
            CORRAL_ASSERT(promise_ != detail::noopPromise());
        }
        if (promise_) {
            promise_->cancel();
        } else {
            // If promise_ is null, then continuation() was called,
            // so we're about to be resumed, so the cancel will fail.
        }
        return false;
    }

    void await_introspect(detail::TaskTreeCollector& c) const noexcept {
        if (!promise_) {
            c.node("<completed task>");
            return;
        }
        if constexpr (std::is_same_v<T, void>) {
            if (promise_ == detail::noopPromise()) [[unlikely]] {
                c.node("<noop>");
                return;
            }
        }
        promise_->await_introspect(c);
    }

  protected:
    Handle continuation() noexcept {
        promise_ = nullptr;
        return continuation_;
    }

  private:
    Promise<T>* promise_;
    Handle continuation_;
};

template <class T> class TaskResultStorage : public TaskParent<T> {
  public:
    T await_resume() && {
        if (value_.index() == Value) {
            return Storage<T>::unwrap(std::get<Value>(std::move(value_)));
        } else {
            CORRAL_ASSERT(value_.index() == Exception &&
                          "co_await on a null or cancelled task");
            std::rethrow_exception(std::get<Exception>(value_));
        }
    }

    bool await_must_resume() const noexcept {
        return value_.index() != Cancelled;
    }

  protected:
    void onTaskDone() {
        CORRAL_ASSERT(value_.index() != Incomplete &&
                      "task exited without co_return'ing a result");
    }

  private:
    void storeValue(T t) override {
        value_.template emplace<Value>(Storage<T>::wrap(std::forward<T>(t)));
    }
    void storeException() override {
        value_.template emplace<Exception>(std::current_exception());
    }
    void cancelled() override { value_.template emplace<Cancelled>(); }

  private:
    std::variant<std::monostate,
                 typename Storage<T>::Type,
                 std::exception_ptr,
                 std::monostate>
            value_;

    // Indexes of types stored in variant
    static constexpr const size_t Incomplete = 0;
    static constexpr const size_t Value = 1;
    static constexpr const size_t Exception = 2;
    static constexpr const size_t Cancelled = 3;
};

template <> class TaskResultStorage<void> : public TaskParent<void> {
  public:
    void await_resume() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }

    bool await_must_resume() const noexcept { return completed_; }

  protected:
    void onTaskDone() {}

  private:
    void storeSuccess() override { completed_ = true; }
    void storeException() override {
        completed_ = true;
        exception_ = std::current_exception();
    }

  private:
    std::exception_ptr exception_;
    bool completed_ = false;
};

/// An awaitable returned by `Task::operator co_await()`.
/// co_await'ing on it runs the task, suspending the parent until it
/// completes.
template <class T>
class TaskAwaitable final : public TaskResultStorage<T>,
                            public TaskAwaitableBase<T, TaskAwaitable<T>> {
    using Base = TaskAwaitableBase<T, TaskAwaitable<T>>;
    using Storage = TaskResultStorage<T>;

  public:
    TaskAwaitable() = default;
    explicit TaskAwaitable(Promise<T>* promise) : Base(promise) {}

  private:
    Handle continuation(BasePromise*) noexcept override {
        Storage::onTaskDone();
        return Base::continuation();
    }
};

template <class, class, class...> class TryBlock;

} // namespace corral::detail
