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

#include <chrono>
#include <type_traits>

#include "../Executor.h"
#include "utility.h"

namespace corral {

namespace detail {
template <bool ThrowOnError> struct asio_awaitable_t {
    constexpr asio_awaitable_t() = default;
};
} // namespace detail

/// As ASIO completion token, suitable for passing into any `asio::async_*()`
/// funciton, which would convert it into corral C++20 awaitable.
///
/// Requires the first argument of the completion signature to be `error_code`,
/// and throws `system_error` exception if called with a nontrivial error code.
///
///    asio::steady_timer t(io);
///    co_await = t.async_wait(corral::asio_awaitable);
///    co_await aw;
///
/// Works for both boost::asio and standalone asio, depending on whether
/// "corral/asio.h" and/or "corral/asio-standalone.h" is included
/// (the two can be used together in the same source file).
static constexpr const detail::asio_awaitable_t<true> asio_awaitable;

/// Same as above, but does not throw any exceptions, instead prepending
/// its co_returned value with `error_code`:
///
///    std::error_code ec =
///        co_await t.async_wait(corral::asio_nothrow_awaitable);
static constexpr const detail::asio_awaitable_t<false> asio_nothrow_awaitable;


namespace detail {

template <class AsioImpl,
          bool ThrowOnError,
          class Init,
          class Args,
          class... Ret>
class AsioAwaitable;

template <class AsioImpl, bool ThrowOnError, class... Ret>
class TypeErasedAsioAwaitable;

template <class AsioImpl, class... Ret> class AsioAwaiterBase {
    using Err = typename AsioImpl::error_code;

  protected:
    struct DoneCB {
        AsioAwaiterBase* aw_;

        void operator()(Err err, Ret... ret) const {
            aw_->done(err, std::forward<Ret>(ret)...);
        }

        using cancellation_slot_type = typename AsioImpl::cancellation_slot;
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
    typename AsioImpl::cancellation_signal cancelSig_;

    template <class, bool, class, class, class...> friend class AsioAwaitable;
    template <class, bool, class...> friend class TypeErasedAsioAwaitable;
};


template <class AsioImpl, class InitFn, bool ThrowOnError, class... Ret>
class AsioAwaiter : private AsioAwaiterBase<AsioImpl, Ret...>,
                    private detail::Noncopyable {
  public:
    explicit AsioAwaiter(InitFn&& initFn)
      : initFn_(std::forward<InitFn>(initFn)) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(Handle h) {
        this->parent_ = h;
        this->doneCB_ =
                typename AsioAwaiterBase<AsioImpl, Ret...>::DoneCB{this};
        this->initFn_(this->doneCB_);
    }

    auto await_resume() noexcept(!ThrowOnError) {
        if constexpr (ThrowOnError) {
            if (this->ec_) {
                throw typename AsioImpl::system_error(this->ec_);
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
        this->cancelSig_.emit(AsioImpl::cancellation_type::all);
        return false;
    }

    bool await_must_resume() const noexcept {
        return this->ec_ != AsioImpl::operation_aborted;
    }

  private:
    CORRAL_NO_UNIQUE_ADDR InitFn initFn_;
};


template <class AsioImpl,
          bool ThrowOnError,
          class Init,
          class Args,
          class... Ret>
class AsioAwaitable {
    using DoneCB = typename AsioAwaiterBase<AsioImpl, Ret...>::DoneCB;

    struct InitFn {
        Init init_;
        CORRAL_NO_UNIQUE_ADDR Args args_;

        template <class... Ts>
        explicit InitFn(Init&& init, Ts&&... ts)
          : init_(std::forward<Init>(init)), args_(std::forward<Ts>(ts)...) {}

        void operator()(DoneCB& doneCB) {
            auto impl = [this, &doneCB]<class... A>(A&&... args) {
                using Sig = void(typename AsioImpl::error_code, Ret...);
                AsioImpl::template async_initiate<DoneCB, Sig>(
                        std::move(init_), doneCB, std::forward<A>(args)...);
            };
            std::apply(impl, args_);
        }
    };

  public:
    template <class... Ts>
    explicit AsioAwaitable(Init&& init, Ts&&... args)
      : initFn_(std::forward<Init>(init), std::forward<Ts>(args)...) {}

    Awaiter auto operator co_await() && {
        return AsioAwaiter<AsioImpl, InitFn, ThrowOnError, Ret...>(
                std::forward<InitFn>(initFn_));
    }

  private:
    InitFn initFn_;

    friend TypeErasedAsioAwaitable<AsioImpl, ThrowOnError, Ret...>;
};


/// AsioAwaitable is parametrized by its initiation object (as it needs
/// to store it). `asio::async_result<>` does not have initiation among
/// its type parameter list, yet needs to export something under dependent
/// name `return_type`, which is used for asio-related functions still
/// having an explicit return type (like Boost.Beast).
///
/// To accommodate that, we have this class, which stores a type-erased
/// initiation object, and is constructible from `AsioAwaitable`.
template <class AsioImpl, bool ThrowOnError, class... Ret>
class TypeErasedAsioAwaitable {
    using DoneCB = typename AsioAwaiterBase<AsioImpl, Ret...>::DoneCB;

    struct InitFnBase {
        virtual ~InitFnBase() = default;
        virtual void operator()(DoneCB&) = 0;
    };

    template <class Impl> struct InitFnImpl : InitFnBase {
        explicit InitFnImpl(Impl&& impl) : impl_(std::move(impl)) {}
        void operator()(DoneCB& doneCB) override { impl_(doneCB); }

      private:
        CORRAL_NO_UNIQUE_ADDR Impl impl_;
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
    explicit(false) TypeErasedAsioAwaitable(
            AsioAwaitable<AsioImpl, ThrowOnError, Init, Args, Ret...>&& rhs)
      : initFn_(std::move(rhs.initFn_)) {}

    Awaiter auto operator co_await() && {
        return AsioAwaiter<AsioImpl, InitFn, ThrowOnError, Ret...>(
                std::move(initFn_));
    }

  private:
    InitFn initFn_;
};

template <class AsioImpl> struct AsioEventLoopTraitsImpl {
    static EventLoopID eventLoopID(typename AsioImpl::io_context& io) {
        return EventLoopID(&io);
    }

    static void run(typename AsioImpl::io_context& io) {
        io.get_executor().on_work_started();
        io.run();
        io.restart();
    }

    static void stop(typename AsioImpl::io_context& io) {
        io.get_executor().on_work_finished();
        io.stop();
    }
};

template <class AsioImpl> struct AsioThreadNotificationImpl {
    AsioThreadNotificationImpl(typename AsioImpl::io_context&,
                               void (*)(void*),
                               void*) {}

    void post(typename AsioImpl::io_context& io,
              void (*fn)(void*),
              void* arg) noexcept {
        AsioImpl::dispatch(io, [fn, arg] { fn(arg); });
    }
};

template <class AsioImpl, bool ThrowOnError, class... Ret>
struct AsyncResultImpl {
    using return_type = TypeErasedAsioAwaitable<AsioImpl, ThrowOnError, Ret...>;

    /// Use `AsioAwaitable` here, so asio::async_*() functions which
    /// don't use `return_type` and instead have `auto` for their return types
    /// will do without type erase.
    template <class Init, class... Args>
        requires (std::is_move_constructible_v<Args> && ...)
    static auto initiate(Init&& init,
                         asio_awaitable_t<ThrowOnError>,
                         Args... args) {
        return AsioAwaitable<AsioImpl, ThrowOnError, Init, std::tuple<Args...>,
                             Ret...>(std::forward<Init>(init),
                                     std::move(args)...);
    }
};

/// A helper class for `corral::sleepFor()`.
template <class AsioImpl> class AsioTimer {
  public:
    template <class R, class P>
    AsioTimer(typename AsioImpl::io_context& io,
              std::chrono::duration<R, P> delay)
      : timer_(io) {
        timer_.expires_after(delay);
    }

    Awaiter auto operator co_await() {
        return getAwaiter(timer_.async_wait(asio_awaitable));
    }

  private:
    typename AsioImpl::steady_timer timer_;
};

} // namespace detail
} // namespace corral
