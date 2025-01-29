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
#include "detail/ParkingLot.h"

namespace corral {

/// A parking lot, aka wait queue, aka condition variable (only without the
/// accompanying mutex).
class ParkingLot : public detail::ParkingLotImpl<ParkingLot> {
  public:
    using Base = detail::ParkingLotImpl<ParkingLot>;
    class Awaiter : public Base::Parked {
      public:
        using Parked::Parked;
        bool await_ready() const noexcept { return false; }
        void await_suspend(Handle h) { return this->doSuspend(h); }
        void await_resume() {}
    };

    using Awaitable = Awaiter; // backwards compatibility

    /// Returns an awaitable which, when co_await'ed, suspends the caller
    /// until any of unpark*() functions are called.
    [[nodiscard]] corral::Awaiter<void> auto park() { return Awaiter(*this); }

    /// Resumes the earliest parked task, if any.
    void unparkOne() { return Base::unparkOne(); }

    /// Resumes all currently parked tasks.
    void unparkAll() { return Base::unparkAll(); }
};

} // namespace corral
