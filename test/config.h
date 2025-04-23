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
#include <catch2/catch.hpp>

#ifndef CATCH_TEST_CASE
#define CATCH_TEST_CASE(...) TEST_CASE(__VA_ARGS__)
#endif

#ifndef CATCH_TEMPLATE_TEST_CASE
#define CATCH_TEMPLATE_TEST_CASE(...) TEMPLATE_TEST_CASE(__VA_ARGS__)
#endif

#ifndef CATCH_SECTION
#define CATCH_SECTION(...) SECTION(__VA_ARGS__)
#endif

#ifndef CATCH_CHECK
#define CATCH_CHECK(...) CHECK(__VA_ARGS__)
#endif

#ifndef CATCH_REQUIRE
#define CATCH_REQUIRE(...) REQUIRE(__VA_ARGS__)
#endif

#ifndef CATCH_CHECK_THROWS
#define CATCH_CHECK_THROWS(expr) CHECK_THROWS(expr)
#endif

#ifndef CATCH_CHECK_THROWS_WITH
#define CATCH_CHECK_THROWS_WITH(expr, matcher) CHECK_THROWS_WITH(expr, matcher)
#endif
