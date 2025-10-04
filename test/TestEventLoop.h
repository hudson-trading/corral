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

#include <chrono>
#include <map>

#include "../corral/corral.h"
#include "config.h"

namespace corral::testing {

class TestEventLoop {
    using milliseconds = std::chrono::milliseconds;
    using EventQueue = std::multimap<milliseconds, std::function<void()>>;

  public:
    //
    // Public EventLoop-like interface
    //

    void run() {
        CORRAL_TRACE("=== running test case ===");
        running_ = true;
        while (running_) {
            // If this assertion fires, your test would have
            // deadlocked: there are tasks waiting for something, but
            // nothing more will happen.
            CORRAL_ASSERT(!events_.empty());
            step();
        }
    }

    ~TestEventLoop() {
        if (!events_.empty()) {
            CORRAL_TRACE("=== running event leftovers ===");
            while (!events_.empty()) {
                step();
            }
        }
        CORRAL_TRACE("=== done ===");
    }

    void stop() { running_ = false; }
    bool isRunning() const { return running_; }

    //
    // Fixture interface visible to test bodies
    //

    milliseconds now() const { return now_; }

    void schedule(milliseconds delay, std::function<void()> cb) {
        events_.emplace(now_ + delay, std::move(cb));
    }

    template <bool Cancellable> class Sleep;

    corral::Awaitable<void> auto sleep(milliseconds tm);

    struct UninterruptibleTag {
        constexpr UninterruptibleTag() = default;
    };
    corral::Awaitable<void> auto sleep(milliseconds tm, UninterruptibleTag);

  private:
    void step() {
        CORRAL_ASSERT(!events_.empty());
        auto [time, func] = *events_.begin();

        if (now_ != time) {
            now_ = time;
            CORRAL_TRACE("-- %ld ms --",
                         (long) std::chrono::duration_cast<
                                 std::chrono::milliseconds>(now_)
                                 .count());
        }
        events_.erase(events_.begin());
        func();
    }

  private:
    EventQueue events_;
    bool running_ = false;
    milliseconds now_{0};
};


template <bool Interruptible> class TestEventLoop::Sleep {
  public:
    Sleep(TestEventLoop& eventLoop, milliseconds delay)
      : eventLoop_(eventLoop), delay_(delay) {}

    Sleep(Sleep&&) noexcept = default;
    ~Sleep() { CORRAL_ASSERT(!suspended_); }

    auto await_early_cancel() noexcept {
        if constexpr (Interruptible) {
            CORRAL_TRACE("sleep %p (%ld ms) early cancelling", this, delayMS());
            return std::true_type{};
        } else {
            CORRAL_TRACE("sleep %p (%ld ms) NOT early cancelling", this,
                         delayMS());
            return false;
        }
    }
    bool await_ready() const noexcept { return false; }
    void await_suspend(Handle h) {
        CORRAL_TRACE("    ...on sleep %p (%ld ms)", this, delayMS());
        suspended_ = true;
        parent_ = h;
        it_ = eventLoop_.events_.emplace(eventLoop_.now_ + delay_, [this] {
            CORRAL_TRACE("sleep %p (%ld ms) resuming parent", this, delayMS());
            suspended_ = false;
            parent_.resume();
        });
    }
    auto await_cancel(Handle) noexcept {
        if constexpr (Interruptible) {
            CORRAL_TRACE("sleep %p (%ld ms) cancelling", this, delayMS());
            eventLoop_.events_.erase(it_);
            suspended_ = false;
            return std::true_type{};
        } else {
            CORRAL_TRACE("sleep %p (%ld ms) NOT cancelling", this, delayMS());
            return false;
        }
    }
    auto await_must_resume() const noexcept {
        // shouldn't actually be called unless await_cancel() returns false
        CATCH_CHECK(!Interruptible);
        if constexpr (Interruptible) {
            return std::false_type{};
        } else {
            return true;
        }
    }
    void await_resume() { suspended_ = false; }

    void await_introspect(auto& c) const noexcept { c.node("Sleep", this); }

  private:
    long delayMS() const { return static_cast<long>(delay_.count()); }

  private:
    TestEventLoop& eventLoop_;
    milliseconds delay_;
    Handle parent_;
    EventQueue::iterator it_;
    bool suspended_ = false;
};


inline corral::Awaitable<void> auto TestEventLoop::sleep(
        std::chrono::milliseconds tm) {
    return Sleep<true>(*this, tm);
}

inline corral::Awaitable<void> auto TestEventLoop::sleep(
        std::chrono::milliseconds tm, TestEventLoop::UninterruptibleTag) {
    return Sleep<false>(*this, tm);
}

static_assert(corral::detail::Cancellable<TestEventLoop::Sleep<true>>);

static constexpr const TestEventLoop::UninterruptibleTag uninterruptible{};

} // namespace corral::testing


namespace corral {
template <> struct EventLoopTraits<testing::TestEventLoop> {
    static EventLoopID eventLoopID(testing::TestEventLoop& p) noexcept {
        return EventLoopID(&p);
    }
    static void run(testing::TestEventLoop& p) { p.run(); }
    static void stop(testing::TestEventLoop& p) noexcept { p.stop(); }
    static bool isRunning(testing::TestEventLoop& p) noexcept {
        return p.isRunning();
    }
};
} // namespace corral
