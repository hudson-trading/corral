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
#include <optional>
#include <type_traits>
#include <utility>

namespace corral {
namespace detail {

/// A wrapper wrapping a pointer to a std::optional<Ref>-like interface.
template <class T>
    requires(std::is_reference_v<T>)
class OptionalRef {
    using Pointee = std::remove_reference_t<T>;

  public:
    OptionalRef() noexcept : ptr_(nullptr) {}
    OptionalRef(T value) noexcept : ptr_(&value) {}
    OptionalRef(std::nullopt_t) noexcept : ptr_(nullptr) {}

    bool has_value() const noexcept { return ptr_ != nullptr; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }

    Pointee& operator*() noexcept { return *ptr_; }
    const Pointee& operator*() const noexcept { return *ptr_; }

    Pointee* operator->() noexcept { return ptr_; }
    const Pointee* operator->() const noexcept { return ptr_; }

    Pointee& value() { return ref(); }
    const Pointee& value() const { return ref(); }

    template <class U> Pointee value_or(U&& def) const {
        return has_value() ? *ptr_ : static_cast<Pointee>(std::forward<U>(def));
    }

    void reset() noexcept { ptr_ = nullptr; }
    void swap(OptionalRef& other) noexcept { std::swap(ptr_, other.ptr_); }

  private:
    Pointee& ref() const {
        if (has_value()) {
            return *ptr_;
        } else {
#if __cpp_exceptions
            throw std::bad_optional_access();
#else
            std::terminate();
#endif
        }
    }

  private:
    Pointee* ptr_;
};


class OptionalVoid {
  public:
    OptionalVoid() : value_(false) {}
    explicit OptionalVoid(std::in_place_t) : value_(true) {}
    explicit(false) OptionalVoid(std::nullopt_t) : value_(false) {}

    bool has_value() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_; }

    void value() const {
        if (!value_) {
#if __cpp_exceptions
            throw std::bad_optional_access();
#else
            std::terminate();
#endif
        }
    }
    void operator*() const {}

    void reset() noexcept { value_ = false; }
    void swap(OptionalVoid& other) noexcept { std::swap(value_, other.value_); }

  private:
    bool value_;
};


template <class T> struct MakeOptional {
    using Type = std::optional<T>;
};
template <class T>
    requires std::is_reference_v<T>
struct MakeOptional<T> {
    using Type = OptionalRef<T>;
};
template <class T>
    requires std::is_void_v<T> // also matches cv-qualified void
struct MakeOptional<T> {
    using Type = OptionalVoid;
};

} // namespace detail

template <class T> using Optional = typename detail::MakeOptional<T>::Type;

} // namespace corral
