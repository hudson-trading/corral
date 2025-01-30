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
#include "IntrusiveList.h"
#include "utility.h"

namespace corral::detail {

/// A common part of ParkingLot, Event and Semaphore,
/// an intrusive wait queue.
template <class Self> class ParkingLotImpl {
  public:
    ParkingLotImpl() = default;
    ParkingLotImpl(ParkingLotImpl&&) = delete;
    ParkingLotImpl& operator=(ParkingLotImpl&&) = delete;

  protected:
    class Parked : public IntrusiveListItem<Parked> {
      public:
        explicit Parked(Self& object) : object_(object) {}

        auto await_cancel(Handle) noexcept {
            this->unlink();
            handle_ = std::noop_coroutine();
            return std::true_type{};
        }

      protected:
        Self& object() { return object_; }
        const Self& object() const { return object_; }

        void doSuspend(Handle h) {
            handle_ = h;
            object_.parked_.push_back(*this);
        }
        void unpark() {
            this->unlink();
            std::exchange(handle_, std::noop_coroutine()).resume();
        }

      private:
        Self& object_;
        Handle handle_;
        friend class ParkingLotImpl<Self>;
    };
    friend class Parked;

  protected:
    /// Return a pointer to the awaiter whose task unparkOne()
    /// would wake, or nullptr if there are no waiters currently.
    /// You can use its unpark() method to wake it and remove it
    /// from the list of waiters.
    Parked* peek() {
        if (!parked_.empty()) {
            return &parked_.front();
        }
        return nullptr;
    }

    /// Wake the oldest waiting task, removing it from the list of waiters.
    void unparkOne() {
        if (!parked_.empty()) {
            parked_.front().unpark();
        }
    }

    /// Wake all tasks that were waiting when the call to unparkAll() began.
    void unparkAll() {
        auto parked = std::move(parked_);
        while (!parked.empty()) {
            parked.front().unpark();
        }
    }

    /// Returns true if no tasks are waiting, which implies peek() will
    /// return nullptr and unparkOne() and unparkAll() are no-ops.
    bool empty() const { return parked_.empty(); }

  private:
    IntrusiveList<Parked> parked_;
};

} // namespace corral::detail
