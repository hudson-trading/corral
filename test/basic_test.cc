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

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <optional>
#include <vector>

#include "../corral/corral.h"
#include "config.h"
#include "helpers.h"

using namespace corral;
using namespace std::chrono_literals;
using corral::detail::ScopeGuard;

struct NonCancellableTag {};

using std::chrono::milliseconds;
using EventQueue = std::multimap<milliseconds, std::function<void()>>;

class TestEventLoop {
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

    template <bool Cancellable> class Sleep {
      public:
        Sleep(TestEventLoop& eventLoop, milliseconds delay)
          : eventLoop_(eventLoop), delay_(delay) {}

        Sleep(Sleep&&) noexcept = default;
        ~Sleep() { CORRAL_ASSERT(!suspended_); }

        auto await_early_cancel() noexcept {
            if constexpr (Cancellable) {
                CORRAL_TRACE("sleep %p (%lld ms) early cancelling", this,
                             delay_.count());
                return std::true_type{};
            } else {
                CORRAL_TRACE("sleep %p (%lld ms) NOT early cancelling", this,
                             delay_.count());
                return false;
            }
        }
        bool await_ready() const noexcept { return false; }
        void await_suspend(Handle h) {
            CORRAL_TRACE("    ...on sleep %p (%lld ms)", this, delay_.count());
            suspended_ = true;
            parent_ = h;
            it_ = eventLoop_.events_.emplace(eventLoop_.now_ + delay_, [this] {
                CORRAL_TRACE("sleep %p (%lld ms) resuming parent", this,
                             delay_.count());
                suspended_ = false;
                parent_.resume();
            });
        }
        auto await_cancel(Handle) noexcept {
            if constexpr (Cancellable) {
                CORRAL_TRACE("sleep %p (%lld ms) cancelling", this,
                             delay_.count());
                eventLoop_.events_.erase(it_);
                suspended_ = false;
                return std::true_type{};
            } else {
                CORRAL_TRACE("sleep %p (%lld ms) NOT cancelling", this,
                             delay_.count());
                return false;
            }
        }
        auto await_must_resume() const noexcept {
            // shouldn't actually be called unless await_cancel() returns false
            CATCH_CHECK(!Cancellable);
            if constexpr (Cancellable) {
                return std::false_type{};
            } else {
                return true;
            }
        }
        void await_resume() { suspended_ = false; }

        void await_introspect(auto& c) const noexcept { c.node("Sleep"); }

      private:
        TestEventLoop& eventLoop_;
        milliseconds delay_;
        Handle parent_;
        EventQueue::iterator it_;
        bool suspended_ = false;
    };

    corral::Awaitable<void> auto sleep(milliseconds tm) {
        return Sleep<true>(*this, tm);
    }
    corral::Awaitable<void> auto sleep(milliseconds tm, NonCancellableTag) {
        return Sleep<false>(*this, tm);
    }

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
    milliseconds now_ = 0ms;
};

namespace corral {
template <> struct EventLoopTraits<TestEventLoop> {
    static EventLoopID eventLoopID(TestEventLoop& p) noexcept {
        return EventLoopID(&p);
    }
    static void run(TestEventLoop& p) { p.run(); }
    static void stop(TestEventLoop& p) noexcept { p.stop(); }
    static bool isRunning(TestEventLoop& p) noexcept { return p.isRunning(); }
};
} // namespace corral

static_assert(corral::detail::Cancellable<TestEventLoop::Sleep<true>>);

namespace {

#define CORRAL_TEST_CASE(...)                                                  \
    CORRAL_TEST_CASE_IMPL(TestEventLoop, t, __VA_ARGS__)

const NonCancellableTag noncancellable;

struct Ready {
    bool await_early_cancel() noexcept { return false; }
    bool await_ready() const noexcept { return true; }
    void await_suspend(Handle) noexcept { CORRAL_ASSERT_UNREACHABLE(); }
    void await_resume() noexcept {}
    bool await_cancel(Handle) noexcept { return false; }
    bool await_must_resume() const noexcept { return true; }
};

struct ReadyCancellable {
    bool await_ready() const noexcept { return true; }
    void await_suspend(Handle) noexcept { CORRAL_ASSERT_UNREACHABLE(); }
    void await_resume() noexcept {}
    auto await_cancel(Handle) noexcept { return std::true_type{}; }
};

//
// Tests
//

CORRAL_TEST_CASE("smoke") {
    auto one = [&]() -> Task<int> { co_return 1; };
    auto two = [&]() -> Task<int> { co_return 2; };
    auto three = [&]() -> Task<int> { co_return 3; };

    auto six = [&]() -> Task<int> {
        int x = co_await one;
        int y = co_await two;
        int z = co_await three;
        co_return x + y + z;
    };

    int ret = co_await six;
    CATCH_CHECK(ret == 6);
}

CORRAL_TEST_CASE("task-awaitable-types") {
    CATCH_SECTION("task-object") {
        auto taskLambda = []() -> Task<int> { co_return 42; };
        Task<int> task = taskLambda();
        int x = co_await task;
        CATCH_CHECK(x == 42);
    }

    CATCH_SECTION("task-callable") {
        int x = co_await []() -> Task<int> { co_return 43; };
        CATCH_CHECK(x == 43);
    }
}

CORRAL_TEST_CASE("interleave") {
    std::vector<int> v;
    auto one = [&]() -> Task<> {
        co_await t.sleep(1ms);
        v.push_back(1);
        co_await t.sleep(2ms);
        v.push_back(3);
        co_await t.sleep(2ms);
        v.push_back(5);
    };

    auto two = [&]() -> Task<> {
        co_await t.sleep(2ms);
        v.push_back(2);
        co_await t.sleep(2ms);
        v.push_back(4);
        co_await t.sleep(2ms);
        v.push_back(6);
    };

    co_await allOf(one, two);
    CATCH_CHECK(v == (std::vector<int>{1, 2, 3, 4, 5, 6}));
}

CORRAL_TEST_CASE("anyof") {
    CATCH_SECTION("smoke") {
        auto [a, b, c] =
                co_await anyOf(t.sleep(2ms), t.sleep(3ms),
                               [&]() -> Task<> { co_await t.sleep(5ms); });
        CATCH_CHECK(a);
        CATCH_CHECK(!b);
        CATCH_CHECK(!c);
        CATCH_CHECK(t.now() == 2ms);
    }

    CATCH_SECTION("immediate-front") {
        co_await anyOf([&]() -> Task<> { co_return; },
                       [&]() -> Task<> { co_await t.sleep(1ms); });
        CATCH_CHECK(t.now() == 0ms);
    }

    CATCH_SECTION("immediate-back") {
        co_await anyOf([&]() -> Task<> { co_await t.sleep(1ms); },
                       [&]() -> Task<> { co_return; });
        CATCH_CHECK(t.now() == 0ms);
    }

    CATCH_SECTION("immediate-both") {
        auto [a, b] = co_await anyOf(Ready{}, Ready{});
        CATCH_CHECK(t.now() == 0ms);
        CATCH_CHECK(a);
        CATCH_CHECK(b);

        std::tie(a, b) = co_await anyOf(ReadyCancellable{}, ReadyCancellable{});
        CATCH_CHECK(t.now() == 0ms);
        CATCH_CHECK(a);
        CATCH_CHECK(!b);
    }

    CATCH_SECTION("empty") {
        auto r = co_await anyOf();
        static_assert(std::tuple_size_v<decltype(r)> == 0);
    }

    CATCH_SECTION("non-cancellable") {
        auto [a, b, c] = co_await anyOf(
                t.sleep(2ms), t.sleep(3ms, noncancellable), t.sleep(5ms));
        CATCH_CHECK(t.now() == 3ms);
        CATCH_CHECK(a);
        CATCH_CHECK(b);
        CATCH_CHECK(!c);
    }

    CATCH_SECTION("return-ref") {
        int x = 42;
        auto [lx, s1] = co_await anyOf([&]() -> Task<int&> { co_return x; },
                                       t.sleep(2ms));
        CATCH_CHECK(&*lx == &x);
        CATCH_CHECK(!s1);

        auto [rx, s2] = co_await anyOf(
                [&]() -> Task<int&&> { co_return std::move(x); }, t.sleep(2ms));
        CATCH_CHECK(&*rx == &x);
        CATCH_CHECK(!s2);
    }
}

CORRAL_TEST_CASE("mostof") {
    CATCH_SECTION("smoke") {
        co_await mostOf(t.sleep(2ms), t.sleep(3ms),
                        [&]() -> Task<> { co_await t.sleep(5ms); });
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("empty") {
        auto r = co_await mostOf();
        static_assert(std::tuple_size_v<decltype(r)> == 0);
    }

    CATCH_SECTION("retval") {
        auto [a, b] = co_await mostOf([]() -> Task<int> { co_return 42; },
                                      []() -> Task<int> { co_return 43; });
        CATCH_CHECK(*a == 42);
        CATCH_CHECK(*b == 43);
    }

    CATCH_SECTION("noncancellable") {
        bool resumed = false;
        auto sub = [&]() -> Task<> {
            auto [a, b, c] =
                    co_await mostOf([&]() -> Task<int> { co_return 42; },
                                    t.sleep(3ms, noncancellable), t.sleep(5ms));
            CATCH_CHECK(a);
            CATCH_CHECK(*a == 42);
            CATCH_CHECK(b);
            CATCH_CHECK(!c);
            resumed = true;
        };
        co_await anyOf(sub(), t.sleep(1ms));
        CATCH_CHECK(t.now() == 3ms);
        CATCH_CHECK(resumed);
    }

    CATCH_SECTION("throws") {
        bool cancelled = false;
        CATCH_CHECK_THROWS(co_await mostOf(
                [&]() -> Task<> {
                    ScopeGuard guard([&] { cancelled = true; });
                    co_await SuspendForever{};
                },
                [&]() -> Task<> {
                    co_await t.sleep(1ms);
                    throw std::runtime_error("boo!");
                }));
        CATCH_CHECK(cancelled);
    }
}

CORRAL_TEST_CASE("allof") {
    CATCH_SECTION("smoke") {
        co_await allOf(t.sleep(2ms), t.sleep(3ms),
                       [&]() -> Task<> { co_await t.sleep(5ms); });
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("empty") {
        auto r = co_await allOf();
        static_assert(std::tuple_size_v<decltype(r)> == 0);
    }

    CATCH_SECTION("retval") {
        auto [a, b] = co_await allOf([]() -> Task<int> { co_return 42; },
                                     []() -> Task<int> { co_return 43; });
        CATCH_CHECK(a == 42);
        CATCH_CHECK(b == 43);
    }

    CATCH_SECTION("throws") {
        bool cancelled = false;
        CATCH_CHECK_THROWS(co_await allOf(
                [&]() -> Task<> {
                    ScopeGuard guard([&] { cancelled = true; });
                    co_await SuspendForever{};
                },
                [&]() -> Task<> {
                    co_await t.sleep(1ms);
                    throw std::runtime_error("boo!");
                }));
        CATCH_CHECK(cancelled);
    }
}

CORRAL_TEST_CASE("anyof-allof-mix") {
    auto five = [&]() -> Task<> { co_await t.sleep(5ms); };
    co_await allOf(
            t.sleep(2ms), [&]() -> Task<> { co_await t.sleep(3ms); },
            anyOf(five, t.sleep(6ms)));
    CATCH_CHECK(t.now() == 5ms);
}

CORRAL_TEST_CASE("mux-range") {
    auto task = [&t](int x) -> Task<int> {
        co_await t.sleep(std::chrono::milliseconds(x));
        co_return x * 100;
    };

    std::vector<Task<int>> v;
    v.push_back(task(3));
    v.push_back(task(2));
    v.push_back(task(5));

    CATCH_SECTION("anyof") {
        auto ret = co_await anyOf(v);
        CATCH_CHECK(t.now() == 2ms);
        CATCH_CHECK(ret.size() == 3);
        CATCH_CHECK(!ret[0]);
        CATCH_CHECK(*ret[1] == 200);
        CATCH_CHECK(!ret[2]);
    }

    CATCH_SECTION("allof") {
        auto ret = co_await allOf(v);
        CATCH_CHECK(t.now() == 5ms);
        CATCH_CHECK(ret[0] == 300);
        CATCH_CHECK(ret[1] == 200);
        CATCH_CHECK(ret[2] == 500);
    }

    CATCH_SECTION("anyof-immediate") {
        auto imm = []() -> Task<int> { co_return 0; };
        v.push_back(imm());
        co_await anyOf(v);
    }

    CATCH_SECTION("anyof-empty") {
        v.clear();
        co_await anyOf(v);
    }

    CATCH_SECTION("allof-empty") {
        v.clear();
        co_await allOf(v);
    }
}

CORRAL_TEST_CASE("event") {
    Event evt;
    auto task = [&]() -> Task<> {
        co_await evt;
        CATCH_CHECK(t.now() == 2ms);
        co_await evt;
        CATCH_CHECK(t.now() == 2ms);
    };
    auto trigger = [&]() -> Task<> {
        co_await t.sleep(2ms);
        evt.trigger();
    };
    co_await allOf(task, trigger);
}

CORRAL_TEST_CASE("basic-cancel") {
    bool started = false;
    auto task = [&]() -> Task<> {
        co_await t.sleep(1ms);
        started = true;
        co_await t.sleep(2ms);
        CATCH_CHECK(false);
    };

    co_await anyOf(task, t.sleep(2ms));
    CATCH_CHECK(started);

    co_await t.sleep(5ms);
}

CORRAL_TEST_CASE("self-cancel") {
    bool started = false;
    Event cancelEvt;
    auto outer = [&]() -> Task<> {
        auto work = [&]() -> Task<> {
            auto interrupt = [&]() -> Task<> {
                started = true;
                co_await t.sleep(1ms);
                cancelEvt.trigger();
            };
            co_await interrupt;
            co_await yield;
            CATCH_CHECK(false);
        };
        co_await anyOf(work, cancelEvt);
    };
    co_await outer;
    CATCH_CHECK(started);
}

CORRAL_TEST_CASE("no-cancel") {
    bool resumed = false;
    auto task = [&]() -> Task<> {
        co_await t.sleep(5ms, noncancellable);

        CATCH_SECTION("next-cancellable") {
            resumed = true;
            co_await yield;
            CATCH_CHECK(!"should not reach here");
        }

        CATCH_SECTION("next-noncancellable") {
            co_await t.sleep(0ms, noncancellable);
            resumed = true;
        }

        CATCH_SECTION("next-task") {
            co_await []() -> Task<> { co_return; };
            resumed = true;
        }

        CATCH_SECTION("next-nontrivial") {
            co_await anyOf(
                    [&]() -> Task<> {
                        co_await yield;
                        CATCH_CHECK(!"should not reach here");
                        co_return;
                    },
                    untilCancelledAnd([&]() -> Task<> {
                        resumed = true;
                        co_return;
                    }));
        }
    };
    auto task2 = [&]() -> Task<> {
        co_await anyOf(task, t.sleep(2ms));
        CATCH_CHECK(t.now() == 5ms);
    };

    co_await task2;
    CATCH_CHECK(t.now() == 5ms);
    CATCH_CHECK(resumed);
}

CORRAL_TEST_CASE("no-cancel-exc") {
    CATCH_SECTION("nested-task") {
        CATCH_CHECK_THROWS(co_await [&]() -> Task<> {
            co_await anyOf(t.sleep(1ms), [&]() -> Task<> {
                co_await t.sleep(2ms, noncancellable);
                co_await [&]() -> Task<> {
                    co_await t.sleep(1ms, noncancellable);
                    throw std::runtime_error("boo!");
                };
            });
        });
    }

    CATCH_SECTION("all-of") {
        CATCH_CHECK_THROWS(co_await [&]() -> Task<> {
            co_await anyOf(t.sleep(1ms), [&]() -> Task<> {
                co_await t.sleep(2ms, noncancellable);
                co_await allOf(t.sleep(1ms), [&]() -> Task<> {
                    co_await t.sleep(1ms, noncancellable);
                    throw std::runtime_error("boo!");
                });
            });
        });
    }
}

CORRAL_TEST_CASE("mux-cancel") {
    auto task = [&]() -> Task<> { co_await t.sleep(5ms); };
    std::vector<Task<>> tv;
    tv.push_back(task());
    tv.push_back(task());

    CATCH_SECTION("any-of") {
        co_await anyOf([&]() -> Task<> { co_await anyOf(task(), task()); },
                       t.sleep(1ms));
    }
    CATCH_SECTION("all-of") {
        co_await anyOf([&]() -> Task<> { co_await allOf(task(), task()); },
                       t.sleep(1ms));
    }
    CATCH_SECTION("any-of-range") {
        co_await anyOf([&]() -> Task<> { co_await anyOf(tv); }, t.sleep(1ms));
    }
    CATCH_SECTION("all-of-range") {
        co_await anyOf([&]() -> Task<> { co_await allOf(tv); }, t.sleep(1ms));
    }
}

CORRAL_TEST_CASE("mux-cancel-done") {
    auto noop = []() -> Task<> { co_return; };
    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> {
            co_await allOf(SuspendForever{}, SuspendForever{}, noop());
        });
        co_await t.sleep(1ms);
        co_return cancel;
    };
}

CORRAL_TEST_CASE("return-types") {
    int x = 42;
    const int cx = 43;

    auto refTask = [&]() -> Task<int&> { co_return x; };
    int& ref = co_await refTask();
    CATCH_CHECK(&ref == &x);

    auto crefTask = [&]() -> Task<const int&> { co_return cx; };
    const int& cref = co_await crefTask();
    CATCH_CHECK(&cref == &cx);

    auto rrefTask = [&]() -> Task<int&&> { co_return std::move(x); };
    int&& rref = co_await rrefTask();
    CATCH_CHECK(&rref == &x);
}

CORRAL_TEST_CASE("cb") {
    auto schedule = [&](std::function<void(int, int)> cb) {
        t.schedule(1ms, [cb] { cb(1, 2); });
        t.schedule(2ms, [cb] { cb(3, 4); });
        t.schedule(3ms, [cb] { cb(5, 6); });
    };

    CBPortal<int, int> aw;
    int msgs = 0;
    auto consumer = [&]() -> Task<> {
        auto [x, y] = co_await untilCBCalled(schedule, aw);
        ++msgs;
        CATCH_CHECK(x == 1);
        CATCH_CHECK(y == 2);

        std::tie(x, y) = co_await aw;
        ++msgs;
        CATCH_CHECK(x == 3);
        CATCH_CHECK(y == 4);

        std::tie(x, y) = co_await aw;
        ++msgs;
        CATCH_CHECK(x == 5);
        CATCH_CHECK(y == 6);

        std::tie(x, y) = co_await aw;
        CATCH_CHECK(false);
    };

    co_await anyOf(consumer, t.sleep(5ms));
    CATCH_CHECK(msgs == 3);
}

CORRAL_TEST_CASE("cb-simplified") {
    CATCH_SECTION("immediate") {
        auto [i, s] = co_await untilCBCalled(
                [](std::function<void(int, std::string)> cb) {
                    cb(42, "forty-two");
                });
        CATCH_CHECK(i == 42);
        CATCH_CHECK(s == "forty-two");
    }

    CATCH_SECTION("delayed") {
        CORRAL_WITH_NURSERY(nursery) {
            int x = co_await untilCBCalled([&](std::function<void(int)> cb) {
                nursery.start([&, cb]() -> Task<> {
                    co_await t.sleep(2ms);
                    cb(42);
                });
            });
            CATCH_CHECK(t.now() == 2ms);
            CATCH_CHECK(x == 42);
            co_return cancel;
        };
    }

    CATCH_SECTION("multiple") {
        auto [i, s] = co_await untilCBCalled(
                [](std::function<void(int)> cbi,
                   std::function<void(std::string)> cbs) { cbs("hello"); });
        CATCH_CHECK(!i);
        CATCH_CHECK(*s == "hello");
    }
}

CORRAL_TEST_CASE("cb-cstyle") {
    CORRAL_WITH_NURSERY(nursery) {
        CBPortal<int> cbp;
        int x = co_await untilCBCalled(
                [&]<class CB>(CB& cb) {
                    void (*ccb)(void*, int) = +[](void* obj, int xx) {
                        (*reinterpret_cast<CB*>(obj))(xx);
                    };
                    void* ccbArg = &cb;

                    nursery.start([&, ccb, ccbArg]() -> Task<> {
                        co_await t.sleep(2ms);
                        (*ccb)(ccbArg, 42);
                    });
                },
                cbp);
        CATCH_CHECK(t.now() == 2ms);
        CATCH_CHECK(x == 42);
        co_return cancel;
    };
};

CORRAL_TEST_CASE("cb-anyof") {
    auto schedule = [&](std::function<void()> voidcb,
                        std::function<void(std::string)> strcb) {
        t.schedule(1ms, [voidcb] { voidcb(); });
        t.schedule(2ms, [strcb] { strcb("one"); });
        t.schedule(3ms, [voidcb] { voidcb(); });
        t.schedule(4ms, [strcb] { strcb("two"); });
    };

    CBPortal<> aw1;
    CBPortal<std::string> aw2;

    auto [v, s] = co_await untilCBCalled(schedule, aw1, aw2);
    CATCH_CHECK(v);
    CATCH_CHECK(!s);

    std::tie(v, s) = co_await anyOf(aw1, aw2);
    CATCH_CHECK(!v);
    CATCH_CHECK(*s == "one");

    std::tie(v, s) = co_await anyOf(aw1, aw2);
    CATCH_CHECK(v);
    CATCH_CHECK(!s);

    std::tie(v, s) = co_await anyOf(aw1, aw2);
    CATCH_CHECK(!v);
    CATCH_CHECK(*s == "two");
}

CORRAL_TEST_CASE("cb-immediate") {
    CBPortal<int> aw;
    auto func = [](std::function<void(int)> cb) {
        cb(1);
        cb(2);
        cb(3);
    };

    int x = co_await untilCBCalled(func, aw);
    CATCH_CHECK(x == 1);

    x = co_await aw;
    CATCH_CHECK(x == 2);

    x = co_await aw;
    CATCH_CHECK(x == 3);
}

CORRAL_TEST_CASE("cb-return-type") {
    auto val = [](std::function<void(int)> cb) { cb(42); };
    CBPortal<int> valCB;
    int x = co_await untilCBCalled(val, valCB);
    CATCH_CHECK(x == 42);

    auto lref = [&x](std::function<void(int&)> cb) { cb(x); };
    CBPortal<int&> lrefCB;
    int& lr = co_await untilCBCalled(lref, lrefCB);
    CATCH_CHECK(&lr == &x);

    auto cref = [&x](std::function<void(const int&)> cb) { cb(x); };
    CBPortal<const int&> crefCB;
    const int& cr = co_await untilCBCalled(cref, crefCB);
    CATCH_CHECK(&cr == &x);

    auto rref = [&x](std::function<void(int&&)> cb) { cb(std::move(x)); };
    CBPortal<int&&> rrefCB;
    int&& rr = co_await untilCBCalled(rref, rrefCB);
    CATCH_CHECK(&rr == &x);
}

template <class T> class MyLightweightFn;
template <class... Args> class MyLightweightFn<void(Args...)> {
    void (*fn_)(size_t, Args...);
    size_t obj_;

  public:
    template <class Fn> MyLightweightFn(Fn fn) {
        static_assert(std::is_trivially_copyable_v<Fn>);
        static_assert(std::is_trivially_destructible_v<Fn>);
        static_assert(sizeof(Fn) == sizeof(obj_));
        memcpy(&obj_, &fn, sizeof(Fn));
        fn_ = +[](size_t obj, Args... args) {
            (*reinterpret_cast<Fn*>(&obj))(args...);
        };
    }

    void operator()(Args... args) { return fn_(obj_, args...); }
};

CORRAL_TEST_CASE("cb-lightweight") {
    CATCH_SECTION("simplified") {
        auto [i, s] = co_await untilCBCalled(
                [](MyLightweightFn<void(int, std::string)> cb) {
                    cb(42, "forty-two");
                });
        CATCH_CHECK(i == 42);
        CATCH_CHECK(s == "forty-two");
    }

    CATCH_SECTION("explicit") {
        CBPortal<int, std::string> cbp;
        auto [i, s] = co_await untilCBCalled(
                [](MyLightweightFn<void(int, std::string)> cb) {
                    cb(42, "forty-two");
                },
                cbp);
        CATCH_CHECK(i == 42);
        CATCH_CHECK(s == "forty-two");
    }
}

CORRAL_TEST_CASE("nursery") {
    size_t count = 0;
    auto incrementAfter = [&](milliseconds delay) -> Task<void> {
        co_await t.sleep(delay);
        ++count;
    };
    CORRAL_WITH_NURSERY(n) {
        n.start(incrementAfter(2ms));
        n.start(incrementAfter(3ms));
        n.start(incrementAfter(5ms));

        co_await t.sleep(4ms);
        CATCH_CHECK(count == 2);

        co_await t.sleep(2ms);
        CATCH_CHECK(count == 3);

        co_return join;
    };
}

CORRAL_TEST_CASE("nursery-ret-policy") {
    auto sleep = [&](milliseconds delay) -> Task<void> {
        co_await t.sleep(delay);
    };

    CATCH_SECTION("join") {
        CORRAL_WITH_NURSERY(n) {
            n.start(sleep(5ms));
            co_return join;
        };
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("cancel") {
        CORRAL_WITH_NURSERY(n) {
            n.start(sleep(5ms));
            co_return cancel;
        };
        CATCH_CHECK(t.now() == 0ms);
    }

    CATCH_SECTION("suspend_forever") {
        co_await anyOf(sleep(5ms), [&]() -> Task<void> {
            CORRAL_WITH_NURSERY(n) {
                co_await SuspendForever{};
                co_return join;
            };
        });
        CATCH_CHECK(t.now() == 5ms);
    }
}

CORRAL_TEST_CASE("nursery-cancel-enqueue") {
    bool started = false;
    CORRAL_WITH_NURSERY(nursery) {
        ScopeGuard guard([&] {
            nursery.start([&]() -> Task<> {
                started = true;
                co_await corral::yield;
                CATCH_CHECK(!"should never reach here");
                co_return;
            });
        });
        co_return cancel;
    };
    CATCH_CHECK(started);
}

CORRAL_TEST_CASE("nursery-sync-cancel") {
    bool cancelled = false;
    CORRAL_WITH_NURSERY(n) {
        n.start([&]() -> Task<> {
            ScopeGuard guard([&] { cancelled = true; });
            co_await t.sleep(5ms);
        });
        co_await t.sleep(1ms);
        CATCH_CHECK(!cancelled);
        n.cancel();
        CATCH_CHECK(!cancelled);
        co_await yield;
        CATCH_CHECK(!"should not reach here");
        co_return cancel;
    };
    CATCH_CHECK(cancelled);
}

CORRAL_TEST_CASE("nursery-multi-cancel") {
    auto task = [&](Nursery& n) -> Task<> {
        co_await t.sleep(1ms, noncancellable);
        n.cancel();
    };
    CORRAL_WITH_NURSERY(n) {
        n.start(task(n));
        n.start(task(n));
        n.start(task(n));
        co_return join;
    };
}

CORRAL_TEST_CASE("nursery-multi-exception") {
    auto task = [&](Nursery& n) -> Task<> {
        co_await t.sleep(1ms, noncancellable);
        throw std::runtime_error("boo!");
    };
    try {
        CORRAL_WITH_NURSERY(n) {
            n.start(task(n));
            n.start(task(n));
            n.start(task(n));
            co_return join;
        };
    } catch (std::runtime_error&) {}
}

CORRAL_TEST_CASE("nursery-cancel-exception") {
    CATCH_CHECK_THROWS_WITH(
            CORRAL_WITH_NURSERY(n) {
                n.start([&]() -> Task<> {
                    co_await t.sleep(2ms, noncancellable);
                    throw std::runtime_error("boo!");
                });
                n.start([&]() -> Task<> {
                    co_await t.sleep(3ms, noncancellable);
                });
                co_await t.sleep(1ms);
                co_return cancel;
            },
            Catch::Equals("boo!"));
    CATCH_CHECK(t.now() == 3ms);
}

CORRAL_TEST_CASE("exceptions") {
    auto bad = [&]() -> Task<> {
        co_await std::suspend_never();
        throw std::runtime_error("boo!");
    };
    CATCH_CHECK_THROWS_WITH(co_await bad, Catch::Equals("boo!"));
}

CORRAL_TEST_CASE("nursery-exception") {
    auto t1 = [&]() -> Task<> { co_await t.sleep(2ms); };
    auto t2 = [&]() -> Task<> {
        co_await std::suspend_never();
        throw std::runtime_error("boo!");
    };

    CATCH_CHECK_THROWS_WITH(
            CORRAL_WITH_NURSERY(n) {
                n.start(t1());
                n.start(t2());
                co_return join;
            },
            Catch::Equals("boo!"));
    CATCH_CHECK(t.now() == 0ms);
}

CORRAL_TEST_CASE("shared") {
    auto shared = Shared([&]() -> Task<int> {
        co_await t.sleep(5ms);
        co_return 42;
    });

    auto use = [&](milliseconds delay = 0ms) -> Task<int> {
        if (delay != 0ms) {
            co_await t.sleep(delay);
        }
        int ret = co_await shared;
        CATCH_CHECK(t.now() == 5ms);
        CATCH_CHECK(ret == 42);
        co_return std::move(ret);
    };

    CATCH_SECTION("smoke") {
        auto [x, y] = co_await allOf(use(), use(1ms));
        CATCH_CHECK(x == 42);
        CATCH_CHECK(y == 42);
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("cancellation") {
        co_await anyOf(allOf(use(), use(1ms)), t.sleep(3ms));
        CATCH_CHECK(t.now() == 3ms);
    }
}

CORRAL_TEST_CASE("shared-no-cancel") {
    auto shared = Shared([&]() -> Task<> {
        ScopeGuard check([&] { CATCH_CHECK(t.now() == 5ms); });
        co_await t.sleep(10ms);
    });

    auto first = [&]() -> Task<> {
        co_await anyOf(shared, t.sleep(2ms));
        CATCH_CHECK(t.now() == 2ms);
    };

    auto second = [&]() -> Task<> {
        co_await t.sleep(1ms);
        co_await anyOf(shared, t.sleep(4ms));
        CATCH_CHECK(t.now() == 5ms);
    };

    co_await allOf(first, second);
}

CORRAL_TEST_CASE("shared-cancel-self") {
    auto shared = Shared([&]() -> Task<> { co_await t.sleep(5ms); });
    auto parent = [&]() -> Task<void> { co_await shared; };
    co_await anyOf(parent(), parent());
}

CORRAL_TEST_CASE("semaphores") {
    Semaphore sem(5);
    int concurrency = 0;
    auto worker = [&]() -> Task<> {
        auto lk = co_await sem.lock();
        ++concurrency;
        CATCH_CHECK(concurrency <= 5);
        co_await t.sleep(1ms);
        --concurrency;
    };
    CORRAL_WITH_NURSERY(nursery) {
        for (int i = 0; i != 20; ++i) {
            nursery.start(worker());
        }
        co_return join;
    };
    CATCH_CHECK(t.now() == 4ms);
}

CORRAL_TEST_CASE("value") {
    Value<int> v{0};

    co_await allOf(
            [&]() -> Task<> {
                co_await v.untilEquals(0);
                CATCH_CHECK(t.now() == 0ms);

                auto [from, to] = co_await v.untilChanged();
                CATCH_CHECK(t.now() == 1ms);
                CATCH_CHECK(from == 0);
                CATCH_CHECK(to == 3);

                std::tie(from, to) = co_await v.untilChanged();
                // Should skip the 3->3 change
                CATCH_CHECK(t.now() == 3ms);
                CATCH_CHECK(from == 3);
                CATCH_CHECK(to == 4);

                to = co_await v.untilEquals(5);
                CATCH_CHECK(t.now() == 4ms);
                CATCH_CHECK(to == 5);
                CATCH_CHECK(v == 7);
            },
            [&]() -> Task<> {
                co_await t.sleep(1ms);
                v = 3;

                co_await t.sleep(1ms);
                v = 3;
                co_await t.sleep(1ms);
                v = 4;

                co_await t.sleep(1ms);
                v = 7;
                v = 5;
                v = 7;
            });

    CATCH_CHECK(t.now() == 4ms);
}

CORRAL_TEST_CASE("noop") {
    co_await noop();
    co_await anyOf(noop(), noop());
}

CORRAL_TEST_CASE("executor") {
    bool innerInvoked = false;
    Event evt;
    auto inner = [&]() -> Task<> {
        CORRAL_TRACE("entering inner(), waiting on event");
        co_await evt;
        CORRAL_TRACE("inner() resumed by event");
        innerInvoked = true;
        co_return;
    };

    CORRAL_WITH_NURSERY(n) {
        n.start(inner());

        evt.trigger();
        CATCH_CHECK(!innerInvoked);

        co_await yield;
        CATCH_CHECK(innerInvoked);

        co_return join;
    };
}

CORRAL_TEST_CASE("chained-executors") {
    Event evt, evt2;
    UnsafeNursery n(t);

    CATCH_SECTION("timeout") {
        n.start([&]() -> Task<> {
            co_await t.sleep(1ms);
            evt.trigger();
        });
        co_await evt;
    }

    CATCH_SECTION("event") {
        n.start([&]() -> Task<> {
            co_await evt2;
            evt.trigger();
        });
        evt2.trigger();
        co_await evt;
    }
}

CORRAL_TEST_CASE("multiverse") {
    co_await t.sleep(2ms);

    TestEventLoop t2;

    CATCH_SECTION("run") {
        run(t2, [&t2]() -> Task<void> { co_await t2.sleep(3ms); });
    }
    CATCH_SECTION("unsafe-nursery") {
        UnsafeNursery n(t2);
        n.start([&t2]() -> Task<void> {
            co_await t2.sleep(3ms);
            t2.stop();
        });
        t2.run();
    }

    co_await t.sleep(4ms);

    CATCH_CHECK(t.now() == 6ms);
    CATCH_CHECK(t2.now() == 3ms);
}

CORRAL_TEST_CASE("run-on-cancel") {
    co_await anyOf(t.sleep(2ms), untilCancelledAnd(t.sleep(1ms)));
    CATCH_CHECK(t.now() == 3ms);
}

CORRAL_TEST_CASE("task-tree") {
    std::vector<TreeDumpElement> tree;
    CORRAL_WITH_NURSERY(n) {
        co_await allOf(allOf(t.sleep(1ms), t.sleep(3ms)), [&]() -> Task<> {
            co_await t.sleep(2ms);
            co_await dumpTaskTree(std::back_inserter(tree));
        });
        co_return join;
    };

    std::vector<std::pair<int, std::string>> items;
    for (auto& elem : tree) {
        items.emplace_back(elem.depth,
                           std::holds_alternative<uintptr_t>(elem.value)
                                   ? "<TASK>"
                                   : std::get<const char*>(elem.value));
    }
    CATCH_CHECK(items == std::vector<std::pair<int, std::string>>{
                                 {0, "<TASK>"},
                                 {1, "Nursery"},
                                 {2, "<TASK>"},
                                 {3, "AllOf"},
                                 {4, "AllOf"},
                                 {5, "Sleep"},
                                 {4, "<TASK>"},
                                 {5, "<YOU ARE HERE>"}});
}

using RawStack = std::vector<uintptr_t>;

Task<void> stackTraceHelper(int depth, const RawStack& caller) {
    RawStack stack0;
    Executor::collectAsyncStackTrace(std::back_inserter(stack0));

    // The caller should have one less stack frame than the callee.
    CATCH_REQUIRE(stack0.size() == caller.size() + 1);

    // Recall:
    //
    // stack[0]  = callee frame
    // stack[1]  = caller frame
    // caller[0] = caller frame
    //
    // While stack[1] and caller[0] both correspond to the caller frame, they
    // should have different return addresses because caller[0] was captured on
    // a different line of code than the call to this function.
    CATCH_CHECK(RawStack(caller.begin() + 1, caller.end()) ==
                RawStack(stack0.begin() + 2, stack0.end()));
    CATCH_CHECK(stack0[1] != 0);
    CATCH_CHECK(stack0[0] != 0);
    CATCH_CHECK(caller[0] != 0);
    CATCH_CHECK(caller[0] != stack0[1]);

    // Introduce a co_await expression. The program counter in the top (this)
    // coroutine frame should reflect this co_await.
    RawStack stack1;
    co_await AsyncStackTrace(std::back_inserter(stack1));
    CATCH_CHECK(RawStack(stack0.begin() + 1, stack0.end()) ==
                RawStack(stack1.begin() + 1, stack1.end()));
    CATCH_CHECK(stack1[0] != 0);
    CATCH_CHECK(stack1[0] != stack0[0]);

    // Grab an async stack trace again. The async stack should be identical to
    // the one we got above because the program counter is updated only on a
    // co_await.
    RawStack stack2;
    Executor::collectAsyncStackTrace(std::back_inserter(stack2));
    CATCH_CHECK(stack2 == stack1);

    if (depth <= 0) {
        co_return;
    }

    co_await stackTraceHelper(depth - 1, stack0);
}

CORRAL_TEST_CASE("async-stack-trace") {
    if (!detail::frame_tags::enabled()) {
        co_return;
    }

    RawStack stack0;
    co_await AsyncStackTrace(std::back_inserter(stack0));
    CATCH_REQUIRE(stack0.size() == 2);
    CATCH_CHECK(stack0[0] != 0);
    CATCH_CHECK(stack0[1] != 0);

    co_await stackTraceHelper(0, stack0);
    co_await stackTraceHelper(1, stack0);
    co_await stackTraceHelper(2, stack0);
    co_await anyOf(stackTraceHelper(0, stack0), stackTraceHelper(1, stack0),
                   stackTraceHelper(2, stack0));
    co_await allOf(stackTraceHelper(0, stack0), stackTraceHelper(1, stack0),
                   stackTraceHelper(2, stack0));

    RawStack stack1;
    co_await AsyncStackTrace(std::back_inserter(stack1));
    CATCH_REQUIRE(stack1.size() == 2);
    CATCH_CHECK(stack1[0] != 0);
    CATCH_CHECK(stack1[0] != stack0[0]);
    CATCH_CHECK(stack1[1] == stack0[1]);
}

CORRAL_TEST_CASE("frames") {
    if (!detail::frame_tags::enabled()) {
        co_return;
    }

    using detail::CoroutineFrame;
    using detail::frameCast;
    using detail::ProxyFrame;
    using detail::TaskFrame;

    CoroutineFrame f1;
    CATCH_CHECK(frameCast<ProxyFrame>(&f1) == nullptr);
    CATCH_CHECK(frameCast<TaskFrame>(&f1) == nullptr);

    ProxyFrame f2;
    CoroutineFrame* f3 = &f2;
    CATCH_CHECK(frameCast<ProxyFrame>(f3) == &f2);
    CATCH_CHECK(frameCast<TaskFrame>(f3) == nullptr);
    CATCH_CHECK(f2.followLink() == nullptr);

    f2.linkTo(f1.toHandle());
    CATCH_CHECK(f2.followLink() == f1.toHandle());

    TaskFrame f4;
    CoroutineFrame* f5 = &f4;
    ProxyFrame* f6 = &f4;
    CATCH_CHECK(frameCast<ProxyFrame>(f5) == &f4);
    CATCH_CHECK(frameCast<TaskFrame>(f5) == &f4);
    CATCH_CHECK(frameCast<TaskFrame>(f6) == &f4);
    CATCH_CHECK(f4.followLink() == nullptr);

    f4.linkTo(f2.toHandle());
    CATCH_CHECK(f4.followLink() == f2.toHandle());

    co_return;
}

struct ThrowingAwaitable {
    bool await_ready() const noexcept { return false; }
    void await_suspend(corral::Handle) { throw std::runtime_error("test"); }
    void await_resume() noexcept {}
};

CORRAL_TEST_CASE("throwing-awaitable") {
    CATCH_SECTION("immediate") {
        CATCH_CHECK_THROWS(co_await ThrowingAwaitable{});
    }

    CATCH_SECTION("combiner-first") {
        CATCH_CHECK_THROWS(co_await anyOf(ThrowingAwaitable(), t.sleep(5ms)));
        CATCH_CHECK(t.now() == 0ms);
    }

    CATCH_SECTION("combiner-last") {
        CATCH_CHECK_THROWS(co_await anyOf(t.sleep(5ms), ThrowingAwaitable{}));
        CATCH_CHECK(t.now() == 0ms);
    }

    CATCH_SECTION("combiner-noncancellable") {
        CATCH_CHECK_THROWS(co_await anyOf(t.sleep(5ms, noncancellable),
                                          ThrowingAwaitable{}));
        CATCH_CHECK(t.now() == 5ms);
    }

    CATCH_SECTION("shared") {
        Shared aw{ThrowingAwaitable{}};
        CATCH_CHECK_THROWS(co_await aw);
        CATCH_CHECK_THROWS(co_await aw);
    }
}

CORRAL_TEST_CASE("bounded-channel") {
    Channel<int> channel{3};

    CATCH_SECTION("smoke") {
        std::vector<std::optional<int>> results;

        co_await channel.send(1);
        co_await channel.send(2);
        co_await channel.send(3);

        CATCH_CHECK(channel.full());
        results.push_back(co_await channel.receive());
        results.push_back(co_await channel.receive());
        results.push_back(co_await channel.receive());
        CATCH_CHECK(channel.empty());
        CATCH_CHECK(results == std::vector<std::optional<int>>{1, 2, 3});

        channel.close();

        auto last = co_await channel.receive();
        CATCH_CHECK(last == std::nullopt);
    }

    CATCH_SECTION("blocking") {
        bool ranLast = false;

        CORRAL_WITH_NURSERY(n) {
            n.start([&]() -> Task<> { co_await channel.send(1); });
            n.start([&]() -> Task<> { co_await channel.send(2); });
            n.start([&]() -> Task<> { co_await channel.send(3); });
            n.start([&]() -> Task<> {
                co_await channel.send(4);
                ranLast = true;
            });

            co_await t.sleep(5ms);

            CATCH_CHECK(channel.size() == 3);
            CATCH_CHECK(channel.full());
            CATCH_CHECK(ranLast == false);

            std::array<std::optional<int>, 3> values = {
                    co_await channel.receive(),
                    co_await channel.receive(),
                    co_await channel.receive(),
            };
            CATCH_CHECK(values == std::array<std::optional<int>, 3>{1, 2, 3});

            co_await yield;

            CATCH_CHECK(ranLast == true);
            CATCH_CHECK(channel.size() == 1);

            int value = *co_await channel.receive();
            CATCH_CHECK(value == 4);
            CATCH_CHECK(channel.size() == 0);

            co_return join;
        };
    }

    CATCH_SECTION("alternating") {
        std::vector<int> results;

        co_await allOf(
                [&]() -> Task<> {
                    for (int i = 0; i < 10; i++) {
                        co_await channel.send(i);
                    }
                    channel.close();
                },
                [&]() -> Task<> {
                    while (std::optional<int> v = co_await channel.receive()) {
                        if (*v % 2 == 0) {
                            continue;
                        }
                        results.push_back(*v);
                    }
                });

        CATCH_CHECK(results == std::vector<int>{1, 3, 5, 7, 9});
    }

    CATCH_SECTION("try") {
        // False because channel empty
        CATCH_CHECK(!channel.tryReceive());

        // True because free space
        CATCH_CHECK(channel.trySend(1));
        CATCH_CHECK(channel.trySend(2));
        CATCH_CHECK(channel.trySend(3));

        // False because full
        CATCH_CHECK(!channel.trySend(4));

        // True because not empty
        CATCH_CHECK(channel.tryReceive());
        CATCH_CHECK(channel.tryReceive());

        channel.close();

        // We can still read remainign data while closed
        CATCH_CHECK(channel.tryReceive());

        // But we cannot write new data while closed
        CATCH_CHECK(channel.empty());
        CATCH_CHECK(channel.closed());
        CATCH_CHECK(!channel.trySend(5));
    }

    CATCH_SECTION("close") {
        std::vector<std::optional<int>> results;

        CORRAL_WITH_NURSERY(nursery) {
            co_await channel.send(1);
            co_await channel.send(2);
            co_await channel.send(3);

            // Discard result
            auto send = [&](int value) -> Task<> {
                co_await channel.send(value);
            };

            // These should remain blocked
            nursery.start(send(4));
            nursery.start(send(5));

            co_await yield;

            CATCH_CHECK(channel.full());
            CATCH_CHECK(!channel.closed());
            channel.close();
            CATCH_CHECK(channel.full());
            CATCH_CHECK(channel.closed());

            while (auto item = co_await channel.receive()) {
                results.push_back(*item);
            }

            co_return cancel;
        };


        CATCH_CHECK(channel.empty());
        CATCH_CHECK(results == std::vector<std::optional<int>>{1, 2, 3});

        // More reads will return nullopt
        std::optional<int> item = co_await channel.receive();
        CATCH_CHECK(item == std::nullopt);

        bool sent = co_await channel.send(6);
        CATCH_CHECK(sent == false);
    }
}

CORRAL_TEST_CASE("unbounded-channel") {
    Channel<int> channel;

    CATCH_SECTION("many") {
        for (int i = 0; i < 10'000; i++) {
            co_await channel.send(i);
        }

        CATCH_CHECK(channel.size() == 10'000);

        for (int i = 0; i < 10'000; i++) {
            std::optional<int> v = co_await channel.receive();
            CATCH_REQUIRE(v.has_value());
            CATCH_CHECK(*v == i);
        }

        CATCH_CHECK(channel.size() == 0);
        CATCH_CHECK(channel.empty());
    }

    CATCH_SECTION("close") {
        std::vector<std::optional<int>> results;

        bool sent = co_await channel.send(1);
        CATCH_CHECK(sent);
        channel.close();

        results.push_back(co_await channel.receive());
        CATCH_CHECK(channel.empty());
        results.push_back(co_await channel.receive());
        CATCH_CHECK(channel.empty());

        CATCH_CHECK(results ==
                    std::vector<std::optional<int>>{1, std::nullopt});

        // More writes will fail
        sent = co_await channel.send(2);
        CATCH_CHECK(!sent);
    }
}

} // namespace
