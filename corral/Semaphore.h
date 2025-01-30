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
#include "detail/ParkingLot.h"

namespace corral {

/// A semaphore, which can also be used to implement a lock.
///
/// The semaphore maintains an internal counter which logically tracks
/// a count of resources available to hand out; it is initially 1 or
/// can be overridden by passing an argument to the constructor.
/// `acquire()` waits for this counter to be at least 1, then decrements it.
/// `release()` increments the counter. `lock()` returns an RAII guard that
/// wraps `acquire()` and `release()`.
class Semaphore : public detail::ParkingLotImpl<Semaphore> {
    template <class Retval> class Awaiter;

  public:
    class Lock;

    explicit Semaphore(size_t initial = 1) : value_(initial) {}

    size_t value() const noexcept { return value_; }

    /// An awaitable that decrements the semaphore, suspending
    /// the caller if it is currently zero.
    [[nodiscard]] corral::Awaitable<void> auto acquire();

    /// Increments the semaphore, waking one suspended task (if any).
    void release();

    /// RAII-style decrement, returning guard object which
    /// will increment the semaphore back upon going out of scope
    [[nodiscard]] corral::Awaitable<Lock> auto lock();

  private:
    size_t value_;
};


//
// Implementation
//

class [[nodiscard]] Semaphore::Lock {
  public:
    Lock() = default;
    Lock(Lock&& lk) noexcept : sem_(std::exchange(lk.sem_, nullptr)) {}
    Lock& operator=(Lock lk) noexcept {
        std::swap(sem_, lk.sem_);
        return *this;
    }
    ~Lock() {
        if (sem_) {
            sem_->release();
        }
    }

  private:
    explicit Lock(Semaphore& sem) : sem_(&sem) {}
    friend class Semaphore::Awaiter<Lock>;

  private:
    Semaphore* sem_ = nullptr;
};

template <class Retval>
class Semaphore::Awaiter : public detail::ParkingLotImpl<Semaphore>::Parked {
  public:
    using Parked::Parked;
    bool await_ready() const noexcept { return this->object().value() > 0; }

    void await_suspend(Handle h) { this->doSuspend(h); }

    auto await_resume() {
        --this->object().value_;
        if constexpr (!std::is_same_v<Retval, void>) {
            return Retval(this->object());
        }
    }
};

inline corral::Awaitable<void> auto Semaphore::acquire() {
    return Awaiter<void>(*this);
}

inline corral::Awaitable<Semaphore::Lock> auto Semaphore::lock() {
    return Awaiter<Lock>(*this);
}

inline void Semaphore::release() {
    ++value_;
    unparkOne();
}

} // namespace corral
