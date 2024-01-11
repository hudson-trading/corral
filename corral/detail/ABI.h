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
#include "../defs.h"

namespace corral::detail {

// A common header of any coroutine frame, used by all major compilers
// (gcc, clang, msvc-16.8+).
//
// See https://devblogs.microsoft.com/oldnewthing/20211007-00/?p=105777
// for details.
//
// await_suspend() receives a std::coroutine_handle for indicating
// completion of the awaited operation, but in many cases corral wants
// to be able to intercept that completion without creating another
// full coroutine frame (due to the expense, heap-allocation, etc
// thereof). This is achieved by filling out a CoroutineFrame
// subobject with function pointers of our choosing, and synthesizing
// a std::coroutine_handle that points to it. See detail/frames.h for
// more details.
struct CoroutineFrame {
    /// std::coroutine_handle<>::resume() is effectively a call to this function
    /// pointer. For ProxyFrames, this is used as a callback.
    void (*resumeFn)(CoroutineFrame*) = nullptr;

    /// Likewise, std::coroutine_handle<>::destroy calls this. For ProxyFrames
    /// (where only resume() is used), destroyFn is repurposed as a pointer to
    /// the coroutine frame for the parent task.
    void (*destroyFn)(CoroutineFrame*) = nullptr;

    static CoroutineFrame* fromHandle(Handle h) {
        return reinterpret_cast<CoroutineFrame*>(h.address());
    }

    Handle toHandle() & { return Handle::from_address(this); }
};

} // namespace corral::detail
