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
