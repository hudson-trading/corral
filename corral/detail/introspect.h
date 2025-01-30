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

#include <concepts>
#include <iterator>
#include <variant>

#include "../concepts.h"
#include "ScopeGuard.h"

namespace corral {

struct TreeDumpElement {
    [[no_unique_address]] std::variant<
            // If the tree node is an async function invocation, this holds
            // the address it's currently suspended on.
            uintptr_t /*pc*/,

            // If the tree node is an Introspectable awaiter,
            // this holds its name, as reported by `node()` call.
            const char* /*name*/,

            // If the tree node is an awaiter which is not Introspectable,
            // this holds its type.
            const std::type_info* /*type*/> value;
    int depth;
};

namespace detail {

class TaskTreeCollector {
  public:
    TaskTreeCollector(void (*sink)(void*, TreeDumpElement), void* cookie)
      : sink_(sink), cookie_(cookie) {}

    void node(const char* name) noexcept {
        TreeDumpElement elt{name, depth_};
        sink_(cookie_, elt);
    }

    void node(const std::type_info* ti) noexcept {
        TreeDumpElement elt{ti, depth_};
        sink_(cookie_, elt);
    }

    void taskPC(uintptr_t pc) noexcept {
        TreeDumpElement elt{pc, depth_};
        sink_(cookie_, elt);
    }

    template <class Child> void child(const Child& child) noexcept {
        ++depth_;
        ScopeGuard guard([&] { --depth_; });

        if constexpr (Introspectable<Child>) {
            child.await_introspect(*this);
        } else {
            TreeDumpElement elt{&typeid(child), depth_};
            sink_(cookie_, elt);
        }
    }

    void footnote(const char* name) noexcept {
        TreeDumpElement elt{name, depth_ + 1};
        sink_(cookie_, elt);
    }

  private:
    int depth_ = 0;
    void (*sink_)(void*, TreeDumpElement);
    void* cookie_;
};

template <class Awaiter, std::invocable<const TreeDumpElement&> Sink>
void dumpTaskTree(const Awaiter& awaiter, Sink&& sink) {
    TaskTreeCollector collector(
            [](void* cookie, TreeDumpElement node) {
                auto& sink_ =
                        *static_cast<std::remove_reference_t<Sink>*>(cookie);
                sink_(std::move(node));
            },
            &sink);
    if constexpr (std::is_same_v<Awaiter, Executor>) {
        awaiter.collectTaskTree(collector);
    } else {
        awaitIntrospect(awaiter, collector);
    }
}

template <std::output_iterator<TreeDumpElement> OutIt>
OutIt dumpTaskTree(const auto& awaiter, OutIt out) {
    dumpTaskTree(awaiter,
                 [&out](const TreeDumpElement& node) { *out++ = node; });
    return out;
}

} // namespace detail
} // namespace corral
