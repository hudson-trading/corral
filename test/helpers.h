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

#include <string>

#define CORRAL_TEST_CASE_IMPL(EventLoopType, eventLoopArg, ...)                \
    CORRAL_TEST_CASE_IMPL_1(__COUNTER__, EventLoopType, eventLoopArg,          \
                            __VA_ARGS__)
#define CORRAL_TEST_CASE_IMPL_1(counter, EventLoopType, eventLoopArg, ...)     \
    CORRAL_TEST_CASE_IMPL_2(counter, EventLoopType, eventLoopArg, __VA_ARGS__)
#define CORRAL_TEST_CASE_IMPL_2(counter, EventLoopType, eventLoopArg, ...)     \
    static ::corral::Task<void> test_body_##counter(EventLoopType&);           \
    CATCH_TEST_CASE(__VA_ARGS__) {                                             \
        EventLoopType eventLoop;                                               \
        corral::run(eventLoop, test_body_##counter(eventLoop));                \
    }                                                                          \
    static ::corral::Task<void> test_body_##counter(EventLoopType& eventLoopArg)


namespace corral::testing {

//
// An assortment of helper classes with various properties.
//


/// An immediately ready awaitable, not supporting early cancellation.
struct Ready {
    bool await_early_cancel() noexcept { return false; }
    bool await_ready() const noexcept { return true; }
    void await_suspend(Handle) noexcept { CORRAL_ASSERT_UNREACHABLE(); }
    void await_resume() noexcept {}
    bool await_cancel(Handle) noexcept { return false; }
    bool await_must_resume() const noexcept { return true; }
};

/// An immediately ready awaitable, supporting early cancellation.
struct ReadyCancellable {
    bool await_ready() const noexcept { return true; }
    void await_suspend(Handle) noexcept { CORRAL_ASSERT_UNREACHABLE(); }
    void await_resume() noexcept {}
    auto await_cancel(Handle) noexcept { return std::true_type{}; }
};

struct LValueQualifiedImm {
    bool await_ready() const noexcept { return true; }
    void await_suspend(Handle) noexcept { CORRAL_ASSERT_UNREACHABLE(); }
    int await_resume() & { return 42; }
};

struct RValueQualifiedImm {
    bool await_ready() const noexcept { return true; }
    void await_suspend(Handle) noexcept { CORRAL_ASSERT_UNREACHABLE(); }
    int await_resume() && { return 42; }
};

struct LValueQualified {
    auto operator co_await() & { return RValueQualifiedImm{}; }
};
struct RValueQualified {
    auto operator co_await() && { return RValueQualifiedImm{}; }
};


struct NonmoveableAwaiter : detail::Noncopyable {
    bool await_ready() const noexcept { return true; }
    bool await_suspend(Handle) { return false; }
    int await_resume() { return 42; }
};

struct NonmoveableAwaitable : detail::Noncopyable {
    NonmoveableAwaiter operator co_await() { return {}; }
};


#if __cpp_exceptions
struct ThrowingAwaitable {
    bool await_ready() const noexcept { return false; }
    void await_suspend(corral::Handle) { throw std::runtime_error("test"); }
    void await_resume() noexcept {}
};
#endif


/// An object which tracks its lifetime and can catch use-after-frees.
class LifetimeTracked {
    enum class Status { Good, MovedFrom };
    using Map = std::unordered_map<const LifetimeTracked*, Status>;
    static Map& map() {
        static Map m;
        return m;
    }

    static constexpr const char* literal =
            "a string presumably long enough to inhibit SSO";
    std::string str_ = literal;

  public:
    void assertValid() const {
        auto it = map().find(this);
        CATCH_CHECK(it != map().end());
        CATCH_CHECK(it->second == Status::Good);

        // Give ASAN (or valgrind) a chance to catch a use-after-free
        // and emit a useful message with a nice stack trace
        CATCH_CHECK((str_ == literal));
    }

    LifetimeTracked() { map()[this] = Status::Good; }

    LifetimeTracked(const LifetimeTracked& other) {
        other.assertValid();
        str_ = other.str_;
        map()[this] = Status::Good;
    }

    LifetimeTracked(LifetimeTracked&& other) noexcept {
        other.assertValid();
        str_ = std::move(other.str_);
        map()[this] = Status::Good;
        map()[&other] = Status::MovedFrom;
    }

    ~LifetimeTracked() {
        auto it = map().find(this);
        CATCH_CHECK(it != map().end());
        map().erase(it);
    }

    struct Maker {
        operator LifetimeTracked() const { return LifetimeTracked(); }
    };
};

/// A class which checks that certain calls are made in a certain order.
class StageChecker {
    int stage_ = 0;

  public:
    void require(int expected) {
        CATCH_CHECK(stage_ == expected);
        stage_ = expected + 1;
    }

    [[nodiscard]] auto requireOnExit(int expected) {
        return detail::ScopeGuard([expected, this] { require(expected); });
    }
};


} // namespace corral::testing
