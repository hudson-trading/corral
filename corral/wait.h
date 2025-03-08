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

#include "detail/Sequence.h"
#include "detail/exception.h"
#include "detail/wait.h"
#include "utility.h"

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
template <class Policy = detail::Unspecified, Awaitable... Ts>
Awaitable auto anyOf(Ts&&... awaitables) {
    static_assert(
            sizeof...(Ts) == 0 ||
                    (detail::Cancellable<detail::AwaiterType<Ts>> || ...),
            "anyOf() makes no sense if all awaitables are non-cancellable");

    return makeAwaitable<detail::AnyOf<
            detail::ChooseErrorPolicyForAwaitables<Policy, Ts...>, Ts...>>(
            std::forward<Ts>(awaitables)...);
}

/// Same as above, but for variable-length ranges of awaitables.
/// For similar reasons, returns a vector<Optional<R>>.
template <class Policy = detail::Unspecified, AwaitableRange<> Range>
Awaitable auto anyOf(Range&& range) {
    using T = decltype(*std::begin(range));
    return makeAwaitable<detail::AnyOfRange<
            detail::ChooseErrorPolicyForAwaitables<Policy, T>, Range>>(
            std::forward<Range>(range));
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
template <class Policy = detail::Unspecified, Awaitable... Ts>
Awaitable auto allOf(Ts&&... awaitables) {
    return makeAwaitable<detail::AllOf<
            detail::ChooseErrorPolicyForAwaitables<Policy, Ts...>, Ts...>>(
            std::forward<Ts>(awaitables)...);
}

/// Same as above, but for variable-length ranges of awaitables.
/// Returns a vector<R>.
template <class Policy = detail::DefaultErrorPolicy, AwaitableRange<> Range>
Awaitable auto allOf(Range&& range) {
    using T = decltype(*std::begin(range));
    return makeAwaitable<detail::AllOfRange<
            detail::ChooseErrorPolicyForAwaitables<Policy, T>, Range>>(
            std::forward<Range>(range));
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
template <class Policy = detail::DefaultErrorPolicy, Awaitable... Ts>
Awaitable auto mostOf(Ts&&... awaitables) {
    return makeAwaitable<detail::MostOf<
            detail::ChooseErrorPolicyForAwaitables<Policy, Ts...>, Ts...>>(
            std::forward<Ts>(awaitables)...);
}

/// Same as above, but for variable-length ranges of awaitables.
template <class Policy = detail::DefaultErrorPolicy, AwaitableRange<> Range>
Awaitable auto mostOf(Range&& range) {
    using T = decltype(*std::begin(range));
    return makeAwaitable<detail::MostOfRange<
            detail::ChooseErrorPolicyForAwaitables<Policy, T>, Range>>(
            std::forward<Range>(range));
}

/// A try/finally block allowing both try and finally blocks to be asynchronous,
/// useful instead of a scope guard if the cleanup code is asynchronous.
///
///     AsyncThing thing = co_await AsyncThing::create();
///     co_await corral::try_([&]() -> corral::Task<> {
///         co_await workWithThing();
///     }).catch_([&](const std::logic_error& e) -> corral::Task<> {
///         co_await handleLogicError(e);
///     }).catch_([&](const std::exception& e) -> corral::Task<> {
///         co_await handleException(e);
///     }).finally([&]() -> corral::Task<> {
///         co_await thing.destroy();
///     });
///
/// Unlike anyOf() etc, the try block is fully destroyed (triggering any scope
/// guards etc) before the finally block begins executing.
template <class TryBlock>
    requires(std::is_invocable_r_v<Task<void>, TryBlock>)
auto try_(TryBlock&& tryBlock) {
    return detail::TryBlockBuilder<TryBlock>(std::forward<TryBlock>(tryBlock),
                                             std::make_tuple());
}

/// Same as above but with a different, slightly more laconic, syntax:
///
///     AsyncThing thing = co_await AsyncThing::create();
///     CORRAL_TRY {
///         co_await workWithThing();
///     } CORRAL_CATCH(const std::logic_error& e) {
///         co_await handleLogicError(e);
///     } CORRAL_CATCH(const std::exception& e) {
///         co_await handleException(e);
///     } CORRAL_FINALLY {
///         co_await thing.destroy();
///     }; // <-- the semicolon is required here
#define CORRAL_TRY                                                             \
    co_yield ::corral::detail::TryBlockMacroFactory{} % [&]()                  \
            -> ::corral::Task<void>
#define CORRAL_CATCH(arg) / [&](arg) -> ::corral::Task<void>
#define CORRAL_FINALLY % [&]() -> ::corral::Task<void>

/// As catch_() takes an asynchronous lambda, it will be executed outside
/// of a normal C++ catch-block (as catch-blocks are not allowed to suspend),
/// so `throw` without arguments (normally supposed to re-throw the current
/// exception) will not work (it will std::terminate the process instead).
///
/// Use `co_await corral::rethrow;` to re-throw the current exception instead.
static constexpr const detail::CoAwaitFactory<detail::RethrowCurrentException>
        rethrow;

/// A placeholder type for catch-all clauses in try-blocks
/// (as `catch_([&](...) -> Task<>` is not allowed in C++).
using Ellipsis = detail::Ellipsis;


/// Chains multiple awaitables together, without having to allocate a coroutine
/// frame.
///
/// `thenFn()` must be an invocable that either takes no arguments or takes
/// one argument representing the result of the previous awaitable (by value
/// or by lvalue- or rvalue-reference) and returns a new awaitable.
/// Lifetime of the result of the previous awaitable is extended
/// until the next awaitable completes, allowing the following:
///
///    class My {
///        corral::Semaphore sem_;
///      public:
///        Awaitable<void> auto doSmth() {
///           return sem_.lock() | corral::then([]{
///               reallyDoSmth();
///               return corral::noop();
///           }
///        }
///
/// -- which would be a more efficient equivalent of
///        Task<> doSmth() {
///            co_await sem_.lock();
///            reallyDoSmth();
///        }
///
///
/// Multiple `then()` can be chained together, but using a coroutine
/// might yield better readability in such cases. Also keep in mind that
/// lifetime extension only spans until the next awaitable completes, so
///
///       return sem_.lock() | corral::then(doThis) | corral::then(doThat);
///
/// is roughly equivalent to
///       { co_await sem_.lock(); co_await doThis(); }
///       co_await doThat();
///
/// and is therefore different from
///       return sem_.lock() | corral::then([]{
///           return doThis | corral::then(doThat);
///       });
template <class ThenFn> auto then(ThenFn&& thenFn) {
    return detail::SequenceBuilder<ThenFn>(std::forward<ThenFn>(thenFn));
}

} // namespace corral
