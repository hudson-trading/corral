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

#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)
#define CORRAL_NOINLINE __declspec(noinline)
#define CORRAL_RETURN_ADDRESS() _ReturnAddress()

#elif defined(__GNUC__) || defined(__clang__)
#define CORRAL_NOINLINE __attribute__((noinline))
#define CORRAL_RETURN_ADDRESS() __builtin_return_address(0)

#else
#define CORRAL_NOINLINE
#define CORRAL_RETURN_ADDRESS() nullptr
#endif


// gcc doesn't support maybe_unused attribute on non-static member variables
#if defined(__clang__) || defined(_MSC_VER)
#define CORRAL_UNUSED_MEMBER [[maybe_unused]]
#else
#define CORRAL_UNUSED_MEMBER
#endif


namespace corral::detail {
#if __cpp_exceptions
using std::current_exception;
using std::exception_ptr;
using std::rethrow_exception;
#else
struct exception_ptr {
    constexpr exception_ptr() noexcept = default;
    explicit constexpr operator bool() const noexcept { return false; }
};
[[noreturn]] void unreachable();
exception_ptr current_exception() noexcept {
    unreachable();
}
[[noreturn]] void rethrow_exception(exception_ptr) {
    unreachable();
}
#endif

inline void spinLoopBody() {
#if defined(_MSC_VER)
    ::_mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    __asm__ __volatile__("isb");
#elif defined(__arm__)
    __asm__ __volatile__("yield");
#endif
}

} // namespace corral::detail
