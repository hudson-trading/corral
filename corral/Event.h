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

/// A level-triggered event supporting multiple waiters.
///
/// An Event tracks a boolean flag ('triggered') that semantically
/// represents whether something of interest has "happened yet",
/// and allows tasks to wait for the thing to happen. Upon
/// construction, the event has not happened (the boolean flag is
/// false); calling the trigger() method indicates that the event has
/// happened and wakes up any tasks that were waiting for that to occur.
/// Awaiting an event that has already been triggered is a no-op (no
/// suspension occurs).
///
/// An event can't "un-happen": once triggered, it can't be reset.
/// This avoids a variety of potential race conditions. If you want to
/// wait for a state _transition_ -- that is, wait for something to
/// *just now happen*, rather than *have happened yet* -- see
/// ParkingLot instead.
///
/// The awaitable (obtained through get() or operator co_await) is impicitly
/// convertible to a boolean flag representing whether the event has happened
/// yet. This makes it easy for a class to expose something that is both
/// a flag and an awaitable:
///
///    class Example {
///        corral::Event connected_;
///    public:
///        auto connected() { return connected_.get(); }
///        bool connected() const { return connected_.get(); }
///    };
///
///    if (example.connected()) { /* works as bool */ }
///    co_await example.connected(); // works as awaitable
///
class Event : public detail::ParkingLotImpl<Event> {
  public:
    class Awaitable : public detail::ParkingLotImpl<Event>::Parked {
      public:
        using Parked::Parked;
        explicit operator bool() { return this->object().triggered(); }
        bool await_ready() const noexcept { return this->object().triggered(); }
        void await_suspend(Handle h) { this->doSuspend(h); }
        void await_resume() {}
    };

    /// Trigger the event, waking any tasks that are waiting for it to
    /// occur.
    void trigger() {
        triggered_ = true;
        unparkAll();
    }

    /// Returns true if the event has happened yet.
    bool triggered() const noexcept { return triggered_; }

    /// Returns an awaitable which becomes ready when the event is triggered
    /// and is immediately ready if that has already happened.
    [[nodiscard]] corral::Awaitable<void> auto get() {
        return Awaitable(*this);
    }

    /// Alias for triggered() that allows generic code to use get() as
    /// a boolean regardless of the constness of the Event it's
    /// operating on.
    bool get() const noexcept { return triggered(); }

    corral::Awaitable<void> auto operator co_await() {
        return Awaitable(*this);
    }

  private:
    bool triggered_ = false;
};

} // namespace corral
