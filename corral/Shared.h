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
#include <memory>

#include "detail/ABI.h"
#include "detail/IntrusiveList.h"
#include "detail/IntrusivePtr.h"
#include "detail/utility.h"

namespace corral {

/// Models a shared asynchronous operation: an awaitable of type `Object` that
/// can be awaited multiple times in parallel, modeling a task with
/// multiple parents. The result of the operation will be propagated to each
/// of its parents, and the shared operation will be cancelled if all of its
/// parents are cancelled.
///
/// Cancellation of a parent while other parents remain always
/// succeeds. Cancellation of the last parent becomes cancellation
/// of the shared task, and may complete asynchronously or fail if
/// the shared task can handle cancellation in those ways.
///
/// `Shared` is copyable, and copies reference the same underlying task.
/// There is no difference between running co_await one time on each
/// of N copies, and running co_await N times on one copy.
///
/// New parents that attempt to join after the shared task has been
/// cancelled (due to cancellation of all the parents in the initial
/// batch) will see a std::runtime_error explaining that the value is
/// unavailable due to cancellation; this can also be tested
/// explicitly using the closed() method.
///
/// Behavior is undefined if the shared operation indirectly attempts to
/// await itself. If this occurs, it is possible for cancellations
/// to result in the shared task being the only thing keeping itself
/// alive, which will cause a resource leak or worse.
template <class Object> class Shared {
    using AwaitableObj = detail::AwaitableType<Object&>;
    using ReturnType = decltype(std::declval<AwaitableObj>().await_resume());
    using ConstRef = std::add_lvalue_reference_t<const ReturnType>;
    using Storage = detail::Storage<ReturnType>;

    class State;
    class Awaitable;

  public:
    Shared() = default;
    Shared(Object&& obj);
    template <class... Args>
        requires(std::is_constructible_v<Object, Args...>)
    Shared(std::in_place_t, Args&&... args);

    Object* get() const;
    explicit operator bool() const { return !state_; }
    Object& operator*() const { return *get(); }
    Object* operator->() const { return get(); }

    bool closed() const noexcept;

    corral::Awaitable auto operator co_await();

  private:
    detail::IntrusivePtr<State> state_;
};

/// Awaitable object used for a single co_await on a shared task
template <class Object>
class Shared<Object>::Awaitable : public detail::IntrusiveListItem<Awaitable> {
  public:
    explicit Awaitable(detail::IntrusivePtr<State> state)
      : state_(std::move(state)) {}
    bool await_ready() const noexcept;
    auto await_early_cancel() noexcept;
    void await_set_executor(Executor* ex) noexcept;
    Handle await_suspend(Handle h);
    auto await_cancel(Handle h) noexcept;
    auto await_must_resume() const noexcept;
    ConstRef await_resume();
    void await_introspect(auto& c) const noexcept;

  private:
    void wakeUp() {
        this->unlink();
        std::exchange(parent_, std::noop_coroutine())();
    }
    friend class State;

  private:
    // The shared task state. If null, this awaitable is not associated
    // with any shared task; this can occur when awaiting a moved-from
    // Shared, or after cancellation of an awaitable that is not the last
    // one for its task.
    detail::IntrusivePtr<State> state_;
    Handle parent_;
};

//
// Implementation
//

/// Storage and lifetime management for the shared task underlying a Shared<T>
template <class Object>
class Shared<Object>::State : private detail::ProxyFrame,
                              public detail::RefCounted<State> {
  public:
    template <class... Args> explicit State(Args&&... args);
    Object* get() { return &object_; }
    bool closed() const noexcept { return result_.index() >= Cancelling; }

    bool ready() const noexcept;
    auto earlyCancel(Awaitable* ptr) noexcept;
    void setExecutor(Executor* ex) noexcept;
    Handle suspend(Awaitable* ptr);
    auto cancel(Awaitable* ptr) noexcept;
    auto mustResume() const noexcept;
    ConstRef result();
    void introspect(auto& c) const noexcept;

  private:
    void invoke();
    static void trampoline(detail::CoroutineFrame* frame) {
        static_cast<State*>(frame)->invoke();
    }

  private:
    [[no_unique_address]] Object object_;
    detail::AwaitableAdapter<AwaitableObj> awaitable_;
    detail::IntrusiveList<Awaitable> parents_;
    std::variant<std::monostate,
                 std::monostate,
                 typename Storage::Type,
                 std::exception_ptr,
                 std::monostate,
                 std::monostate>
            result_;

    // Indices of types stored in the variant
    static constexpr const int Incomplete = 0;
    static constexpr const int CancelPending = 1;
    static constexpr const int Value = 2;
    static constexpr const int Exception = 3;
    static constexpr const int Cancelling = 4;
    static constexpr const int Cancelled = 5;
};


template <class Object>
template <class... Args>
Shared<Object>::State::State(Args&&... args)
  : object_(std::forward<Args>(args)...),
    awaitable_(detail::getAwaitable(object_)) {
    this->resumeFn = &State::trampoline;
}

template <class Object>
void Shared<Object>::State::setExecutor(Executor* ex) noexcept {
    if (parents_.empty()) {
        awaitable_.await_set_executor(ex);
    }
}

template <class Object> bool Shared<Object>::State::ready() const noexcept {
    // If we already have some parents, make sure new arrivals don't
    // bypass the queue and try to call result() before the operation
    // officially completes; it's possible that ready() will become true
    // before the handle passed to suspend() is resumed.
    return result_.index() != Incomplete ||
           (parents_.empty() && awaitable_.await_ready());
}

template <class Object>
auto Shared<Object>::State::earlyCancel(Awaitable* ptr) noexcept {
    // The first arriving parent is considered to be responsible for
    // forwarding early cancellation to the shared task. Any parent
    // that arrives after it can safely be skipped without affecting the
    // supervision of the task. If the task already completed, then
    // we know we're not the first arrival, even if there are no
    // parents still registered; one of the previous parents must
    // have retrieved the task's result.
    if (parents_.empty() && result_.index() == Incomplete) {
        // Forward early-cancel request to the shared task
        auto syncEarlyCancelled = awaitable_.await_early_cancel();
        if (syncEarlyCancelled) {
            result_.template emplace<Cancelled>();
            ptr->state_ = nullptr;
        } else {
            result_.template emplace<CancelPending>();
        }
        return syncEarlyCancelled;
    }

    // Skip this parent without affecting the shared task.
    // Match the return type of 'return syncEarlyCancelled;' above.
    ptr->state_ = nullptr;
    if constexpr (detail::Skippable<AwaitableObj>) {
        return std::true_type{};
    } else {
        return true;
    }
}

template <class Object> Handle Shared<Object>::State::suspend(Awaitable* ptr) {
    CORRAL_TRACE("    ...on shared awaitable %p (holding %p)", this,
                 &awaitable_);
    bool isFirst = parents_.empty();
    parents_.push_back(*ptr);
    if (isFirst) {
        // Taking an async backtrace from within a shared task will
        // show its oldest uncancelled parent as the caller.
        ProxyFrame::linkTo(ptr->parent_);
        if (result_.index() == CancelPending) {
            result_.template emplace<Cancelling>();
        }

        try {
            return awaitable_.await_suspend(this->toHandle());
        } catch (...) {
            auto ex = std::current_exception();
            CORRAL_ASSERT(
                    ex &&
                    "foreign exceptions and forced unwinds are not supported");
            result_.template emplace<Exception>(std::move(ex));
            invoke();
            return std::noop_coroutine(); // already woke up
        }
    } else {
        return std::noop_coroutine();
    }
}

template <class Object>
typename Shared<Object>::ConstRef Shared<Object>::State::result() {
    // We can get here with result == CancelPending if early-cancel returned
    // false and the awaitable was then immediately ready. mustResume()
    // was checked already, so treat CancelPending like Incomplete.
    if (result_.index() == Incomplete || result_.index() == CancelPending) {
        try {
            if constexpr (std::is_same_v<ReturnType, void>) {
                std::move(awaitable_).await_resume();
                result_.template emplace<Value>();
            } else {
                result_.template emplace<Value>(
                        Storage::wrap(std::move(awaitable_).await_resume()));
            }
        } catch (...) {
            result_.template emplace<Exception>(std::current_exception());
        }
    }

    if (result_.index() == Value) [[likely]] {
        return Storage::unwrapCRef(std::get<Value>(result_));
    } else if (result_.index() == Exception) {
        std::rethrow_exception(std::get<Exception>(result_));
    } else {
        // We get here if a new parent tries to join the shared operation
        // after all of its existing parents were cancelled and
        // thus the shared task was cancelled. The new parent never
        // called suspend() so we don't have to worry about removing
        // it from the list of parents. We can't propagate the
        // cancellation in a different context than the context that
        // was cancelled, so we throw an exception instead.
        throw std::runtime_error(
                "Shared task was cancelled because all of its parent "
                "tasks were previously cancelled, so there is no "
                "value for new arrivals to retrieve");
    }
}

template <class Object>
auto Shared<Object>::State::cancel(Awaitable* ptr) noexcept {
    if (parents_.contains_one_item()) {
        CORRAL_TRACE("cancelling shared awaitable %p (holding %p); "
                     "forwarding cancellation",
                     this, &awaitable_);
        CORRAL_ASSERT(&parents_.front() == ptr);
        // Prevent new parents from joining, and forward the cancellation
        // to the shared task
        result_.template emplace<Cancelling>();
        auto syncCancelled = awaitable_.await_cancel(this->toHandle());
        if (syncCancelled) {
            result_.template emplace<Cancelled>();
            ptr->unlink();
            ptr->state_ = nullptr;
        }
        return syncCancelled;
    } else {
        // Note that we also get here if parents_ is empty, which can
        // occur if the resumption of one parent cancels another
        // (imagine corral::anyOf() on multiple copies of the same
        // Shared<T>). We're still linked into the list, it's just a
        // local variable in invoke(). We'll let these additional
        // parents propagate cancellation and assume that the first
        // one will do a good enough job of carrying the value. This
        // is important to allow Shared<T> to be abortable/disposable
        // if T is.
        CORRAL_TRACE("cancelling shared awaitable %p (holding %p); "
                     "dropping parent",
                     this, &awaitable_);
        ptr->unlink();
        ptr->state_ = nullptr;

        // If the cancelled parent was previously the first one, then
        // we should choose a new first one to avoid backtracing into
        // something dangling. (If we have no parents_, then the
        // shared task has completed, so it doesn't matter what it
        // declares as its caller.)
        if (!parents_.empty()) {
            ProxyFrame::linkTo(parents_.front().parent_);
        }

        // Match the type of 'return syncCancelled;' above:
        if constexpr (detail::Abortable<AwaitableObj>) {
            return std::true_type{};
        } else {
            return true;
        }
    }
}

template <class Object>
auto Shared<Object>::State::mustResume() const noexcept {
    // This is called after an individual parent's cancellation did not
    // succeed synchronously. Early cancellation of not-the-first parent,
    // and regular cancellation of not-the-last parent, always succeed
    // synchronously and will not enter this function.
    //
    // If regular cancellation of the last parent didn't succeed
    // synchronously, we would have set result_ to Cancelling and
    // invoke() would have clarified that to either Incomplete or
    // Cancelled based on the underlying await_must_resume() by the
    // time we got here. We return true for Incomplete to prompt a
    // call to result() that will fill in the Value or Exception.
    //
    // If early cancellation of the first parent didn't succeed
    // synchronously, we would have set result_ to CancelPending.
    // suspend() transforms that to Cancelling which feeds into the
    // regular-cancel case above once the shared task completes. But
    // if the awaitable was immediately ready() after a
    // non-synchronous earlyCancel(), we get here with CancelPending
    // still set, and need to check the underlying await_must_resume().
    bool ret = result_.index() == CancelPending ? awaitable_.await_must_resume()
                                                : result_.index() != Cancelled;
    if constexpr (detail::CancelAlwaysSucceeds<AwaitableObj>) {
        CORRAL_ASSERT(ret == false);
        return std::false_type{};
    } else {
        return ret;
    }
}

template <class Object>
void Shared<Object>::State::introspect(auto& c) const noexcept {
    switch (result_.index()) {
        case Cancelling:
            c.footnote("(cancelling:)");
            [[fallthrough]];
        case Incomplete:
            c.child(awaitable_);
            break;
        default:
            c.footnote("<complete>");
            break;
    }
}

template <class Object> void Shared<Object>::State::invoke() {
    CORRAL_TRACE("shared awaitable %p (holding %p) resumed", this, &awaitable_);
    if (result_.index() == Cancelling) {
        if (awaitable_.await_must_resume()) {
            result_.template emplace<Incomplete>();
        } else {
            result_.template emplace<Cancelled>();
        }
    }
    auto parents = std::move(parents_);
    while (!parents.empty()) {
        parents.front().wakeUp();
    }
}

template <class Object>
void Shared<Object>::Awaitable::await_set_executor(Executor* ex) noexcept {
    if (state_) {
        state_->setExecutor(ex);
    }
}

template <class Object>
bool Shared<Object>::Awaitable::await_ready() const noexcept {
    return !state_ || state_->ready();
}

template <class Object>
auto Shared<Object>::Awaitable::await_early_cancel() noexcept {
    return state_ ? state_->earlyCancel(this) : std::true_type{};
}

template <class Object>
Handle Shared<Object>::Awaitable::await_suspend(Handle h) {
    parent_ = h;
    return state_->suspend(this);
}

template <class Object>
typename Shared<Object>::ConstRef Shared<Object>::Awaitable::await_resume() {
    if (!state_) {
        if constexpr (std::is_same_v<ReturnType, void>) {
            return;
        } else {
            CORRAL_ASSERT(!"co_await on an empty shared");
        }
    }
    return state_->result();
}

template <class Object>
auto Shared<Object>::Awaitable::await_cancel(Handle) noexcept {
    return state_ ? state_->cancel(this) : std::true_type{};
}

template <class Object>
auto Shared<Object>::Awaitable::await_must_resume() const noexcept {
    return state_ ? state_->mustResume() : std::false_type{};
}

template <class Object>
void Shared<Object>::Awaitable::await_introspect(auto& c) const noexcept {
    c.node("Shared::Awaitable");
    if (state_) {
        state_->introspect(c);
    } else {
        c.footnote("<detached>");
    }
}

template <class Object>
Shared<Object>::Shared(Object&& obj)
  : state_(new State(std::forward<Object>(obj))) {}

template <class Object>
template <class... Args>
    requires(std::is_constructible_v<Object, Args...>)
Shared<Object>::Shared(std::in_place_t, Args&&... args)
  : state_(new State(std::forward<Args>(args)...)) {}

template <class Object> bool Shared<Object>::closed() const noexcept {
    return state_ ? state_->closed() : true;
}

template <class Object> Object* Shared<Object>::get() const {
    return state_ ? state_->get() : nullptr;
}

template <class Object> //
corral::Awaitable auto Shared<Object>::operator co_await() {
    return Awaitable(state_);
}

} // namespace corral
