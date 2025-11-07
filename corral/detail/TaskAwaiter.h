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

#include <cstring>
#include <exception>
#include <tuple>
#include <variant>

#include "../config.h"
#include "Promise.h"
#include "utility.h"

namespace corral::detail {


/// A class holding either a value of type T or a Promise<T>.
/// Small trivial types (or references) are stored inline
/// to avoid heap allocation.
///
/// Additionally, the class has two degenerate states: default-constructed
/// and consumed (transitioned into by takeValue()). In any these states
/// it is only allowed to query the object status (is*() methods),
/// or reassign it (having these two states separate allows simplifying
/// AwaitableLambda a little bit).
template <class T> class ValueOrPromise {
    using Value = InhabitedType<T>;
    using Storage = detail::Storage<Value>;
    using Stored = typename Storage::Type;

  public:
    static constexpr const bool FitsInline =
            std::is_trivially_destructible_v<Stored> &&
            std::is_trivially_copyable_v<Stored> &&
            (sizeof(Stored) <= sizeof(uintptr_t));

    static constexpr const int TypeBits = 2;
    static constexpr const uintptr_t Mask = (1 << TypeBits) - 1;

    ValueOrPromise() : bits_(0) {}

    explicit ValueOrPromise(Promise<T>* promise) {
        bits_ = reinterpret_cast<uintptr_t>(promise) | IsPromise;
    }

    explicit ValueOrPromise(Value value) {
        if constexpr (FitsInline) {
            bits_ = 0;
            if constexpr (!std::is_empty_v<Stored>) {
                Stored wrapped = Storage::wrap(std::forward<Value>(value));
                memcpy(&bits_, &wrapped, sizeof(Stored));
            }

            if ((bits_ & Mask) == 0) {
                bits_ |= IsInlineValue;
                return;
            } else if ((bits_ & (Mask << (sizeof(uintptr_t) * 8 - TypeBits))) ==
                       0) {
                bits_ <<= TypeBits;
                bits_ |= IsShiftedInlineValue;
                return;
            }
        }

        // Either does not fit inline, or does not have spare bits
        // we can reuse for the type tag -- fall back to pointer storage.
        bits_ = reinterpret_cast<uintptr_t>(
                        new AlignedStorage(std::forward<Value>(value))) |
                IsValuePtr;
    }

    ValueOrPromise(ValueOrPromise&& rhs) {
        bits_ = std::exchange(rhs.bits_, 0);
    }
    ValueOrPromise& operator=(ValueOrPromise rhs) {
        std::swap(bits_, rhs.bits_);
        return *this;
    }

    ~ValueOrPromise() {
        static_assert(sizeof(ValueOrPromise) == sizeof(uintptr_t));

        auto [type, bits] = take();
        if (bits == 0) {
            return;
        } else if (type == IsPromise) {
            reinterpret_cast<Promise<T>*>(bits)->destroy();
        } else if (type == IsValuePtr) {
            delete reinterpret_cast<AlignedStorage*>(bits);
        } else {
            // inline trivially destructible value -- nothing to do
        }
    }

    bool isEmpty() const noexcept { return bits_ == 0; }
    bool isConsumed() const noexcept { return bits_ == IsValuePtr; }

    bool hasPromise() const noexcept { return (bits_ & Mask) == IsPromise; }
    bool hasValue() const noexcept { return !hasPromise(); }

    Promise<T>* promise() {
        CORRAL_ASSERT((bits_ & Mask) == IsPromise);
        // Note: IsPromise is all-zeros, so we don't have to mask it out.
        return reinterpret_cast<Promise<T>*>(bits_);
    }

    Promise<T>* takePromise() {
        uintptr_t bits = std::exchange(bits_, 0);
        CORRAL_ASSERT(bits != 0 && (bits & Mask) == IsPromise);
        return reinterpret_cast<Promise<T>*>(bits & ~Mask);
    }

    Value takeValue() && {
        auto [type, bits] = take();

        if (type == IsValuePtr) {
            CORRAL_ASSERT(bits && "value already taken");
            std::unique_ptr<AlignedStorage> p(
                    reinterpret_cast<AlignedStorage*>(bits));
            return std::move(*p).get();
        } else if constexpr (FitsInline) {
            CORRAL_ASSERT(type == IsInlineValue ||
                          type == IsShiftedInlineValue);
            if (type == IsShiftedInlineValue) {
                bits >>= TypeBits;
            }
            return Storage::unwrap(
                    std::move(*reinterpret_cast<Stored*>(&bits)));
        } else {
            CORRAL_ASSERT_UNREACHABLE();
        }
    }

  private:
    std::pair<int /*type*/, uint64_t /*bits*/> take() {
        /// A special value to tell between a default-constructed
        /// ValueOrPromise and the one that has already had its value taken.
        static constexpr const uintptr_t Consumed = 0 | IsValuePtr;
        uintptr_t bits = std::exchange(bits_, Consumed);
        return {bits & Mask, bits & ~Mask};
    }

  private:
    static_assert(alignof(Promise<T>) > Mask);

    static constexpr const int StorageAlignment =
            (std::max) (alignof(Promise<T>), alignof(Stored));
    class alignas(StorageAlignment) AlignedStorage {
        Stored value_;

      public:
        explicit AlignedStorage(Value v)
          : value_(Storage::wrap(std::forward<Value>(v))) {}
        Value get() && { return Storage::unwrap(std::move(value_)); }
    };

    // Lower bits of `bits_` are used to distinguish the stored type:
    enum {
        // A regular promise pointer
        IsPromise = 0,

        // A pointer to AlignedStorage<T> holding a ready value
        IsValuePtr = 1,

        // An inline value of type T. Only used if:
        //    - T is small enough to fit into one uintptr_t;
        //    - T is trivially copyable and destructible;
        //    - the value happens to have its lower bits set to zero
        //      (or is an empty type, like Void or std::monostate).
        IsInlineValue = 2,

        // An inline value of type T, shifted left by TypeBits.
        // Used in cases similar to IsInlineValue, but where the lower bits
        // of the value are not zero (but upper bits are) --
        // like for smaller ints or bools.
        IsShiftedInlineValue = 3,
    };

    uintptr_t bits_;
};


/// An awaiter returned by `Task::operator co_await()`.
/// co_await'ing on it runs the task, suspending the parent until it
/// completes.
template <class T>
class TaskAwaiter final : public TaskParent<T>, private Noncopyable {
  public:
    TaskAwaiter() noexcept : promise_(nullptr) {}

    explicit TaskAwaiter(ValueOrPromise<T>&& v) noexcept {
        // Note: this consumes `v` if it holds a value, but not if it holds
        // a promise (which continues to be owned by Task).
        if (v.hasPromise()) {
            promise_ = v.promise();
        } else {
            promise_ = nullptr;
            result_.storeValue(std::move(v).takeValue());
        }
    }

    void await_set_executor(Executor* ex) noexcept {
        promise_->setExecutor(ex);
    }

    bool await_early_cancel() noexcept {
        if (promise_) {
            promise_->cancel();
        }
        return false;
    }

    bool await_ready() const noexcept { return result_.completed(); }

    Handle await_suspend(Handle h) {
        CORRAL_TRACE("    ...pr %p", promise_);
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
