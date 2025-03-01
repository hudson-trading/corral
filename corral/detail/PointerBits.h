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

#include "../config.h"

namespace corral::detail {

/// A utility class which can store a pointer and a small integer
/// (hopefully) in unused lower bits of the pointer.
///
/// If pointer does not have enough unused bits, will degrade
/// to a plain struct.
template <class T, class Bits, int Width, bool Merged> class PointerBitsImpl;

template <class T, class Bits, int Width>
class PointerBitsImpl<T, Bits, Width, true> {
    static constexpr const uintptr_t BitsMask = (1 << Width) - 1;

  public:
    PointerBitsImpl() noexcept = default;
    PointerBitsImpl(T* ptr, Bits bits) noexcept { set(ptr, bits); }

    T* ptr() const noexcept { return reinterpret_cast<T*>(data_ & ~BitsMask); }
    Bits bits() const noexcept { return static_cast<Bits>(data_ & BitsMask); }

    void set(T* ptr, Bits bits) noexcept {
        uintptr_t p = reinterpret_cast<uintptr_t>(ptr);
        uintptr_t b = static_cast<uintptr_t>(bits);
        CORRAL_ASSERT(!(p & BitsMask));
        CORRAL_ASSERT(!(b & ~BitsMask));
        data_ = (p | b);
    }

  private:
    uintptr_t data_ = 0;
};

template <class T, class Bits, int Width>
class PointerBitsImpl<T, Bits, Width, false> {
  public:
    PointerBitsImpl() noexcept = default;
    PointerBitsImpl(T* ptr, Bits bits) noexcept { set(ptr, bits); }

    T* ptr() const noexcept { return ptr_; }
    Bits bits() const noexcept { return bits_; }
    void set(T* ptr, Bits bits) noexcept {
        ptr_ = ptr;
        bits_ = bits;
    }

  private:
    T* ptr_ = nullptr;
    Bits bits_{};
};

template <class T, class Bits, int Width, int Align = alignof(T)>
class PointerBits
  : public PointerBitsImpl<T, Bits, Width, (Align >= (1 << Width))> {
  public:
    using PointerBits::PointerBitsImpl::PointerBitsImpl;

    void setPtr(T* ptr) { this->set(ptr, this->bits()); }
    void setBits(Bits bits) { this->set(this->ptr(), bits); }
};

} // namespace corral::detail
