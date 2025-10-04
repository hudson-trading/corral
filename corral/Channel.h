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

#include <iterator>
#include <limits>
#include <optional>
#include <ranges>

#include "config.h"
#include "detail/ParkingLot.h"
#include "detail/Queue.h"

namespace corral {

template <typename T> struct Channel;

namespace detail::channel {

template <typename T> struct ReadHalf;
template <typename T> struct WriteHalf;
template <typename Half> class AwaiterBase;
template <typename Half, typename... Args> class Awaiter;

template <typename T, typename E>
concept input_iterator_to =
        std::input_iterator<T> && std::convertible_to<std::iter_value_t<T>, E>;

template <typename T, typename E>
concept input_range_of = std::ranges::input_range<T> &&
                         std::convertible_to<std::ranges::range_value_t<T>, E>;


/// Interface for reading from a Channel. Exposed publicly as
/// Channel<T>::ReadHalf. See Channel for documentation.
template <typename T>
class ReadHalf : public corral::detail::ParkingLotImpl<ReadHalf<T>> {
    friend Channel<T>;
    friend WriteHalf<T>;
    template <typename> friend class AwaiterBase;
    template <typename, typename...> friend class Awaiter;

    using Parked = typename ReadHalf::ParkingLotImpl::Parked;

  public:
    [[nodiscard]] Awaitable<std::optional<T>> auto receive();

    template <std::output_iterator<T> It>
    [[nodiscard]] Awaitable<It> auto receive(
            It out, size_t n = static_cast<size_t>(-1));

    template <std::ranges::output_range<T> R>
    [[nodiscard]] Awaitable<size_t> auto receive(R&& range);

    // Non-blocking variants of the above

    std::optional<T> tryReceive() { return tryTransfer(0); }
    template <std::output_iterator<T> It>
    It tryReceive(It out, size_t n = static_cast<size_t>(-1)) {
        return tryTransfer(0, out, n);
    }
    template <std::ranges::output_range<T> R> size_t tryReceive(R&& range) {
        return tryTransfer(0, std::forward<R>(range));
    }

    size_t size() const noexcept;
    bool empty() const noexcept { return size() == 0; }
    bool closed() const noexcept { return channel().closed(); }

  protected:
    // Only allow construction and destruction as a subobject of
    // Channel; we assume we can't exist as a standalone object.
    ReadHalf() = default;
    ~ReadHalf() = default;

  private:
    Channel<T>& channel() { return static_cast<Channel<T>&>(*this); }
    const Channel<T>& channel() const {
        return static_cast<const Channel<T>&>(*this);
    }

    bool hasWaiters() const noexcept {
        return !ReadHalf::ParkingLotImpl::empty();
    }

    std::optional<T> tryTransfer(size_t claimedCount);

    template <std::output_iterator<T> It>
    It tryTransfer(size_t claimedCount, It out, size_t n);

    template <std::ranges::output_range<T> R>
    size_t tryTransfer(size_t claimedCount, R&& range);


    bool isNoop() const noexcept { return false; }

    template <std::output_iterator<T> It>
    bool isNoop(const It&, size_t n) const noexcept {
        return n == 0;
    }

    template <std::ranges::output_range<T> R>
    bool isNoop(const R& range) const noexcept {
        return std::ranges::empty(range);
    }


    size_t unclaimedCount() const noexcept { return size(); }
    void wakeAwaiters();

  private:
    size_t claims_ = 0;
};

/// Interface for writing to a Channel. Exposed publicly as
/// Channel<T>::WriteHalf. See Channel for documentation.
template <typename T>
class WriteHalf : public corral::detail::ParkingLotImpl<WriteHalf<T>> {
    friend Channel<T>;
    friend ReadHalf<T>;
    template <typename> friend class AwaiterBase;
    template <typename, typename...> friend class Awaiter;

    using Parked = typename WriteHalf::ParkingLotImpl::Parked;

  public:
    template <typename U> [[nodiscard]] Awaitable<bool> auto send(U&& value);

    template <input_iterator_to<T> It>
    [[nodiscard]] Awaitable<It> auto send(It begin, It end);

    template <input_range_of<T> R>
    [[nodiscard]] Awaitable<size_t> auto send(R&& range);

    template <typename U>
        requires(std::is_constructible_v<T, U>)
    bool trySend(U&& value) {
        return tryTransfer(0, std::forward<U>(value));
    }
    template <input_iterator_to<T> It> It trySend(It begin, It end) {
        return tryTransfer(0, begin, end);
    }
    template <input_range_of<T> R> size_t trySend(R&& range) {
        return tryTransfer(0, std::forward<R>(range));
    }

    void close() { channel().close(); }

    size_t space() const noexcept;
    bool full() const noexcept { return space() == 0; }
    bool closed() const noexcept { return channel().closed(); }

  protected:
    // Only allow construction and destruction as a subobject of
    // Channel; we assume we can't exist as a standalone object.
    WriteHalf() = default;
    ~WriteHalf() = default;

  private:
    Channel<T>& channel() { return static_cast<Channel<T>&>(*this); }
    const Channel<T>& channel() const {
        return static_cast<const Channel<T>&>(*this);
    }

    bool reallyFull() const noexcept;

    bool hasWaiters() const noexcept {
        return !WriteHalf::ParkingLotImpl::empty();
    }


    template <typename U>
        requires(std::is_constructible_v<T, U>)
    bool tryTransfer(size_t claimedCount, U&& value);

    template <input_iterator_to<T> It>
    It tryTransfer(size_t claimedCount, It begin, It end);

    template <input_range_of<T> R>
    size_t tryTransfer(size_t claimedCount, R&& range);


    template <typename U>
        requires(std::is_constructible_v<T, U>)
    bool isNoop(const U&) const {
        return false;
    }

    template <input_iterator_to<T> It>
    bool isNoop(const It& begin, const It& end) const {
        return begin == end;
    }

    template <input_range_of<T> R> bool isNoop(const R& range) const {
        return std::ranges::empty(range);
    }


    size_t unclaimedCount() const noexcept { return space(); }
    void wakeAwaiters();

  private:
    size_t claims_ = 0;
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
    size_t size() const noexcept { return ReadHalf::size(); }

    /// Returns true if this channel contains no objects, i.e., a call
    /// to tryReceive() will return std::nullopt.
    bool empty() const noexcept { return ReadHalf::empty(); }

    /// Returns the number of slots immediately available to write
    /// new objects into this channel, i.e., the number of times in a row
    /// that you can call trySend() successfully.
    size_t space() const noexcept { return WriteHalf::space(); }

    /// Returns true if this channel contains no space for more objects,
    /// i.e., a call to trySend() will return false. This may be because
    /// the channel is closed or because it has reached its capacity limit.
    bool full() const noexcept { return WriteHalf::full(); }

    /// Returns true if close() has been called on this channel.
    bool closed() const noexcept { return closed_; }

    /// Closes the channel. No more data can be written to the channel. All
    /// queued writes are aborted. Any suspended reads will still be able to
    /// read whatever data remains in the channel.
    void close();


    //
    // Receive operations
    //

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
    [[nodiscard]] Awaitable<std::optional<T>> auto receive() {
        return readHalf().receive();
    }

    /// Retrieve and return up to `n` objects from the channel, writing them
    /// to `out`. May retrieve less elements than requested, but blocks
    /// if no objects are immediately available.
    template <std::output_iterator<T> It>
    [[nodiscard]] Awaitable<It> auto receive(
            It out, size_t n = static_cast<size_t>(-1)) {
        return readHalf().receive(std::move(out), n);
    }

    /// Retrieve and return up to `n` objects from the channel, writing them
    /// to `r`. Returns the number of elements retrieved.
    /// May retrieve less elements than requested, but blocks if no objects
    /// are immediately available.
    template <std::ranges::output_range<T> R>
    [[nodiscard]] Awaitable<size_t> auto receive(R&& r) {
        return readHalf().receive(std::forward<R>(r));
    }

    /// Retrieve and return an object from the channel, or return std::nullopt
    /// if none are immediately available.
    std::optional<T> tryReceive() { return readHalf().tryReceive(); }

    /// Retrieve and return up to `n` objects from the channel, writing them
    /// to `out`. If no elements are immediately available, does nothing
    /// and returns `out` unmodified.
    template <std::output_iterator<T> It> It tryReceive(size_t n, It out) {
        return readHalf().tryReceive(n, std::move(out));
    }

    /// Retrieve and return up to `n` objects from the channel, writing them
    /// to `r`. Returns the number of elements retrieved, or 0 if none are
    /// immediately available.
    template <std::ranges::output_range<T> R> size_t tryReceive(R&& r) {
        return readHalf().tryReceive(std::forward<R>(r));
    }


    //
    // Send operations
    //

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
    template <typename U>
        requires(std::is_constructible_v<T, U>)
    [[nodiscard]] Awaitable<bool> auto send(U&& value) {
        return writeHalf().send(std::forward<U>(value));
    }

    /// Deliver a range of objects to the channel. May push less than requested
    /// number of elements, but blocks if the channel was already full.
    /// Returns the iterator to the first element that was not delivered.
    template <detail::channel::input_iterator_to<T> It>
    [[nodiscard]] Awaitable<It> auto send(It begin, It end) {
        return writeHalf().send(std::move(begin), std::move(end));
    }

    /// Deliver a range of objects to the channel. May push less than requested
    /// number of elements, but blocks if the channel was already full.
    /// Returns the number of elements that were pushed.
    template <detail::channel::input_range_of<T> R>
    [[nodiscard]] Awaitable<size_t> auto send(R&& r) {
        return writeHalf().send(std::forward<R>(r));
    }

    /// Deliver an object to the channel if there is space immediately
    /// available for it in the buffer. Returns true if the object was
    /// delivered, false if there was no space or the channel was closed.
    template <typename U>
        requires(std::is_constructible_v<T, U>)
    bool trySend(U&& value) {
        return writeHalf().trySend(std::forward<U>(value));
    }

    /// Deliver a range of objects to the channel.
    /// Returns the iterator to the first element that was not delivered
    /// (which may be `begin` if the channel was full, or `end`
    /// if all elements were pushed).
    template <detail::channel::input_iterator_to<T> It>
    It trySend(It begin, It end) {
        return writeHalf().trySend(std::move(begin), std::move(end));
    }

    /// Deliver a range of objects to the channel. Returns the number of
    /// elements that were pushed (which may be 0 if the channel was full).
    template <detail::channel::input_range_of<T> R> size_t trySend(R&& r) {
        return writeHalf().trySend(std::forward<R>(r));
    }

  private:
    friend ReadHalf;
    friend WriteHalf;

  private:
    corral::detail::Queue<T> buf_;
    bool closed_ = false;
    bool bounded_ = false;
};


// --------------------------------------------------------------------------------
// Implementation
//

namespace detail::channel {

//
// Awaiter
//

// Awaiter::await_ready() cannot simply return `channel().empty()` (or full(),
// depending on direction); otherwise
//     co_await anyOf(chan.receive(), chan.receive());
// will serve a premature EOF if the channel holds one element (because
// the element is only fetched in Awaiter::await_resume(), and only one awaiter
// can be fulfilled).
//
// To work around this (and to maintain early-cancellability), we allow Awaiters
// to claim available elements (or free space) on the channel during Awaiter
// construction. Awaiter which claimed an element is await_ready(), so can
// proceed directly to await_resume() which will carry out the transfer.
// Channel will keep track of how many claims are outstanding, and will suspend
// any further transfers (or reject trySend() / tryReceive()) if this will make
// any pending claimant short of an element to perform on.

template <typename Half> class AwaiterBase : public Half::Parked {
    friend Half;

    static constexpr const unsigned ClaimBit = 1;

  public:
    explicit AwaiterBase(Half& half) : Half::Parked(half) {}

    AwaiterBase(const AwaiterBase&) = delete;
    AwaiterBase& operator=(const AwaiterBase&) = delete;

    ~AwaiterBase() {
        if (claimed()) {
            --this->object().claims_;
            this->object().wakeAwaiters();
        }
    }

    bool await_suspend(corral::Handle h) {
        CORRAL_ASSERT(!claimed());
        if (this->object().unclaimedCount() > 0) {
            CORRAL_TRACE("    ...channel %p awaiter %p [ready]", &channel(),
                         this);
            this->claim();
            return false;
        } else {
            CORRAL_TRACE("    ...channel %p awaiter %p", &channel(), this);
            this->doSuspend(h);
            return true;
        }
    }

    using Half::Parked::await_cancel;

  protected:
    auto& channel() { return this->object().channel(); }

    bool claimed() const { return this->bits() & ClaimBit; }

    void claim() {
        CORRAL_ASSERT(!claimed());
        this->setBits(ClaimBit);
        ++this->object().claims_;
    }

    using Half::Parked::unpark;
};

template <typename Half, typename... Args>
class Awaiter : public AwaiterBase<Half> {
  public:
    explicit Awaiter(Half& half, Args... args)
      : AwaiterBase<Half>(half), args_(std::forward<Args>(args)...) {
        if (half.unclaimedCount() > 0 && !isNoop()) {
            this->claim();
        }
    }

    bool await_ready() const noexcept { return ready(); }

    auto await_resume() && {
        CORRAL_ASSERT(ready());
        size_t claimedCount = (this->claimed() ? 1 : 0);
        this->setBits(0);
        return std::apply(
                [&]<class... U>(U&&... args) {
                    return this->object().tryTransfer(claimedCount,
                                                      std::forward<U>(args)...);
                },
                std::move(args_));
    }

  private:
    bool ready() const noexcept {
        return this->claimed() || this->object().closed() || isNoop();
    }

    bool isNoop() const {
        return std::apply(
                [&](const auto&... args) {
                    return this->object().isNoop(args...);
                },
                args_);
    }

  private:
    CORRAL_NO_UNIQUE_ADDR std::tuple<Args...> args_;
};


//
// ReadHalf
//

template <typename T> size_t ReadHalf<T>::size() const noexcept {
    return channel().buf_.size() - claims_;
}

template <typename T> void ReadHalf<T>::wakeAwaiters() {
    while (unclaimedCount() && !ReadHalf::ParkingLotImpl::empty()) {
        auto awaiter = static_cast<AwaiterBase<ReadHalf>*>(this->peek());
        awaiter->claim();
        awaiter->unpark();
    }
}

template <typename T>
std::optional<T> ReadHalf<T>::tryTransfer(size_t claimedCount) {
    CORRAL_ASSERT(claimedCount <= claims_);
    if (!claimedCount && !unclaimedCount()) {
        return std::nullopt;
    }
    CORRAL_ASSERT(!channel().buf_.empty());

    std::optional<T> data(std::move(channel().buf_.front()));
    channel().buf_.pop_front();
    claims_ -= claimedCount;
    channel().writeHalf().wakeAwaiters();
    return data;
}

template <typename T>
template <std::output_iterator<T> It>
It ReadHalf<T>::tryTransfer(size_t claimedCount, It it, size_t n) {
    CORRAL_ASSERT(claimedCount <= claims_);
    n = std::min<size_t>(n, claimedCount + unclaimedCount());
    if (n == 0) {
        return it;
    }
    CORRAL_ASSERT(!channel().buf_.empty());

    std::tie(it, std::ignore) = channel().buf_.pop_front_to(n, std::move(it));
    claims_ -= claimedCount;
    channel().writeHalf().wakeAwaiters();
    return it;
}

template <typename T>
template <std::ranges::output_range<T> R>
size_t ReadHalf<T>::tryTransfer(size_t claimedCount, R&& range) {
    CORRAL_ASSERT(claimedCount <= claims_);
    size_t n = claimedCount + unclaimedCount();
    if constexpr (std::ranges::forward_range<R>) {
        n = std::min<size_t>(n, std::ranges::distance(range));
    }
    CORRAL_ASSERT(!channel().buf_.empty());

    auto [_, ret] = channel().buf_.pop_front_to(n, std::ranges::begin(range));
    claims_ -= claimedCount;
    channel().writeHalf().wakeAwaiters();
    return ret;
}


template <typename T> Awaitable<std::optional<T>> auto ReadHalf<T>::receive() {
    using Ret = Awaiter<ReadHalf>;
    return makeAwaitable<Ret, ReadHalf&>(*this);
}

template <typename T>
template <std::output_iterator<T> It>
Awaitable<It> auto ReadHalf<T>::receive(It it, size_t n) {
    using Ret = Awaiter<ReadHalf, It, size_t>;
    return makeAwaitable<Ret, ReadHalf&, It, size_t>(*this, std::move(it), n);
}

template <typename T>
template <std::ranges::output_range<T> R>
Awaitable<size_t> auto ReadHalf<T>::receive(R&& r) {
    using Ret = Awaiter<ReadHalf, R&&>;
    return makeAwaitable<Ret, ReadHalf&, R&&>(*this, std::forward<R>(r));
}


//
// WriteHalf
//

template <typename T> size_t WriteHalf<T>::space() const noexcept {
    if (channel().closed_) {
        return 0;
    } else if (!channel().bounded_) {
        return (std::numeric_limits<size_t>::max)();
    } else {
        return channel().buf_.capacity() - channel().buf_.size() - claims_;
    }
}

template <typename T> void WriteHalf<T>::wakeAwaiters() {
    while (unclaimedCount() && !WriteHalf::ParkingLotImpl::empty()) {
        auto awaiter = static_cast<AwaiterBase<WriteHalf>*>(this->peek());
        awaiter->claim();
        awaiter->unpark();
    }
}

template <typename T> bool WriteHalf<T>::reallyFull() const noexcept {
    return channel().bounded_ &&
           channel().buf_.capacity() == channel().buf_.size();
}

template <typename T>
template <typename U>
    requires(std::is_constructible_v<T, U>)
bool WriteHalf<T>::tryTransfer(size_t claimedCount, U&& value) {
    CORRAL_ASSERT(claimedCount <= claims_);
    if (!claimedCount && !unclaimedCount()) {
        return false;
    }
    CORRAL_ASSERT(!channel().closed() && !reallyFull());

    claims_ -= claimedCount;
    channel().buf_.push_back(std::forward<U>(value));
    channel().readHalf().wakeAwaiters();
    return true;
}

template <typename T>
template <input_iterator_to<T> It>
It WriteHalf<T>::tryTransfer(size_t claimedCount, It begin, It end) {
    CORRAL_ASSERT(claimedCount <= claims_);
    if (begin == end) {
        return begin;
    }
    size_t avail = channel().bounded_ ? claimedCount + unclaimedCount()
                                      : (std::numeric_limits<size_t>::max)();
    if (!avail) {
        return begin;
    }
    CORRAL_ASSERT(!channel().closed() && !reallyFull());

    if constexpr (std::random_access_iterator<It>) {
        size_t n = std::min<size_t>(end - begin, avail);
        end = begin + n;
        channel().buf_.append(begin, end);
    } else {
        while (avail-- && (begin != end)) {
            channel().buf_.push_back(std::move(*begin));
            ++begin;
        }
        end = begin;
    }

    claims_ -= claimedCount;
    channel().readHalf().wakeAwaiters();
    return end;
}

template <typename T>
template <input_range_of<T> R>
size_t WriteHalf<T>::tryTransfer(size_t claimedCount, R&& range) {
    CORRAL_ASSERT(claimedCount <= claims_);
    auto begin = std::ranges::begin(range);
    auto end = std::ranges::end(range);
    size_t n = 0;

    if (begin == end) {
        return 0;
    }
    size_t avail = channel().bounded_ ? claimedCount + unclaimedCount()
                                      : (std::numeric_limits<size_t>::max)();
    if (!avail) {
        return 0;
    }
    CORRAL_ASSERT(!channel().closed() && !reallyFull());

    if constexpr (std::ranges::random_access_range<R>) {
        n = std::min<size_t>(end - begin, avail);
        end = begin + n;
        channel().buf_.append(begin, end);
    } else {
        while (avail-- && (begin != end)) {
            channel().buf_.push_back(std::move(*begin));
            ++begin, ++n;
        }
    }

    claims_ -= claimedCount;
    channel().readHalf().wakeAwaiters();
    return n;
}

template <typename T>
template <typename U>
Awaitable<bool> auto WriteHalf<T>::send(U&& value) {
    using Ret = Awaiter<WriteHalf, U>;
    return makeAwaitable<Ret, WriteHalf&, U&&>(*this, std::forward<U>(value));
}

template <typename T>
template <input_iterator_to<T> It>
Awaitable<It> auto WriteHalf<T>::send(It begin, It end) {
    using Ret = Awaiter<WriteHalf, It, It>;
    return makeAwaitable<Ret, WriteHalf&, It, It>(*this, std::move(begin),
                                                  std::move(end));
}

template <typename T>
template <input_range_of<T> R>
Awaitable<size_t> auto WriteHalf<T>::send(R&& range) {
    using Ret = Awaiter<WriteHalf, R&&>;
    return makeAwaitable<Ret, WriteHalf&, R&&>(*this, std::forward<R>(range));
}

} // namespace detail::channel

//
// Channel
//

template <typename T> void Channel<T>::close() {
    closed_ = true;
    readHalf().unparkAll();
    writeHalf().unparkAll();
}

} // namespace corral
