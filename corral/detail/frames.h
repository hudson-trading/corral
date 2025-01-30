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
#include <cstdint>

#include "../defs.h"
#include "ABI.h"

namespace corral::detail {
namespace frame_tags {

static_assert(alignof(CoroutineFrame) >= 4);

#if defined(__x86_64__) || defined(__aarch64__) || defined(_WIN64)

// Allows distinguishing between frames constructed by the library (PROXY
// frames) vs. real coroutine frames constructed by the compiler.
//
// We are free to use two MSB's of a pointer, which are unused on all modern
// 64-bit architectures.
//
// https://en.wikipedia.org/wiki/Intel_5-level_paging
constexpr uintptr_t PROXY = 1ull << 63;

// See TaskFrame.
constexpr uintptr_t TASK = 1ull << 62;

constexpr bool HaveSpareBitsInPC = true;

#elif defined(__arm__) && !defined(__thumb__)
// ARM processors have several instruction sets. The most-condensed instruction
// set is called "Thumb" and has 16-bit long instructions:
//
// https://stackoverflow.com/questions/10638130/what-is-the-arm-thumb-instruction-set
//
// These instructions are 2-byte aligned. Thus, it would seem like we have 1 LSB
// available to distinguish between PROXY frames and real coroutine frames.
//
// However, we do NOT:
//
// https://stackoverflow.com/questions/27084857/what-does-bx-lr-do-in-arm-assembly-language
//
// tldr:
//
// 1. A program may contain both 32-bit ARM and 16-bit Thumb instructions.
// 2. A pointer to a function compiled with 16-bit Thumb instructions has 1 bit
//    in the LSB to trigger a CPU switch to Thumb state when calling the
//    function.
//
// Thus, this trick with the LSB only work reliably when Thumb is disabled.
constexpr uintptr_t PROXY = 1ul;

// TASK is only used in PROXY frames. A handle to a coroutine can be linkTo()'d
// to a PROXY frame. Fortunately, a CoroutineFrame is at least 4-byte aligned,
// which means we actually have 2 LSB's available for tagging.
constexpr uintptr_t TASK = 1ul << 1;

constexpr bool HaveSpareBitsInPC = true;

#else

// Unknown architecture, or an architecture known not to have any spare bits
// in a pointer to a function. We're going to fall back to magic numbers
// in `destroyFn` to tell between different frame types, and use an extra
// pointer for a uplink.
constexpr uintptr_t PROXY = 1;
constexpr uintptr_t TASK = 2;
constexpr bool HaveSpareBitsInPC = false;
#endif

// We use `CoroutineFrame::destroyFn` for storing links between coroutine
// frames. Thus, we disallow awaitables to call destroy(). The tags above are
// stored in `CoroutineFrame::destroyFn`.
constexpr uintptr_t MASK = PROXY | TASK;

} // namespace frame_tags

// A CoroutineFrame constructed by the library, rather than the compiler.
//
// This exists to allow intercepting the resumption of a parent task in order to
// do something other than immediately resuming the task that's backing it,
// such as propagate a cancellation instead. We also store a linkage pointer in
// the otherwise-unused destroyFn field in order to allow extracting async
// backtraces.
template <bool HaveSpareBitsInPC> struct ProxyFrameImpl;

template <> struct ProxyFrameImpl<true> : public CoroutineFrame {
  public:
    static bool isTagged(uintptr_t tag, CoroutineFrame* f) {
        uintptr_t fnAddr = reinterpret_cast<uintptr_t>(f->destroyFn);
        return (fnAddr & tag) == tag;
    }

    // Make a link from `this` to `h` such that `h` will be returned from calls
    // to `followLink()`.
    void linkTo(Handle h) {
        uintptr_t handleAddr = reinterpret_cast<uintptr_t>(h.address());
        CORRAL_ASSERT((handleAddr & frame_tags::MASK) == 0);
        uintptr_t fnAddr =
                reinterpret_cast<uintptr_t>(CoroutineFrame::destroyFn);
        CoroutineFrame::destroyFn =
                reinterpret_cast<decltype(CoroutineFrame::destroyFn)>(
                        (handleAddr & ~frame_tags::MASK) |
                        (fnAddr & frame_tags::MASK));
    }

    // Returns a coroutine handle for the task that was linked to
    // `this` via `linkTo()`. Returns `nullptr` if no task has
    // been linked.
    Handle followLink() const {
        uintptr_t fnAddr =
                reinterpret_cast<uintptr_t>(CoroutineFrame::destroyFn);
        return Handle::from_address(
                reinterpret_cast<void*>(fnAddr & ~frame_tags::MASK));
    }

  protected:
    void tagWith(uintptr_t tag) {
        uintptr_t fnAddr =
                reinterpret_cast<uintptr_t>(CoroutineFrame::destroyFn);
        fnAddr |= tag;
        CoroutineFrame::destroyFn =
                reinterpret_cast<decltype(CoroutineFrame::destroyFn)>(fnAddr);
    }
};

// A version of the above for architectures not offering any spare bits
// in function pointers.
// Since coroutine frames are nevertheless at least 4-byte aligned,
// we still can reuse lower bits for tags.
template <> struct ProxyFrameImpl<false> : public CoroutineFrame {
  public:
    void linkTo(Handle h) {
        uintptr_t p = reinterpret_cast<uintptr_t>(h.address());
        CORRAL_ASSERT((p & frame_tags::MASK) == 0);
        link_ = (link_ & frame_tags::MASK) | p;
    }

    Handle followLink() const {
        return Handle::from_address(
                reinterpret_cast<void*>(link_ & ~frame_tags::MASK));
    }

    static bool isTagged(uintptr_t tag, CoroutineFrame* f) {
        return f->destroyFn == &ProxyFrameImpl::tagFn &&
               (static_cast<ProxyFrameImpl*>(f)->link_ & tag) == tag;
    }

  protected:
    void tagWith(uintptr_t tag) {
        CoroutineFrame::destroyFn = &ProxyFrameImpl::tagFn;
        link_ = tag;
    }

  private:
    static void tagFn(CoroutineFrame*) {}

  private:
    uintptr_t link_ = 0;
};

struct ProxyFrame : public ProxyFrameImpl<frame_tags::HaveSpareBitsInPC> {
    static constexpr uintptr_t TAG = frame_tags::PROXY;
    ProxyFrame() { tagWith(TAG); }

    ProxyFrame(const ProxyFrame&) = delete;
};

// A ProxyFrame that corresponds to an underlying task invocation, used in
// the implementation of Task. In addition to the CoroutineFrame, it stores a
// program counter value representing the point at which the task will
// resume execution.
struct TaskFrame : ProxyFrame {
    static constexpr uintptr_t TAG = frame_tags::TASK | ProxyFrame::TAG;
    TaskFrame() { tagWith(TAG); }

    uintptr_t pc = 0;
};

// Attempts a conversion from `CoroutineFrame` to `F`. Returns `nullptr` if `f`
// does not point to an `F`.
template <std::derived_from<CoroutineFrame> F> F* frameCast(CoroutineFrame* f) {
    return (f && ProxyFrame::isTagged(F::TAG, f)) ? static_cast<F*>(f)
                                                  : nullptr;
}

} // namespace corral::detail
