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

// Customization points for adapting corral to your codebase. #define any
// of these macros before the first corral header you include.

// Provide debugging information using a printf-style format string.
#ifndef CORRAL_TRACE
#define CORRAL_TRACE(fmt, ...)
#endif

// Indicate that a boolean condition is expected to always be true. If
// it's false, some assumption has been violated and you might want to
// crash the program to investigate further. Evaluating the condition
// will not have any side-effects and may be skipped in release builds
// where performance is more important than double-checking.
#ifndef CORRAL_ASSERT
#include <cassert>
#define CORRAL_ASSERT(cond) assert(cond)
#endif

// Indicate that execution is expected to never reach this point.
#ifndef CORRAL_ASSERT_UNREACHABLE
#define CORRAL_ASSERT_UNREACHABLE()                                            \
    do {                                                                       \
        CORRAL_ASSERT(!"should never reach here");                             \
        ::corral::detail::unreachable();                                       \
    } while (0)
#endif

// Storage class specifier for thread-local variables; currently there is
// only one, the currently running executor. May be replaced with 'static'
// to save a tiny bit of overhead if your program will run corral tasks in
// at most one thread. Note that even if you use threads, there is no
// supported way to interact with the _same_ corral tasks from multiple
// threads; each thread is expected to be its own parallel universe.
#ifndef CORRAL_THREAD_LOCAL
#define CORRAL_THREAD_LOCAL thread_local
#endif

// Users can provide their own implementation of flat_hash_set,
// if a more efficient version than `std::unordered_set` is available to them.
// (For example, one option would be absl::flat_hash_set.) This is primarily
// used to store the set of tasks running in a nursery. To do so, #define
// CORRAL_FLAT_HASH_SET and provide a corral::detail::flat_hash_set typedef.
#ifndef CORRAL_FLAT_HASH_SET
#define CORRAL_FLAT_HASH_SET
#include <unordered_set>
namespace corral::detail {
template <class T> using flat_hash_set = std::unordered_set<T>;
}
#endif

// Users can provide their own logic to handle the case where an
// UnsafeNursery is destroyed, or a corral::run()'s event loop stops,
// before all tasks have exited. For example, this could
// format the task tree and include it in an exception or assertion
// message.
#ifndef CORRAL_FAIL_FOR_DANGLING_TASKS
#define CORRAL_FAIL_FOR_DANGLING_TASKS(msg, root_awaitable)                    \
    CORRAL_ASSERT(false && msg)
#endif

// Certain earlier versions of mainstream compilers or their STL used
// to provide <coroutine> under a different path (e.g.
// <experimental/coroutine>) and under a different namespace (like
// `std::experimental`). If you need to support this, define
// CORRAL_COROUTINE_HANDLE and provide your own
// corral::detail::CoroutineHandle typedef.
#ifndef CORRAL_COROUTINE_HANDLE
#define CORRAL_COROUTINE_HANDLE
#include <coroutine>
namespace corral::detail {
template <class Promise> using CoroutineHandle = std::coroutine_handle<Promise>;
}
#endif

// You may define CORRAL_AWAITABLE_STATE_DEBUG to enable additional
// runtime checks that the corral is actually interacting with your
// awaitables in a way that respects the documented awaitable state
// machine. This is somewhat expensive and is useful primarily for
// diagnosing bugs in corral itself.
/* #define CORRAL_AWAITABLE_STATE_DEBUG */
