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

#include "detail/wait.h"

namespace corral {

//
// AnyOf
//

/// Run multiple awaitables concurrently. Upon completion
/// of any one, request cancellation of the rest; once all are finished
/// executing, return the result(s) of the awaitable(s) that completed
/// normally.
///
/// Returns a std::tuple<Optional<R>...> of the awaitable return types,
/// as multiple awaitables may complete at the same time.
///
/// If any of the awaitables would return void, it will be replaced
/// by `corral::detail::Void` type, so the resulting type would still
/// compile. This applies even if all awaitables return void, so one can use
/// `std::get<N>(result).has_value()` to figure out which awaitable(s)
/// completed.
template <Awaitable... Ts> auto anyOf(Ts&&... awaitables) {
    static_assert(
            sizeof...(Ts) == 0 ||
                    (detail::Cancellable<detail::AwaitableType<Ts>> || ...),
            "anyOf() makes no sense if all awaitables are non-cancellable");

    return detail::AnyOf<detail::AwaitableType<Ts>...>(
            detail::getAwaitable(std::forward<Ts>(awaitables))...);
}

/// Same as above, but for variable-length ranges of awaitables.
/// For similar reasons, returns a vector<Optional<R>>.
template <AwaitableRange<> Range> auto anyOf(Range&& range) {
    return detail::AnyOfRange<Range>(std::forward<Range>(range));
}


//
// AllOf
//

/// Run multiple awaitables concurrently; once all of them complete,
/// return a std::tuple<R...> of the results.
///
/// If cancellation occurs before all awaitables complete, the results
/// of the awaitables that did complete before the cancellation may be
/// discarded. If that's not desirable, use `corral::mostOf()` instead.
template <Awaitable... Ts> auto allOf(Ts&&... awaitables) {
    return detail::AllOf<detail::AwaitableType<Ts>...>(
            detail::getAwaitable(std::forward<Ts>(awaitables))...);
}

/// Same as above, but for variable-length ranges of awaitables.
/// Returns a vector<R>.
template <AwaitableRange<> Range> auto allOf(Range&& range) {
    return detail::AllOfRange<Range>(std::forward<Range>(range));
}


//
// MostOf
//

/// Run multiple awaitables concurrently; once all of them complete
/// or are cancelled, return a tuple of the available results.
///
/// Upon cancellation, proxies cancellation to all of the awaitables;
/// if some of them complete before cancellation and others get cancelled,
/// may return a partial result. Hence returns a std::tuple<Optional<R>...>.
template <Awaitable... Ts> auto mostOf(Ts&&... awaitables) {
    return detail::MostOf<detail::AwaitableType<Ts>...>(
            detail::getAwaitable(std::forward<Ts>(awaitables))...);
}

/// Same as above, but for variable-length ranges of awaitables.
template <AwaitableRange<> Range> auto mostOf(Range&& range) {
    return detail::MostOfRange<Range>(std::forward<Range>(range));
}

/// A try/finally block allowing both try and finally blocks to be asynchronous,
/// useful instead of a scope guard if the cleanup code is asynchronous.
///
///     AsyncThing thing = co_await AsyncThing::create();
///     co_await corral::try_([&]() -> corral::Task<> {
///         co_await workWithThing();
///     }).finally([&]() -> corral::Task<> {
///         co_await thing.destroy();
///     });
///
/// Unlike anyOf() etc, the try block is fully destroyed (triggering any scope
/// guards etc) before the finally block begins executing.
template <class TryBlock>
    requires(std::is_invocable_r_v<Task<void>, TryBlock>)
auto try_(TryBlock&& tryBlock) {
    return detail::TryFinallyFactory(std::forward<TryBlock>(tryBlock));
}

/// Same as above but with a different, slightly more laconic, syntax:
///
///     AsyncThing thing = co_await AsyncThing::create();
///     CORRAL_TRY {
///         co_await workWithThing();
///     } CORRAL_FINALLY {
///         co_await thing.destroy();
///     }; // <-- the semicolon is required here
#define CORRAL_TRY                                                             \
    co_yield ::corral::detail::TryFinallyMacroFactory{} % [&]()                \
            -> ::corral::Task<void>
#define CORRAL_FINALLY % [&]() -> ::corral::Task<void>

} // namespace corral
