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

#include <string>
#include <vector>

#include "../corral/corral.h"
#include "TestEventLoop.h"
#include "helpers.h"

using namespace corral;
using namespace corral::testing;
using namespace std::chrono_literals;
using corral::detail::ScopeGuard;

#define CORRAL_TEST_CASE(...)                                                  \
    CORRAL_TEST_CASE_IMPL(TestEventLoop, t, __VA_ARGS__)

namespace {

CORRAL_TEST_CASE("task-tree") {
    std::vector<TreeDumpElement> tree;
    std::vector<std::pair<int, std::string>> items;

    CORRAL_WITH_NURSERY(n) {
        n.start(annotate, "a sibling task",
                [&]() -> Task<> { co_await t.sleep(5ms); });

        co_await allOf(
                allOf(annotate("this should complete", t.sleep(1ms)),
                      annotate("this should be sleeping", t.sleep(3ms))),
                [&]() -> Task<> {
                    co_await t.sleep(2ms);

                    co_await dumpTaskTree(std::back_inserter(tree));
                    for (auto& elem : tree) {
                        items.emplace_back(
                                elem.depth,
                                std::holds_alternative<uintptr_t>(elem.value)
                                        ? "<TASK>"
                                        : std::get<const char*>(elem.value));
                    }
                });
        co_return join;
    };

    CATCH_CHECK(items == std::vector<std::pair<int, std::string>>{
                                 {0, "<TASK>"},
                                 {1, "Nursery"},
                                 {2, "<TASK>"},
                                 {3, "AllOf"},
                                 {4, "AllOf"},
                                 {5, "this should be sleeping"},
                                 {6, "Sleep"},
                                 {4, "<TASK>"},
                                 {5, "<YOU ARE HERE>"},
                                 {2, "<TASK>"},
                                 {3, "a sibling task"},
                                 {4, "<TASK>"},
                                 {5, "Sleep"}});
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

} // namespace
