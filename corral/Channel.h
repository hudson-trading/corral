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

#include <optional>

#include "config.h"
#include "detail/ParkingLot.h"
#include "detail/Queue.h"

namespace corral {

template <typename T> struct Channel;

namespace detail::channel {

template <typename T> struct ReadHalf;
template <typename T> struct WriteHalf;

/// Interface for reading from a Channel. Exposed publicly as
/// Channel<T>::ReadHalf. See Channel for documentation.
template <typename T>
class ReadHalf : public corral::detail::ParkingLotImpl<ReadHalf<T>> {
    friend Channel<T>;
    friend WriteHalf<T>;

  private:
    Channel<T>& channel() { return static_cast<Channel<T>&>(*this); }
    const Channel<T>& channel() const {
        return static_cast<const Channel<T>&>(*this);
    }

    struct ReadAwaiter : public ReadHalf::ParkingLotImpl::Parked {
        using Base = typename ReadHalf::ParkingLotImpl::Parked;

        explicit ReadAwaiter(ReadHalf& self) : Base(self) {}

        bool await_ready() const noexcept {
            return !channel().empty() || channel().closed();
        }

        void await_suspend(corral::Handle h) {
            CORRAL_TRACE("    ...channel %p receive %p", &channel(), this);
            this->doSuspend(h);
        }

        std::optional<T> await_resume() { return channel().tryReceive(); }

        using Base::await_cancel;

      private:
        Channel<T>& channel() {
            return static_cast<Channel<T>&>(Base::object());
        }
        const Channel<T>& channel() const {
            return static_cast<const Channel<T>&>(Base::object());
        }
    };

  public:
    corral::Awaitable/*<std::optional<T>>*/ auto receive() {
        return ReadAwaiter(*this);
    }
    std::optional<T> tryReceive() {
        std::optional<T> data;
        if (!channel().empty()) {
            data.emplace(std::move(channel().buf_.front()));
            channel().buf_.pop_front();
            this->channel().writeHalf().unparkOne();
        }
        return data;
    }

    size_t size() const noexcept { return channel().size(); }
    bool empty() const noexcept { return channel().empty(); }
    bool closed() const noexcept { return channel().closed(); }

  protected:
    // Only allow construction and destruction as a subobject of
    // Channel; we assume we can't exist as a standalone object.
    ReadHalf() = default;
    ~ReadHalf() = default;

    bool hasWaiters() const noexcept {
        return !ParkingLotImpl<ReadHalf<T>>::empty();
    }
};

/// Interface for writing to a Channel. Exposed publicly as
/// Channel<T>::WriteHalf. See Channel for documentation.
template <typename T>
class WriteHalf : public corral::detail::ParkingLotImpl<WriteHalf<T>> {
    friend Channel<T>;
    friend ReadHalf<T>;

  private:
    Channel<T>& channel() { return static_cast<Channel<T>&>(*this); }
    const Channel<T>& channel() const {
        return static_cast<const Channel<T>&>(*this);
    }

    template <typename U>
    struct WriteAwaiter : public WriteHalf::ParkingLotImpl::Parked {
        using Base = typename WriteHalf::ParkingLotImpl::Parked;

        WriteAwaiter(WriteHalf& self, U&& data)
          : Base(self), data_(std::forward<U>(data)) {}

        bool await_ready() const noexcept {
            return channel().closed() || !channel().full();
        }

        void await_suspend(corral::Handle h) {
            CORRAL_TRACE("    ...channel %p send %p", &channel(), this);
            this->doSuspend(h);
        }

        bool await_resume() {
            return channel().trySend(std::forward<U>(data_));
        }

        using Base::await_cancel;

      private:
        Channel<T>& channel() {
            return static_cast<Channel<T>&>(Base::object());
        }
        const Channel<T>& channel() const {
            return static_cast<const Channel<T>&>(Base::object());
        }

        U&& data_;
    };

  public:
    template <typename U> corral::Awaitable<bool> auto send(U&& value) {
        return WriteAwaiter<U>(*this, std::forward<U>(value));
    }
    template <typename U> bool trySend(U&& value) {
        if (channel().closed() || channel().full()) {
            return false;
        } else {
            channel().buf_.push_back(std::forward<U>(value));
            channel().readHalf().unparkOne();
            return true;
        }
    }
    void close() { channel().close(); }

    size_t space() const noexcept { return channel().space(); }
    bool full() const noexcept { return channel().full(); }
    bool closed() const noexcept { return channel().closed(); }

  protected:
    // Only allow construction and destruction as a subobject of
    // Channel; we assume we can't exist as a standalone object.
    WriteHalf() = default;
    ~WriteHalf() = default;

    bool hasWaiters() const noexcept {
        return !ParkingLotImpl<WriteHalf<T>>::empty();
    }
};

} // namespace detail::channel

/// A ordered communication channel for sending objects of type T
/// between tasks.
///
/// Each Channel has an internal buffer to store the objects that have been
/// sent but not yet received. This buffer may be allowed to grow without
/// bound (the default) or may be limited to a specific size. Because
/// every object sent through the channel must pass through the buffer
/// on its way from the sending task to the receiving task, a buffer size
/// of zero is nonsensical and is forbidden at construction time.
/// In general, if you want to be able to send N objects in a row without
/// blocking, you must allow for a buffer size of at least N. Even if
/// there are tasks waiting to immediately receive each object you enqueue,
/// the objects will still be stored in the buffer until the next tick of
/// the executor.
///
/// A Channel can be closed by calling its close() method. Closing the
/// channel causes all sleeping readers and writers to wake up with
/// a failure indication, and causes all future reads and writes to
/// fail immediately rather than blocking. If you send some objects
/// through a channel and then close it, the objects that were sent
/// before the closure can still be validly received. Note that destroying
/// the Channel object is _not_ equivalent to closing it; a destroyed
/// Channel must not have any readers or writers still waiting on it.
///
/// A Channel acts as if it is made up of two subobjects, one for reading
/// and one for writing, which can be referenced separately. For example,
/// if you want to allow someone to write to your channel but not read from
/// it, you can pass them a Channel<T>::WriteHalf& and they will only have
/// access to the writing-related methods. The ReadHalf& and WriteHalf& types
/// are implicitly convertible from Channel&, or you can use the readHalf()
/// and writeHalf() methods to obtain them.
template <typename T>
struct Channel : public detail::channel::ReadHalf<T>,
                 public detail::channel::WriteHalf<T> {
    /// Constructs an unbounded channel. No initial capacity is allocated;
    /// later channel operations will need to allocate to do their work.
    Channel() : buf_(0), bounded_(false) {}

    /// Constructs a bounded channel. Space for `maxSize` buffered objects
    /// of type T will be allocated immediately, and no further allocations
    /// will be performed.
    explicit Channel(size_t maxSize) : buf_(maxSize), bounded_(true) {
        CORRAL_ASSERT(maxSize > 0);
    }

    /// Detect some uses of 'Channel(0)' and fail at compile time.
    /// (A literal 0 has conversions of equal rank to size_t and nullptr_t.)
    explicit Channel(std::nullptr_t) = delete;

    /// Verify that no tasks are still waiting on this Channel when it is
    /// about to be destroyed.
    ~Channel() {
        CORRAL_ASSERT(
                !readHalf().hasWaiters() &&
                "Still some tasks suspended while reading from this channel");
        CORRAL_ASSERT(
                !writeHalf().hasWaiters() &&
                "Still some tasks suspended while writing to this channel");
    }

    /// Returns the number of objects immediately available to read
    /// from this channel, i.e., the number of times in a row that you
    /// can call tryReceive() successfully.
    size_t size() const noexcept { return buf_.size(); }

    /// Returns true if this channel contains no objects, i.e., a call
    /// to tryReceive() will return std::nullopt.
    bool empty() const noexcept { return buf_.empty(); }

    /// Returns the number of slots immediately available to write
    /// new objects into this channel, i.e., the number of times in a row
    /// that you can call trySend() successfully.
    size_t space() const noexcept {
        if (closed_) {
            return 0;
        }
        if (!bounded_ && !closed_) {
            return static_cast<size_t>(-1);
        }
        return buf_.capacity() - buf_.size();
    }

    /// Returns true if this channel contains no space for more objects,
    /// i.e., a call to trySend() will return false. This may be because
    /// the channel is closed or because it has reached its capacity limit.
    bool full() const noexcept {
        return (bounded_ && buf_.capacity() == buf_.size()) || closed_;
    }

    /// Returns true if close() has been called on this channel.
    bool closed() const noexcept { return closed_; }

    /// Closes the channel. No more data can be written to the channel. All
    /// queued writes are aborted. Any suspended reads will still be able to
    /// read whatever data remains in the channel.
    void close() {
        closed_ = true;
        readHalf().unparkAll();
        writeHalf().unparkAll();
    }

    /// A reference to this channel that only exposes the operations that
    /// would be needed by a reader: receive(), tryReceive(), size(),
    /// empty(), and closed().
    using ReadHalf = detail::channel::ReadHalf<T>;
    ReadHalf& readHalf() { return static_cast<ReadHalf&>(*this); }
    const ReadHalf& readHalf() const {
        return static_cast<const ReadHalf&>(*this);
    }

    /// Retrieve and return an object from the channel, blocking if no objects
    /// are immediately available. Returns std::nullopt if the channel is
    /// closed and has no objects left to read.
    corral::Awaitable/*<std::optional<T>>*/ auto receive() {
        return readHalf().receive();
    }

    /// Retrieve and return an object from the channel, or return std::nullopt
    /// if none are immediately available.
    std::optional<T> tryReceive() { return readHalf().tryReceive(); }

    /// A reference to this channel that only exposes the operations that
    /// would be needed by a writer: send(), trySend(), close(), space(),
    /// full(), and closed().
    using WriteHalf = detail::channel::WriteHalf<T>;
    WriteHalf& writeHalf() { return static_cast<WriteHalf&>(*this); }
    const WriteHalf& writeHalf() const {
        return static_cast<const WriteHalf&>(*this);
    }

    /// Deliver an object to the channel, blocking if there is no space
    /// available in the buffer. Returns false if the channel cannot
    /// accept more incoming objects because it has been closed, true if
    /// the object was delivered.
    template <typename U> corral::Awaitable<bool> auto send(U&& value) {
        return writeHalf().send(std::forward<U>(value));
    }

    /// Deliver an object to the channel if there is space immediately
    /// available for it in the buffer. Returns true if the object was
    /// delivered, false if there was no space or the channel was closed.
    template <typename U> bool trySend(U&& value) {
        return writeHalf().trySend(std::forward<U>(value));
    }

  protected:
    friend ReadHalf;
    friend WriteHalf;

    corral::detail::Queue<T> buf_;
    bool closed_ = false;
    bool bounded_ = false;
};

} // namespace corral
