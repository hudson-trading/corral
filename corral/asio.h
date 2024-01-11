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

template <class Self, bool ThrowOnError, class... Ret> class AsioAwaitableBase {
    using Err = boost::system::error_code;

  protected:
    struct DoneCB {
        AsioAwaitableBase* aw_;

        void operator()(Err err, Ret... ret) const {
            aw_->done(err, std::forward<Ret>(ret)...);
        }

        using cancellation_slot_type = boost::asio::cancellation_slot;
        cancellation_slot_type get_cancellation_slot() const {
            return aw_->cancelSignal().slot();
        }
    };

  public:
    bool await_ready() const noexcept { return false; }

    void await_suspend(Handle h) {
        new (&cancelSig_) boost::asio::cancellation_signal();
        parent_ = h;
        doneCB_ = DoneCB{this};
        static_cast<Self*>(this)->kickOff();
    }

    auto await_resume() noexcept(!ThrowOnError) {
        if constexpr (ThrowOnError) {
            if (ec_) {
                throw boost::system::system_error(ec_);
            }

            if constexpr (sizeof...(Ret) == 0) {
                return;
            } else if constexpr (sizeof...(Ret) == 1) {
                return std::move(std::get<0>(*ret_));
            } else {
                return std::move(*ret_);
            }
        } else {
            if constexpr (sizeof...(Ret) == 0) {
                return ec_;
            } else {
                return std::tuple_cat(std::make_tuple(ec_), std::move(*ret_));
            }
        }
    }

    bool await_cancel(Handle) noexcept {
        cancelSignal().emit(boost::asio::cancellation_type::all);
        return false;
    }

    bool await_must_resume() const noexcept {
        return ec_ != boost::asio::error::operation_aborted;
    }

  protected:
    auto& doneCB() { return doneCB_; }

  private:
    boost::asio::cancellation_signal& cancelSignal() {
        return *std::launder(
                reinterpret_cast<boost::asio::cancellation_signal*>(
                        &cancelSig_));
    }

    void done(Err ec, Ret... ret) {
        ec_ = ec;
        ret_.emplace(std::forward<Ret>(ret)...);
        cancelSignal().~cancellation_signal();
        std::exchange(parent_, std::noop_coroutine())();
    }

  private:
    template <class T>
    using StorageFor = std::aligned_storage_t<sizeof(T), alignof(T)>;

    Err ec_;
    std::optional<std::tuple<Ret...>> ret_;
    Handle parent_;
    DoneCB doneCB_;

    // cancellation_signal is nonmoveable, while AsioAwaitable needs
    // to be moveable, so we use aligned_storage to store it.
    //
    // cancellation_signal lifetime spans from await_suspend() to done();
    // AsioAwaitable is guaranteed not to get moved during that time window.
    StorageFor<boost::asio::cancellation_signal> cancelSig_;
};


template <bool ThrowOnError, class Init, class Args, class... Ret>
class AsioAwaitable
  : public AsioAwaitableBase<AsioAwaitable<ThrowOnError, Init, Args, Ret...>,
                             ThrowOnError,
                             Ret...> {
    using Base = AsioAwaitableBase<AsioAwaitable, ThrowOnError, Ret...>;
    friend Base;

  public:
    template <class... Ts>
    explicit AsioAwaitable(Init&& init, Ts&&... args)
      : init_(std::forward<Init>(init)), args_(std::forward<Ts>(args)...) {}

  private /*methods*/:
    void kickOff() {
        auto impl = [this]<class... A>(A&&... args) {
            using Sig = void(boost::system::error_code, Ret...);
            boost::asio::async_initiate<typename Base::DoneCB, Sig>(
                    std::forward<Init>(init_), this->doneCB(),
                    std::forward<A>(args)...);
        };
        std::apply(impl, args_);
    }

  private /*fields*/:
    Init init_;
    [[no_unique_address]] Args args_;
    template <bool, class...> friend class TypeErasedAsioAwaitable;
};


/// AsioAwaitable is parametrized by its initiation object (as it needs
/// to store it). `asio::async_result<>` does not have initiation among
/// its type parameter list, yet needs to export something under dependent
/// name `return_type`, which is used for asio-related functions still
/// having an explicit return type (like Boost.Beast).
///
/// To accommodate that, we have this class, which stores a type-erased
/// initiation object, and is constructible from `AsioAwaitable`.
template <bool ThrowOnError, class... Ret>
class TypeErasedAsioAwaitable
  : public AsioAwaitableBase<TypeErasedAsioAwaitable<ThrowOnError, Ret...>,
                             ThrowOnError,
                             Ret...> {
    using Base =
            AsioAwaitableBase<TypeErasedAsioAwaitable, ThrowOnError, Ret...>;
    friend Base;

    struct InitFn {
        virtual ~InitFn() = default;
        virtual void kickOff(typename Base::DoneCB&) = 0;
    };
    template <class Init, class Args> struct InitFnImpl : InitFn {
        Init init_;
        Args args_;

        InitFnImpl(Init&& init, Args&& args)
          : init_(std::forward<Init>(init)), args_(std::move(args)) {}
        void kickOff(typename Base::DoneCB& doneCB) override {
            auto impl = [this, &doneCB]<class... A>(A&&... args) {
                boost::asio::async_initiate<typename Base::DoneCB,
                                            void(boost::system::error_code,
                                                 Ret...)>(
                        std::forward<Init>(init_), doneCB,
                        std::forward<A>(args)...);
            };
            std::apply(impl, args_);
        }
    };

    std::unique_ptr<InitFn> initFn_;

  public:
    template <class Init, class Args>
    explicit(false) TypeErasedAsioAwaitable(
            AsioAwaitable<ThrowOnError, Init, Args, Ret...>&& rhs)
      : initFn_(std::make_unique<InitFnImpl<Init, Args>>(
                std::move(rhs.init_), std::move(rhs.args_))) {}

  private:
    void kickOff() { initFn_->kickOff(this->doneCB()); }
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
            ::corral::detail::TypeErasedAsioAwaitable<ThrowOnError, Ret...>;

    /// Use `AsioAwaitable` here, so asio::async_*() functions which don't
    /// use `return_type` and instead have `auto` for their return types
    /// will do without type erase.
    template <class Init, class... Args>
    static auto initiate(
            Init&& init,
            ::corral::detail::asio_awaitable_t<Executor, ThrowOnError>,
            Args&&... args) {
        return ::corral::detail::AsioAwaitable<ThrowOnError, Init,
                                               std::tuple<Args...>, Ret...>(
                std::forward<Init>(init), std::forward<Args>(args)...);
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

    Awaitable<void> auto operator co_await() {
        return timer_.async_wait(asio_awaitable);
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
