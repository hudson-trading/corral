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

#include <boost/asio.hpp>
#include <boost/version.hpp>

#include "detail/asio.h"

#if BOOST_VERSION < 107700
#error boost 1.77 or above is required
#endif


namespace corral::detail {
struct BoostAsioImpl {
    using io_context = boost::asio::io_context;
    using steady_timer = boost::asio::steady_timer;

    using cancellation_slot = boost::asio::cancellation_slot;
    using cancellation_signal = boost::asio::cancellation_signal;
    using cancellation_type = boost::asio::cancellation_type;

    static constexpr const auto operation_aborted =
            boost::asio::error::operation_aborted;

    template <class C, class Sig, class Init, class... Args>
    static auto async_initiate(Init&& init,
                               std::type_identity_t<C>& c,
                               Args&&... args) {
        return boost::asio::async_initiate<C, Sig>(std::forward<Init>(init), c,
                                                   std::forward<Args>(args)...);
    }

    template <class... Args> static auto dispatch(Args&&... args) {
        return boost::asio::dispatch(std::forward<Args>(args)...);
    }

    using error_code = boost::system::error_code;
    using system_error = boost::system::system_error;
};
} // namespace corral::detail

namespace boost::asio {
template <bool ThrowOnError, class X, class... Ret>
class async_result<::corral::detail::asio_awaitable_t<ThrowOnError>,
                   X(boost::system::error_code, Ret...)>
  : public ::corral::detail::AsyncResultImpl<::corral::detail::BoostAsioImpl,
                                             ThrowOnError,
                                             Ret...> {};
} // namespace boost::asio

namespace corral {

template <>
struct EventLoopTraits<boost::asio::io_context>
  : detail::AsioEventLoopTraitsImpl<detail::BoostAsioImpl> {};
template <>
struct ThreadNotification<boost::asio::io_context>
  : detail::AsioThreadNotificationImpl<detail::BoostAsioImpl> {
    using ThreadNotification::AsioThreadNotificationImpl::
            AsioThreadNotificationImpl;
};

template <class R, class P>
auto sleepFor(boost::asio::io_context& io, std::chrono::duration<R, P> delay) {
    return detail::AsioTimer<detail::BoostAsioImpl>(io, delay);
}

inline auto sleepFor(boost::asio::io_context& io,
                     boost::posix_time::time_duration delay) {
    return detail::AsioTimer<detail::BoostAsioImpl>(
            io, std::chrono::nanoseconds(delay.total_nanoseconds()));
}
} // namespace corral
