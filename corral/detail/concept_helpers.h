// This file is part of corral, a lightweight C++20 coroutine libr<ary.
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
#include <concepts>
#include <type_traits>
#include <utility>

namespace corral::detail {

struct Unspecified {};

template <class From, class To>
concept convertible_to =
        std::same_as<From, To> || std::convertible_to<From, To>;

template <class From, class... To>
concept convertible_to_any = (convertible_to<From, To> || ...);

template <class From, class... To>
concept same_as_any = (std::same_as<From, To> || ...);

/// Has member field `value` that resolves to `true` if T is a template
/// instantiation of Template, and `false` otherwise.
template <template <typename...> typename Template, typename T>
struct is_specialization_of : std::false_type {};

template <template <typename...> typename Template, typename... Args>
struct is_specialization_of<Template, Template<Args...>> : std::true_type {};

template <template <typename...> typename Template, typename T>
constexpr inline bool is_specialization_of_v =
        is_specialization_of<Template, T>::value;

template <typename T>
constexpr bool is_reference_wrapper_v =
        is_specialization_of_v<std::reference_wrapper, T>;

template <typename T>
constexpr bool is_const_reference_v =
        std::is_reference_v<T> && std::is_const_v<std::remove_reference_t<T>>;

} // namespace corral::detail
