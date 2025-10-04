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

#include <cstring>

#include "../corral/corral.h"
#include "TestEventLoop.h"
#include "helpers.h"

using namespace corral;
using namespace corral::testing;
using namespace std::chrono_literals;
using corral::detail::ScopeGuard;

// A small makeshift equivalent for `std::expected<T, int /*errno*/>`.
struct MyError {
    int err;
    bool operator==(const MyError& rhs) const = default;
    friend std::ostream& operator<<(std::ostream& os, const MyError& e) {
        return os << "ERR#" << e.err;
    }
};
template <class T = void> class MyResult;
template <class T> class MyResult {
  public:
    explicit(false) MyResult(T t) : held_(std::forward<T>(t)) {}
    explicit(false) MyResult(MyError e) : held_(e) {}
    explicit operator bool() const { return held_.index() == 0; }
    MyError error() const { return std::get<1>(held_); }
    bool operator==(const MyResult& other) const = default;
    T& operator*() & {
        CATCH_REQUIRE(!!*this);
        return std::get<0>(held_);
    }
    T&& operator*() && {
        CATCH_REQUIRE(!!*this);
        return std::get<0>(std::move(held_));
    }

  private:
    std::variant<T, MyError> held_;
};
template <> class MyResult<void> {
  public:
    constexpr MyResult() : err_(0) {}
    explicit(false) MyResult(MyError e) : err_(e.err) {}
    explicit operator bool() const { return err_ == 0; }
    MyError error() const { return MyError{err_}; }
    bool operator==(const MyResult& other) const = default;
    void operator*() const { CATCH_CHECK(!!*this); }

  private:
    int err_;
};
static constexpr const MyResult<void> OK = {};

// An error policy winding MyError/MyResult to corral.
struct MyErrorPolicy {
    using ErrorType = MyError;
    static MyError fromCurrentException() { CORRAL_ASSERT_UNREACHABLE(); }
    static MyError unwrapError(const auto& v) {
        return v ? MyError{} : v.error();
    }
    static bool hasError(MyError e) { return e.err != 0; }
    template <class T> static auto unwrapValue(T v) { return *v; }
    template <class T> static MyResult<T> wrapValue(T t) {
        return std::forward<T>(t);
    }
    static MyResult<void> wrapValue() { return OK; }
    static MyError wrapError(MyError e) { return e; }
    static void terminateBy(MyError e) {
        fprintf(stderr, "%s\n", strerror(e.err));
        std::terminate();
    }
};

static_assert(corral::ApplicableErrorPolicy<MyErrorPolicy, MyResult<void>>);
static_assert(corral::ApplicableErrorPolicy<MyErrorPolicy, MyResult<int>>);

using MyNursery = BasicNursery<MyErrorPolicy>;
#define WITH_MY_NURSERY(argname)                                               \
    CORRAL_WITH_BASIC_NURSERY(MyErrorPolicy, argname)

namespace corral {
template <class T> struct UseErrorPolicy<MyResult<T>> {
    using Type = MyErrorPolicy;
};
} // namespace corral

#define CORRAL_TEST_CASE(...)                                                  \
    CORRAL_TEST_CASE_IMPL(TestEventLoop, t, __VA_ARGS__)


//
// Tests
//

namespace {

#if __cpp_exceptions
static_assert(ApplicableErrorPolicy<UseExceptions, void>);
static_assert(ApplicableErrorPolicy<UseExceptions, int>);
#endif
static_assert(ApplicableErrorPolicy<Infallible, void>);
static_assert(ApplicableErrorPolicy<Infallible, int>);


CORRAL_TEST_CASE("errp-nursery-smoke") {
    // Smoke test: Task<MyResult<>> can be submitted into the nursery.
    MyResult<> ret = WITH_MY_NURSERY(n) {
        n.start([]() -> Task<MyResult<>> { co_return OK; });
        // Infallible tasks can also be submitted into a nursery
        // with non-default error policy.
        n.start([]() -> Task<void> { co_return; });
        co_return join;
    };
    CATCH_CHECK(ret);
}

CORRAL_TEST_CASE("errp-nursery-errors") {
    // If the task exits with an error according to the used policy,
    // sibling tasks get cancelled, and the error is propagated
    // further up.
    MyResult<> ret = WITH_MY_NURSERY(n) {
        n.start([&]() -> Task<MyResult<>> {
            co_await t.sleep(2ms);
            co_return MyError{EINVAL};
        });
        n.start([&]() -> Task<MyResult<>> {
            co_await t.sleep(5ms);
            co_return OK;
        });
        co_return join;
    };
    CATCH_CHECK(ret == MyError{EINVAL});
    CATCH_CHECK(t.now() == 2ms);
}

CORRAL_TEST_CASE("errp-open-nursery") {
    // openNursery() works in similar fashion.
    MyNursery* n;
    WITH_MY_NURSERY(n2) {
        co_await n2.start([&](TaskStarted<> started) -> Task<MyResult<>> {
            MyResult<> nret = co_await openNursery(n, std::move(started));
            CATCH_CHECK(nret == MyError{EPROTO});
            co_return OK;
        });

        n->start([]() -> Task<MyResult<>> { co_return MyError{EPROTO}; });
        co_return join;
    };
}

CORRAL_TEST_CASE("errp-nursery-fail-early") {
    // If the task taking TaskStarted<> exits with an error
    // before invoking `started()`, the error is propagated up
    // from `BasicNursery::start()`.
    MyResult<> ret = WITH_MY_NURSERY(n) {
        MyResult<> st = co_await n.start(
                [&](TaskStarted<> started) -> Task<MyResult<>> {
                    co_await t.sleep(1ms);
                    co_return MyError{EPROTO};
                });

        CATCH_CHECK(t.now() == 1ms);
        CATCH_CHECK(st == MyError{EPROTO});
        co_return join;
    };
    CATCH_CHECK(ret);
}

CORRAL_TEST_CASE("errp-nursery-fail-late") {
    // If the task fails after invoking `started()`, the error
    // is propagated up to the nursery parent.
    MyResult<> ret = WITH_MY_NURSERY(n) {
        MyResult<> st = co_await n.start(
                [&](TaskStarted<> started) -> Task<MyResult<>> {
                    co_await t.sleep(1ms);
                    started();
                    co_await t.sleep(1ms);
                    co_return MyError{EPROTO};
                });

        CATCH_CHECK(t.now() == 1ms);
        CATCH_CHECK(st);
        co_return join;
    };
    CATCH_CHECK(t.now() == 2ms);
    CATCH_CHECK(ret == MyError{EPROTO});
}

CORRAL_TEST_CASE("errp-nursery-fail-body") {
    MyResult<> ret = WITH_MY_NURSERY(n) {
        co_await t.sleep(1ms);
        co_return MyError{EPROTO};
    };
    CATCH_CHECK(t.now() == 1ms);
    CATCH_CHECK(ret == MyError{EPROTO});
}


CORRAL_TEST_CASE("errp-anyof-success") {
    MyResult<std::tuple<std::optional<int>, std::optional<int>>> r =
            co_await anyOf(
                    [&]() -> Task<MyResult<int>> {
                        co_await t.sleep(1ms);
                        co_return 42;
                    },
                    [&]() -> Task<MyResult<int>> {
                        co_await t.sleep(2ms);
                        CATCH_CHECK(!"should never reach here");
                        co_return -1;
                    });
    CATCH_REQUIRE(r);
    auto [a, b] = *r;

    CATCH_CHECK(*a == 42);
    CATCH_CHECK(!b);
}

CORRAL_TEST_CASE("errp-anyof-error") {
    auto r = co_await anyOf(
            [&]() -> Task<MyResult<int>> {
                co_await t.sleep(1ms);
                co_return MyError{EINVAL};
            },
            [&]() -> Task<MyResult<int>> {
                co_await t.sleep(2ms);
                co_return 42;
            });
    CATCH_CHECK(r == MyError{EINVAL});
}

CORRAL_TEST_CASE("errp-anyof-range") {
    std::vector<Task<MyResult<int>>> tasks;
    auto body = [&](int d) -> Task<MyResult<int>> {
        co_await t.sleep(std::chrono::milliseconds(d));
        co_return d + 10;
    };
    for (int i = 0; i < 3; i++) {
        tasks.push_back(body(i));
    }
    MyResult<std::vector<std::optional<int>>> res = co_await anyOf(tasks);
    CATCH_REQUIRE(res);
    auto& v = *res;
    CATCH_CHECK(*v[0] == 10);
    CATCH_CHECK(!v[1]);
    CATCH_CHECK(!v[2]);
}

CORRAL_TEST_CASE("errp-allof-success") {
    MyResult<std::tuple<int, int>> r = co_await allOf(
            [&]() -> Task<MyResult<int>> {
                co_await t.sleep(1ms);
                co_return 42;
            },
            [&]() -> Task<MyResult<int>> {
                co_await t.sleep(2ms);
                co_return 43;
            });
    CATCH_REQUIRE(r);
    auto [a, b] = *r;

    CATCH_CHECK(a == 42);
    CATCH_CHECK(b == 43);
}

CORRAL_TEST_CASE("errp-allof-error") {
    auto r = co_await allOf(
            [&]() -> Task<MyResult<int>> {
                co_await t.sleep(1ms);
                co_return MyError{EINVAL};
            },
            [&]() -> Task<MyResult<int>> {
                co_await t.sleep(2ms);
                CATCH_CHECK(!"should never reach here");
                co_return 42;
            });
    CATCH_CHECK(r == MyError{EINVAL});
}

CORRAL_TEST_CASE("co_try") {
    auto makeValue = [](int x) -> MyResult<int> {
        if (x < 0) {
            return MyError{EINVAL};
        }
        return x;
    };

    auto body = [&](int x) -> Task<MyResult<std::string>> {
        int rx = CORRAL_CO_TRY makeValue(x);
        co_return std::to_string(rx);
    };

    MyResult<std::string> res = co_await body(42);
    CATCH_CHECK(res == std::string("42"));
    res = co_await body(-1);
    CATCH_CHECK(res == MyError{EINVAL});

    auto voidBody = [&](auto func) -> Task<MyResult<void>> {
        CORRAL_CO_TRY func();
        co_return OK;
    };
    MyResult<void> vres =
            co_await voidBody([]() -> MyResult<void> { return OK; });
    CATCH_CHECK(vres);
    vres = co_await voidBody(
            []() -> MyResult<void> { return MyError{EINVAL}; });
    CATCH_CHECK(vres == MyError{EINVAL});
}

} // namespace
