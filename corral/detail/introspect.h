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
#include <cstdio>
#include <iterator>
#include <string>
#include <variant>

#include "../concepts.h"
#include "ScopeGuard.h"
#include "utility.h"

namespace corral {

struct TreeDumpElement {
    CORRAL_NO_UNIQUE_ADDR std::variant<
            // If the tree node is an async function invocation, this holds
            // the address it's currently suspended on.
            uintptr_t /*pc*/,

            // If the tree node is an Introspectable awaiter,
            // this holds its name, as reported by `node()` call.
            const char* /*name*/

#if __cpp_rtti
            // If the tree node is an awaiter which is not Introspectable,
            // this holds its type.
            ,
            const std::type_info* /*type*/
#endif
            >
            value;
    const void* ptr;
    int depth;
};

namespace detail {

class TaskTreeCollector {
  public:
    TaskTreeCollector(void (*sink)(void*, TreeDumpElement), void* cookie)
      : sink_(sink), cookie_(cookie) {}

    void node(const char* name, const void* ptr = nullptr) noexcept {
        TreeDumpElement elt{name, ptr, depth_};
        sink_(cookie_, elt);
    }

#if __cpp_rtti
    void node(const std::type_info* ti, const void* ptr = nullptr) noexcept {
        TreeDumpElement elt{ti, ptr, depth_};
        sink_(cookie_, elt);
    }
#endif

    void taskPC(uintptr_t pc, Handle h = {}) noexcept {
        TreeDumpElement elt{pc, h.address(), depth_};
        sink_(cookie_, elt);
    }

    template <class Child> void child(const Child& child) noexcept {
        ++depth_;
        ScopeGuard guard([&] { --depth_; });

        if constexpr (Introspectable<Child>) {
            child.await_introspect(*this);
        } else {
#if __cpp_rtti
            TreeDumpElement elt{&typeid(child), &child, depth_};
#else
            TreeDumpElement elt{"<non-introspectable awaitable>", &child,
                                depth_};
#endif
            sink_(cookie_, elt);
        }
    }

    void footnote(const char* name) noexcept {
        TreeDumpElement elt{name, nullptr, depth_ + 1};
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

template <class T> class AnnotatedTreeNode : public AwaiterAdapterBase<T> {
  public:
    AnnotatedTreeNode(std::string annotation, T&& awaitable)
      : AwaiterAdapterBase<T>(std::forward<T>(awaitable)),
        annotation_(std::move(annotation)) {}

    void await_introspect(auto& c) const noexcept {
        c.node(annotation_.c_str());
        c.child(this->awaiter_);
    }

  private:
    std::string annotation_;
};

} // namespace detail

/// Format one task-tree node as text and feed the pieces to `sink`.
///
/// `sink` is invoked with NUL-terminated C strings; pointers are only
/// valid for the duration of that call. Indentation is two spaces per
/// `TreeDumpElement::depth`. Task frames (program counters) are rendered
/// as `Task 0x...` so they can be passed to addr2line; named introspect
/// nodes are rendered as their `await_introspect()` name.
template <class Sink>
void writeTreeDumpElement(const TreeDumpElement& node, Sink&& sink) {
    for (int i = 0; i < node.depth; ++i) {
        sink("  ");
    }
    if (const uintptr_t* pc = std::get_if<uintptr_t>(&node.value)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "Task 0x%llx",
                      static_cast<unsigned long long>(*pc));
        sink(buf);
    } else if (const char* const* name = std::get_if<const char*>(&node.value)) {
        sink(*name ? *name : "<unnamed>");
#if __cpp_rtti
    } else if (const std::type_info* const* ti =
                       std::get_if<const std::type_info*>(&node.value)) {
        sink(*ti && (*ti)->name() ? (*ti)->name()
                                  : "<non-introspectable awaitable>");
#endif
    } else {
        sink("<unknown awaitable>");
    }
    sink("\n");
}

inline void writeTreeDumpElement(const TreeDumpElement& node, std::FILE* out) {
    if (!out) {
        return;
    }
    writeTreeDumpElement(node, [out](const char* s) { std::fputs(s, out); });
}

/// Walk `root` (an awaitable or `Executor`) and feed a text rendering of
/// its task tree to `sink`, one C string at a time. Useful for embedding
/// the tree in an exception message from a custom
/// `CORRAL_FAIL_FOR_DANGLING_TASKS`.
template <class Awaitable, class Sink>
void formatTaskTree(const Awaitable& root, Sink&& sink) {
    detail::dumpTaskTree(root, [&](const TreeDumpElement& node) {
        writeTreeDumpElement(node, sink);
    });
}

/// Write a text rendering of the task tree under `root` to `out`.
/// Intended for crash / dangling-task diagnostics; does not allocate.
template <class Awaitable>
void writeTaskTree(std::FILE* out, const Awaitable& root) {
    if (!out) {
        return;
    }
    formatTaskTree(root, [out](const char* s) { std::fputs(s, out); });
}

namespace detail {

/// Default body of `CORRAL_FAIL_FOR_DANGLING_TASKS`: print the task tree
/// to stderr so the failure is diagnosable, then assert.
template <class Awaitable>
void failForDanglingTasks(const char* msg, const Awaitable& root) {
    std::fputs("corral: ", stderr);
    std::fputs(msg ? msg : "(dangling tasks)", stderr);
    std::fputs("\ntask tree:\n", stderr);
    writeTaskTree(stderr, root);
    std::fflush(stderr);
    CORRAL_ASSERT(false && msg);
}

} // namespace detail
} // namespace corral
