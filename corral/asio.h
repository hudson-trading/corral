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

#include <chrono>

#include <boost/asio.hpp>
#include <boost/version.hpp>

#include "Executor.h"
#include "detail/utility.h"

#if BOOST_VERSION < 107700
#error boost 1.77 or above is required
#endif

namespace corral {

namespace detail {
template <class Executor, bool ThrowOnError> struct asio_awaitable_t {
    constexpr asio_awaitable_t() = default;
};
} // namespace detail


/// As ASIO completion token, suitable for passing into any `asio::async_*()`
/// funciton, which would convert it into corral C++20 awaitable.
///
/// Requires the first argument of the completion signature to be `error_code`,
/// and throws `system_error` exception if called with a nontrivial error code.
///
///    boost::asio::deadline_timer t(io);
///    corral::Awaitable<void> auto aw = t.async_wait(corral::asio_awaitable);
///    co_await aw;
static constexpr const detail::asio_awaitable_t<boost::asio::executor, true>
        asio_awaitable;

/// Same as above, but does not throw any exceptions, instead prepending
/// its co_returned value with `error_code`:
///
///    corral::Awaitable<boost::system::error_code> auto aw =
///        t.async_wait(corral::asio_nothrow_awaitable);
///    auto ec = co_await aw;
static constexpr const detail::asio_awaitable_t<boost::asio::executor, false>
        asio_nothrow_awaitable;


// -----------------------------------------------------------------------------
// Implementation

namespace detail {

template <bool ThrowOnError, class Init, class Args, class... Ret>
class AsioAwaitableFactory;
template <bool ThrowOnError, class... Ret> class TypeErasedAsioAwaitableFactory;

template <class... Ret> class AsioAwaitableBase {
  protected:
    using Err = boost::system::error_code;
    struct DoneCB {
        AsioAwaitableBase* aw_;

        void operator()(Err err, Ret... ret) const {
            aw_->done(err, std::forward<Ret>(ret)...);
        }

        using cancellation_slot_type = boost::asio::cancellation_slot;
        cancellation_slot_type get_cancellation_slot() const {
            return aw_->cancelSig_.slot();
        }
    };

    void done(Err ec, Ret... ret) {
        ec_ = ec;
        ret_.emplace(std::forward<Ret>(ret)...);
        std::exchange(parent_, std::noop_coroutine())();
    }

  protected:
    Err ec_;
    std::optional<std::tuple<Ret...>> ret_;
    Handle parent_;
    DoneCB doneCB_;
    boost::asio::cancellation_signal cancelSig_;

    template <bool, class, class, class...> friend class AsioAwaitableFactory;
    template <bool, class...> friend class TypeErasedAsioAwaitableFactory;
};


template <class InitFn, bool ThrowOnError, class... Ret>
class AsioAwaitable : private AsioAwaitableBase<Ret...> {
    using Err = boost::system::error_code;

  public:
    explicit AsioAwaitable(InitFn&& initFn)
      : initFn_(std::forward<InitFn>(initFn)) {}

    AsioAwaitable(AsioAwaitable&&) = delete;

    bool await_ready() const noexcept { return false; }

    void await_suspend(Handle h) {
        this->parent_ = h;
        this->doneCB_ = typename AsioAwaitableBase<Ret...>::DoneCB{this};
        this->initFn_(this->doneCB_);
    }

    auto await_resume() noexcept(!ThrowOnError) {
        if constexpr (ThrowOnError) {
            if (this->ec_) {
                throw boost::system::system_error(this->ec_);
            }

            if constexpr (sizeof...(Ret) == 0) {
                return;
            } else if constexpr (sizeof...(Ret) == 1) {
                return std::move(std::get<0>(*this->ret_));
            } else {
                return std::move(*this->ret_);
            }
        } else {
            if constexpr (sizeof...(Ret) == 0) {
                return this->ec_;
            } else {
                return std::tuple_cat(std::make_tuple(this->ec_),
                                      std::move(*this->ret_));
            }
        }
    }

    bool await_cancel(Handle) noexcept {
        this->cancelSig_.emit(boost::asio::cancellation_type::all);
        return false;
    }

    bool await_must_resume() const noexcept {
        return this->ec_ != boost::asio::error::operation_aborted;
    }

  private:
    [[no_unique_address]] InitFn initFn_;
};


template <bool ThrowOnError, class Init, class Args, class... Ret>
class AsioAwaitableFactory {
    using DoneCB = typename AsioAwaitableBase<Ret...>::DoneCB;

    struct InitFn {
        Init init_;
        [[no_unique_address]] Args args_;

        template <class... Ts>
        explicit InitFn(Init&& init, Ts&&... ts)
          : init_(std::forward<Init>(init)), args_(std::forward<Ts>(ts)...) {}

        void operator()(DoneCB& doneCB) {
            auto impl = [this, &doneCB]<class... A>(A&&... args) {
                using Sig = void(boost::system::error_code, Ret...);
                boost::asio::async_initiate<DoneCB, Sig>(
                        std::move(init_), doneCB, std::forward<A>(args)...);
            };
            std::apply(impl, args_);
        }
    };

  public:
    template <class... Ts>
    explicit AsioAwaitableFactory(Init&& init, Ts&&... args)
      : initFn_(std::forward<Init>(init), std::forward<Ts>(args)...) {}

    ImmediateAwaitable auto operator co_await() && {
        return AsioAwaitable<InitFn, ThrowOnError, Ret...>(
                std::forward<InitFn>(initFn_));
    }

  private:
    InitFn initFn_;

    friend TypeErasedAsioAwaitableFactory<ThrowOnError, Ret...>;
};


/// AsioAwaitableFactory is parametrized by its initiation object (as it needs
/// to store it). `asio::async_result<>` does not have initiation among
/// its type parameter list, yet needs to export something under dependent
/// name `return_type`, which is used for asio-related functions still
/// having an explicit return type (like Boost.Beast).
///
/// To accommodate that, we have this class, which stores a type-erased
/// initiation object, and is constructible from `AsioAwaitableFactory`.
template <bool ThrowOnError, class... Ret>
class TypeErasedAsioAwaitableFactory {
    using DoneCB = typename AsioAwaitableBase<Ret...>::DoneCB;

    struct InitFnBase {
        virtual ~InitFnBase() = default;
        virtual void operator()(DoneCB&) = 0;
    };

    template <class Impl> struct InitFnImpl : InitFnBase {
        explicit InitFnImpl(Impl&& impl) : impl_(std::move(impl)) {}
        void operator()(DoneCB& doneCB) override { impl_(doneCB); }

      private:
        [[no_unique_address]] Impl impl_;
    };

    struct InitFn {
        std::unique_ptr<InitFnBase> impl_;
        template <class Impl>
        explicit InitFn(Impl&& impl)
          : impl_(std::make_unique<InitFnImpl<Impl>>(
                    std::forward<Impl>(impl))) {}
        void operator()(DoneCB& doneCB) { (*impl_)(doneCB); }
    };

  public:
    template <class Init, class Args>
    explicit(false) TypeErasedAsioAwaitableFactory(
            AsioAwaitableFactory<ThrowOnError, Init, Args, Ret...>&& rhs)
      : initFn_(std::move(rhs.initFn_)) {}

    ImmediateAwaitable auto operator co_await() && {
        return AsioAwaitable<InitFn, ThrowOnError, Ret...>(std::move(initFn_));
    }

  private:
    InitFn initFn_;
};

} // namespace detail

template <> struct EventLoopTraits<boost::asio::io_service> {
    static EventLoopID eventLoopID(boost::asio::io_service& io) {
        return EventLoopID(&io);
    }

    static void run(boost::asio::io_service& io) {
        io.run();
        io.reset();
    }

    static void stop(boost::asio::io_service& io) { io.stop(); }
};

} // namespace corral

namespace boost::asio {

template <class Executor, bool ThrowOnError, class X, class... Ret>
class async_result<::corral::detail::asio_awaitable_t<Executor, ThrowOnError>,
                   X(boost::system::error_code, Ret...)> {
  public:
    using return_type =
            ::corral::detail::TypeErasedAsioAwaitableFactory<ThrowOnError,
                                                             Ret...>;

    /// Use `AsioAwaitableFactory` here, so asio::async_*() functions which
    /// don't use `return_type` and instead have `auto` for their return types
    /// will do without type erase.
    template <class Init, class... Args>
    static auto initiate(
            Init&& init,
            ::corral::detail::asio_awaitable_t<Executor, ThrowOnError>,
            Args... args) {
        return ::corral::detail::AsioAwaitableFactory<
                ThrowOnError, Init, std::tuple<Args...>, Ret...>(
                std::forward<Init>(init), std::move(args)...);
    }
};

} // namespace boost::asio


namespace corral {
namespace detail {
/// A helper class for `corral::sleepFor()`.
class Timer {
  public:
    template <class R, class P>
    Timer(boost::asio::io_service& io, std::chrono::duration<R, P> delay)
      : timer_(io) {
        timer_.expires_from_now(toPosixMicros(delay));
    }

    Timer(boost::asio::io_service& io, boost::posix_time::time_duration delay)
      : timer_(io) {
        timer_.expires_from_now(delay);
    }

    ImmediateAwaitable auto operator co_await() {
        return getAwaitable(timer_.async_wait(asio_awaitable));
    }

  private:
    auto toPosixMicros(auto duration) {
        return boost::posix_time::microseconds(
                std::chrono::duration_cast<std::chrono::microseconds>(duration)
                        .count());
    }

  private:
    boost::asio::deadline_timer timer_;
};
} // namespace detail

/// A utility function, returning an awaitable suspending the caller
/// for specified duration. Suitable for use with anyOf() etc.
template <class R, class P>
auto sleepFor(boost::asio::io_service& io, std::chrono::duration<R, P> delay) {
    return detail::Timer(io, delay);
}
inline auto sleepFor(boost::asio::io_service& io,
                     boost::posix_time::time_duration delay) {
    return detail::Timer(io, delay);
}

} // namespace corral
