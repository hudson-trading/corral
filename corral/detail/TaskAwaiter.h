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

#include <exception>
#include <tuple>
#include <variant>

#include "../config.h"
#include "Promise.h"
#include "utility.h"

namespace corral::detail {

/// An awaiter returned by `Task::operator co_await()`.
/// co_await'ing on it runs the task, suspending the parent until it
/// completes.
template <class T>
class TaskAwaiter final : public TaskParent<T>, private Noncopyable {
  public:
    TaskAwaiter() noexcept : promise_(nullptr) {}
    explicit TaskAwaiter(Promise<T>* promise) noexcept : promise_(promise) {}

    void await_set_executor(Executor* ex) noexcept {
        promise_->setExecutor(ex);
    }

    bool await_early_cancel() noexcept {
        promise_->cancel();
        return false;
    }

    bool await_ready() const noexcept {
        return promise_->checkImmediateResult(
                const_cast<TaskAwaiter<T>*>(this));
    }

    Handle await_suspend(Handle h) {
        CORRAL_TRACE("    ...pr %p", promise_);
        CORRAL_ASSERT(promise_);
        continuation_ = h;
        return promise_->start(this, h);
    }

    bool await_cancel(Handle) noexcept {
        if (promise_) {
            promise_->cancel();
        } else {
            // If promise_ is null, then continuation() was called,
            // so we're about to be resumed, so the cancel will fail.
        }
        return false;
    }

    T await_resume() && { return std::move(result_).value(); }

    bool await_must_resume() const noexcept { return !result_.wasCancelled(); }

    void await_introspect(detail::TaskTreeCollector& c) const noexcept {
        if (!promise_) {
            c.node("<completed task>");
            return;
        }
        promise_->await_introspect(c);
    }

  private:
    // TaskParent implementation
    void storeValue(InhabitedType<T> t) override {
        result_.storeValue(std::forward<InhabitedType<T>>(t));
    }
    void storeException() override { result_.storeException(); }
    void cancelled() override { result_.markCancelled(); }

    Handle continuation(BasePromise*) noexcept override {
        CORRAL_ASSERT(result_.completed() &&
                      "task exited without co_return'ing a result");
        promise_ = nullptr;
        return continuation_;
    }

  private:
    Promise<T>* promise_;
    Result<T> result_;
    Handle continuation_;
};

template <class, class, class...> class TryBlock;

} // namespace corral::detail
