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

namespace corral::detail {

template <class T> class IntrusiveList;
template <class T, class Item> class IntrusiveIterator;

template <class T> class IntrusiveListItem {
  public:
    ~IntrusiveListItem() {
        static_assert(std::is_base_of_v<IntrusiveListItem<T>, T>);
        unlink();
    }

  protected:
    void unlink() {
        if (next_ && prev_) {
            next_->prev_ = prev_;
            prev_->next_ = next_;
        }
        next_ = prev_ = nullptr;
    }

  private:
    friend class IntrusiveList<T>;
    template <class, class> friend class IntrusiveIterator;

    IntrusiveListItem<T>* next_ = nullptr;
    IntrusiveListItem<T>* prev_ = nullptr;
};


template <class T, class Item> class IntrusiveIterator {
    Item* ptr_ = nullptr;

    T* get() const { return static_cast<T*>(ptr_); }

  public:
    using iterator_category = std::forward_iterator_tag;
    using value_type = T;
    using difference_type = std::size_t;

    IntrusiveIterator() = default;
    explicit IntrusiveIterator(Item* ptr) : ptr_(ptr) {}

    T& operator*() const { return *get(); }
    T* operator->() const { return get(); }

    IntrusiveIterator& operator++() {
        ptr_ = ptr_->next_;
        return *this;
    }
    IntrusiveIterator operator++(int) {
        IntrusiveIterator copy = *this;
        ++*this;
        return copy;
    }

    bool operator==(const IntrusiveIterator& rhs) const {
        return ptr_ == rhs.ptr_;
    }
};


template <class T> class IntrusiveList : private IntrusiveListItem<T> {
  public:
    IntrusiveList() { this->next_ = this->prev_ = this; }

    IntrusiveList(IntrusiveList&& rhs) noexcept {
        if (rhs.empty()) {
            this->next_ = this->prev_ = this;
        } else {
            IntrusiveListItem<T>* rhsHook = &rhs;
            this->next_ = std::exchange(rhs.next_, rhsHook);
            this->prev_ = std::exchange(rhs.prev_, rhsHook);

#if (__GNUC__ >= 12)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdangling-pointer="
#endif
            this->next_->prev_ = this->prev_->next_ = this;
#if (__GNUC__ >= 12)
#pragma GCC diagnostic pop
#endif
        }
    }

    IntrusiveList& operator=(IntrusiveList&& rhs) noexcept = delete;
    ~IntrusiveList() { clear(); }

    void clear() {
        while (!empty()) {
            this->next_->unlink();
        }
    }

    bool empty() const { return this->next_ == this; }

    bool contains_one_item() const {
        return !empty() && this->next_->next_ == this;
    }

    T& front() { return *static_cast<T*>(this->next_); }
    const T& front() const { return *static_cast<const T*>(this->next_); }
    void pop_front() { this->next_->unlink(); }

    void erase(T& item) { item.unlink(); }

    void push_back(T& item) {
        item.unlink();
        item.next_ = this;
        item.prev_ = this->prev_;
        this->prev_->next_ = &item;
        this->prev_ = &item;
    }

    using iterator = IntrusiveIterator<T, IntrusiveListItem<T>>;
    iterator begin() { return iterator(this->next_); }
    iterator end() { return iterator(this); }

    using const_iterator =
            IntrusiveIterator<const T, const IntrusiveListItem<T>>;
    const_iterator begin() const { return const_iterator(this->next_); }
    const_iterator end() const { return const_iterator(this); }
};

} // namespace corral::detail
