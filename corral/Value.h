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
#include "defs.h"
#include "detail/IntrusiveList.h"

namespace corral {

/// A variable that can wake tasks when its value changes.
///
/// Allows suspending a task until the value of the variable, or a transition
/// thereof, satisfies a predicate.
template <class T> class Value {
    class AwaiterBase;
    template <class Fn> class UntilMatches;
    template <class Fn> class UntilChanged;

  public:
    Value() = default;
    explicit Value(T value) : value_(std::move(value)) {}

    Value(Value&&) = delete;
    Value& operator=(Value&&) = delete;

    const T& get() const noexcept { return value_; }
    operator const T&() const noexcept { return value_; }

    void set(T value);
    Value& operator=(T value);


    /// Suspends the caller until the stored value matches the predicate.
    /// (or resumes it immediately if it already does).
    ///
    /// Yielded value will be match the predicate, even though the value
    /// stored in the class may have changed since the caller was scheduled
    /// to resume.
    template <std::invocable<const T&> Fn>
    Awaitable<T> auto untilMatches(Fn&& predicate);

    /// Same as above, but accepting an expected value instead.
    Awaitable<T> auto untilEquals(T expected) {
        return untilMatches([expected = std::move(expected)](const T& value) {
            return value == expected;
        });
    }


    /// Suspends the caller until the transition of the stored value
    /// matches the predicate.
    ///
    /// The predicate will be tested on further each assignment, including
    /// an assignment of an already stored value.
    ///
    /// Yields a pair of the previous and the current value that matched
    /// the predicate, even though the value stored in the class may have
    /// changed since the caller was scheduled to resume.
    template <std::invocable<const T& /*from*/, const T& /*to*/> Fn>
    Awaitable<std::pair<T, T>> auto untilChanged(Fn&& predicate);

    /// Shorthands to the above.
    Awaitable<std::pair<T, T>> auto untilChanged() {
        return untilChanged(
                [](const T& from, const T& to) { return from != to; });
    }
    Awaitable<std::pair<T, T>> auto untilChanged(T from, T to) {
        return untilChanged([from = std::move(from),
                             to = std::move(to)](const T& f, const T& t) {
            return f == from && t == to;
        });
    }

  private:
    T value_;
    detail::IntrusiveList<AwaiterBase> parked_;
};


//
// Implementation
//

template <class T>
class Value<T>::AwaiterBase
  : public detail::IntrusiveListItem<Value<T>::AwaiterBase> {
  public:
    void await_suspend(Handle h) {
        handle_ = h;
        cond_.parked_.push_back(*this);
    }

    auto await_cancel(Handle) noexcept {
        this->unlink();
        return std::true_type{};
    }

  protected:
    explicit AwaiterBase(Value& cond) : cond_(cond) {}

    void park() { cond_.parked_.push_back(*this); }
    void doResume() { handle_.resume(); }

    const T& value() const noexcept { return cond_.value_; }

  private:
    virtual void onChanged(const T& from, const T& to) = 0;

  private:
    Value<T>& cond_;
    Handle handle_;
    friend Value<T>;
};


template <class T>
template <class Fn>
class Value<T>::UntilMatches : public AwaiterBase {
  public:
    UntilMatches(Value& cond, Fn fn) : AwaiterBase(cond), fn_(std::move(fn)) {
        if (fn_(cond.value_)) {
            result_ = cond.value_;
        }
    }

    bool await_ready() const noexcept { return result_.has_value(); }
    T await_resume() && { return std::move(*result_); }

  private:
    void onChanged(const T& from, const T& to) override {
        if (fn_(to)) {
            result_ = to;
            this->doResume();
        } else {
            this->park();
        }
    }

  private:
    CORRAL_NO_UNIQUE_ADDR Fn fn_;
    std::optional<T> result_;
};


template <class T>
template <class Fn>
class Value<T>::UntilChanged : public AwaiterBase {
  public:
    explicit UntilChanged(Value& cond, Fn fn)
      : AwaiterBase(cond), fn_(std::move(fn)) {}

    bool await_ready() const noexcept { return false; }
    std::pair<T, T> await_resume() && { return std::move(*result_); }

  private:
    void onChanged(const T& from, const T& to) override {
        if (fn_(from, to)) {
            result_ = std::make_pair(from, to);
            this->doResume();
        } else {
            this->park();
        }
    }

  private:
    CORRAL_NO_UNIQUE_ADDR Fn fn_;
    std::optional<std::pair<T, T>> result_;
};


template <class T> void Value<T>::set(T value) {
    T prev = std::exchange(value_, value);
    auto parked = std::move(parked_);
    while (!parked.empty()) {
        auto& p = parked.front();
        parked.pop_front();

        // Note: not using `value_` here; if `set()` is called outside
        // of a task, then `onChanged()` will immediately resume the
        // awaiting tasks, which could cause `value_` to change further.
        p.onChanged(prev, value);
    }
}

template <class T> Value<T>& Value<T>::operator=(T value) {
    set(std::move(value));
    return *this;
}

template <class T>
template <std::invocable<const T&> Fn>
Awaitable<T> auto Value<T>::untilMatches(Fn&& predicate) {
    return UntilMatches<Fn>(*this, std::forward<Fn>(predicate));
}

template <class T>
template <std::invocable<const T& /*from*/, const T& /*to*/> Fn>
Awaitable<std::pair<T, T>> auto Value<T>::untilChanged(Fn&& predicate) {
    return UntilChanged<Fn>(*this, std::forward<Fn>(predicate));
}

} // namespace corral
