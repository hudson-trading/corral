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

#include <asio.hpp>
#include <asio/version.hpp>
#include <system_error>

#include "detail/asio.h"

#if ASIO_VERSION < 101900
#error asio 1.19 or above is required
#endif

namespace corral::detail {
struct StandaloneAsioImpl {
    using io_service = asio::io_service;
    using steady_timer = asio::steady_timer;

    using cancellation_slot = asio::cancellation_slot;
    using cancellation_signal = asio::cancellation_signal;
    using cancellation_type = asio::cancellation_type;

    static constexpr const auto operation_aborted =
            asio::error::operation_aborted;

    template <class C, class Sig, class Init, class... Args>
    static auto async_initiate(Init&& init,
                               std::type_identity_t<C>& c,
                               Args&&... args) {
        return asio::async_initiate<C, Sig>(std::forward<Init>(init), c,
                                            std::forward<Args>(args)...);
    }

    template <class... Args> static auto dispatch(Args&&... args) {
        return asio::dispatch(std::forward<Args>(args)...);
    }

    using error_code = std::error_code;
    using system_error = std::system_error;
};
} // namespace corral::detail

namespace asio {
template <bool ThrowOnError, class X, class... Ret>
class async_result<::corral::detail::asio_awaitable_t<ThrowOnError>,
                   X(std::error_code, Ret...)>
  : public ::corral::detail::AsyncResultImpl<
            ::corral::detail::StandaloneAsioImpl,
            ThrowOnError,
            Ret...> {};
} // namespace asio

namespace corral {

template <>
struct EventLoopTraits<asio::io_service>
  : detail::AsioEventLoopTraitsImpl<detail::StandaloneAsioImpl> {};
template <>
struct ThreadNotification<asio::io_service>
  : detail::AsioThreadNotificationImpl<detail::StandaloneAsioImpl> {
    using ThreadNotification::AsioThreadNotificationImpl::
            AsioThreadNotificationImpl;
};

template <class R, class P>
auto sleepFor(asio::io_service& io, std::chrono::duration<R, P> delay) {
    return detail::AsioTimer<detail::StandaloneAsioImpl>(io, delay);
}
} // namespace corral
