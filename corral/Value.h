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

    template <class Fn> class Comparison;

  public:
    Value() = default;
    explicit Value(T value) : value_(std::move(value)) {}

    Value(Value&&) = delete;
    Value& operator=(Value&&) = delete;

    const T& get() const noexcept { return value_; }
    operator const T&() const noexcept { return value_; }

    void set(T value);
    Value& operator=(T value);

    /// Runs `fn` on a stored value (which can modify it in-place),
    /// then wakes up awaiters as appropriate.
    ///
    /// Returns the modified value (which may be different from the stored
    /// one if any immediately resumed awaiters modified it further).
    T modify(std::invocable<T&> auto&& fn);


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

    // Shorthands for comparison operations.
    // Each of these yields an object which is convertible to bool,
    // but also can yield an awaitable through a friend `until()` function:
    //
    //     corral::Value<int> v;
    //     bool b = (v >= 42);  // works
    //     co_await until(v >= 42);  // also works
    //
    // Note that unlike `untilMatches()` above, such awaitables do not yield
    // the value which triggered the resumption.
#define CORRAL_DEFINE_COMPARISON_OP(op)                                        \
    template <class U>                                                         \
        requires(requires(const T t, const U u) {                              \
            { t op u } -> std::convertible_to<bool>;                           \
        })                                                                     \
    auto operator op(U&& u) {                                                  \
        return makeComparison([u](const T& t) { return t op u; });             \
    }                                                                          \
                                                                               \
    template <class U>                                                         \
        requires(requires(const T t, const U u) {                              \
            { t op u } -> std::convertible_to<bool>;                           \
        })                                                                     \
    bool operator op(U&& u) const {                                            \
        return value_ op std::forward<U>(u);                                   \
    }

    CORRAL_DEFINE_COMPARISON_OP(==)
    CORRAL_DEFINE_COMPARISON_OP(!=)
    CORRAL_DEFINE_COMPARISON_OP(<)
    CORRAL_DEFINE_COMPARISON_OP(<=)
    CORRAL_DEFINE_COMPARISON_OP(>)
    CORRAL_DEFINE_COMPARISON_OP(>=)
#undef CORRAL_DEFINE_COMPARISON_OP

    template <class U>
        requires(requires(const T t, const U u) { t <=> u; })
    auto operator<=>(U&& rhs) const {
        return value_ <=> std::forward<U>(rhs);
    }

    auto operator!()
        requires(requires(const T t) { !t; })
    {
        return makeComparison([](const T& t) { return !t; });
    }

    bool operator!() const
        requires(requires(const T t) { !t; })
    {
        return !value_;
    }

    /// An alias for `!!value`, for those who prefer more explicit naming.
    auto isTrue()
        requires(requires(const T t) { bool(t); })
    {
        return makeComparison([](const T& t) { return bool(t); });
    }
    bool isTrue() const
        requires(requires(const T t) { bool(t); })
    {
        return bool(value_);
    }

    explicit operator bool() const
        requires(requires(const T t) { bool(t); })
    {
        return bool(value_);
    }

    //
    // Shorthands proxying arithmetic operations to the stored value.
    //

    T operator++()
        requires(requires(T t) { ++t; })
    {
        return modify([](T& v) { ++v; });
    }

    T operator++(int)
        requires(requires(T t) { t++; })
    {
        auto ret = value_;
        modify([](T& v) { ++v; });
        return ret;
    }

    T operator--()
        requires(requires(T t) { --t; })
    {
        return modify([](T& v) { --v; });
    }

    T operator--(int)
        requires(requires(T t) { t--; })
    {
        auto ret = value_;
        modify([](T& v) { --v; });
        return ret;
    }

#define CORRAL_DEFINE_ARITHMETIC_OP(op)                                        \
    template <class U>                                                         \
    T operator op(U&& rhs)                                                     \
        requires(requires(T t, U u) { t op u; })                               \
    {                                                                          \
        return modify([&rhs](T& v) { v op std::forward<U>(rhs); });            \
    }

    CORRAL_DEFINE_ARITHMETIC_OP(+=)
    CORRAL_DEFINE_ARITHMETIC_OP(-=)
    CORRAL_DEFINE_ARITHMETIC_OP(*=)
    CORRAL_DEFINE_ARITHMETIC_OP(/=)
    CORRAL_DEFINE_ARITHMETIC_OP(%=)
    CORRAL_DEFINE_ARITHMETIC_OP(&=)
    CORRAL_DEFINE_ARITHMETIC_OP(|=)
    CORRAL_DEFINE_ARITHMETIC_OP(^=)
    CORRAL_DEFINE_ARITHMETIC_OP(<<=)
    CORRAL_DEFINE_ARITHMETIC_OP(>>=)

#undef CORRAL_DEFINE_ARITHMETIC_OP

  private:
    template <class Fn> Comparison<Fn> makeComparison(Fn&& fn) {
        // gcc-14 fails to CTAD Comparison signature here,
        // so wrap its construction into a helper function.
        return Comparison<Fn>(*this, std::forward<Fn>(fn));
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

    const T& value() const noexcept { return cond_.value_; }

  private:
    virtual bool matches(const T& from, const T& to) = 0;

    void onChanged(const T& from, const T& to) {
        if (matches(from, to)) {
            handle_.resume();
        } else {
            park();
        }
    }

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
    bool matches(const T& /*from*/, const T& to) override {
        if (fn_(to)) {
            result_ = to;
            return true;
        } else {
            return false;
        }
    }

  private:
    CORRAL_NO_UNIQUE_ADDR Fn fn_;
    std::optional<T> result_;
};

template <class T> template <class Fn> class Value<T>::Comparison {
    class Awaiter : public AwaiterBase {
      public:
        Awaiter(Value& cond, Fn fn) : AwaiterBase(cond), fn_(std::move(fn)) {}
        bool await_ready() const noexcept { return fn_(this->value()); }
        void await_resume() {}

      private:
        bool matches(const T& /*from*/, const T& to) override {
            return fn_(to);
        }

      private:
        Fn fn_;
    };

  public:
    Comparison(Value& cond, Fn fn) : cond_(cond), fn_(std::move(fn)) {}

    operator bool() const noexcept { return fn_(cond_.value_); }

    friend Awaiter until(Comparison&& self) {
        return Awaiter(self.cond_, std::move(self.fn_));
    }

    auto operator!() {
        return cond_.makeComparison([fn = fn_](const T& v) { return !fn(v); });
    }

  private:
    Value& cond_;
    Fn fn_;
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
    bool matches(const T& from, const T& to) override {
        if (fn_(from, to)) {
            result_ = std::make_pair(from, to);
            return true;
        } else {
            return false;
        }
    }

  private:
    CORRAL_NO_UNIQUE_ADDR Fn fn_;
    std::optional<std::pair<T, T>> result_;
};


template <class T> T Value<T>::modify(std::invocable<T&> auto&& fn) {
    T prev = value_;
    std::forward<decltype(fn)>(fn)(value_);
    T value = value_;
    auto parked = std::move(parked_);
    while (!parked.empty()) {
        auto& p = parked.front();
        parked.pop_front();

        // Note: not using `value_` here; if `set()` is called outside
        // of a task, then `onChanged()` will immediately resume the
        // awaiting tasks, which could cause `value_` to change further.
        p.onChanged(prev, value);
    }
    return value;
}

template <class T> void Value<T>::set(T value) {
    modify([&](T& v) { v = std::move(value); });
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
