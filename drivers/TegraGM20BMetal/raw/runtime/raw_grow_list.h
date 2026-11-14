// SPDX-License-Identifier: MIT
// A list one thread appends to while another reads the part of it it was handed (a command buffer's tape, which the submit
// worker encodes while its caller records more). Growth copies the elements into new storage and keeps the old storage
// until clear(), so a reader of an index below what it was handed reads that element from either storage, unchanged. Every
// access loads the storage with acquire, which orders a growth's copy before the reads through it, but the writer's own
// (own(), ownData(), back(), appends), which load it plainly: only the writer grows it. Only the writer writes an element,
// and only one it has not handed over.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <type_traits>
#include <utility>

namespace raw {

template <typename T> class GrowList {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
    static constexpr size_t kOld = 48;  // doubling from one element: more storages than a 64-bit size reaches

public:
    using value_type = T;
    using iterator = T *;
    using const_iterator = const T *;

    GrowList() = default;
    GrowList(const GrowList &) = delete;
    GrowList &operator=(const GrowList &) = delete;

    GrowList(GrowList &&other) noexcept
    {
        take(other);
    }

    GrowList &operator=(GrowList &&other) noexcept
    {
        if (this != &other) {
            release();
            take(other);
        }
        return *this;
    }

    ~GrowList()
    {
        release();
    }

    // The writer's
    size_t size() const
    {
        return size_.load(std::memory_order_relaxed);
    }

    bool empty() const
    {
        return size() == 0;
    }

    size_t capacity() const
    {
        return capacity_;
    }

    // The writer's own accesses
    T &own(size_t i)
    {
        return mine()[i];
    }

    T *ownData()
    {
        return mine();
    }

    T &back()
    {
        return mine()[size() - 1];
    }

    T &operator[](size_t i)
    {
        return const_cast<T *>(seen())[i];
    }

    T *data()
    {
        return const_cast<T *>(seen());
    }

    T *begin()
    {
        return data();
    }

    T *end()
    {
        return data() + size();
    }

    void swap(GrowList &other) noexcept
    {
        GrowList t(std::move(other));
        other = std::move(*this);
        *this = std::move(t);
    }

    void reserve(size_t n)
    {
        if (n > capacity_)
            grow(n);
    }

    void push_back(const T &value)
    {
        const size_t n = size();
        if (n == capacity_)
            grow(n + 1);
        mine()[n] = value;
        size_.store(n + 1, std::memory_order_relaxed);
    }

    template <typename... Args> T &emplace_back(Args &&...args)
    {
        const size_t n = size();
        if (n == capacity_)
            grow(n + 1);
        T *at = mine() + n;
        *at = T{std::forward<Args>(args)...};
        size_.store(n + 1, std::memory_order_relaxed);
        return *at;
    }

    template <typename It> void append(It first, It last)
    {
        const size_t n = size(), more = size_t(std::distance(first, last));
        if (n + more > capacity_)
            grow(n + more);
        std::copy(first, last, mine() + n);
        size_.store(n + more, std::memory_order_relaxed);
    }

    // Shrinks drop the tail; growth value-initialises
    void resize(size_t n, const T &value = T{})
    {
        const size_t had = size();
        if (n > capacity_)
            grow(n);
        if (n > had)
            std::fill(mine() + had, mine() + n, value);
        size_.store(n, std::memory_order_relaxed);
    }

    void pop_back()
    {
        size_.store(size() - 1, std::memory_order_relaxed);
    }

    // The writer's, once no reader holds any of it: the storages outgrown go, the last one stays for the next fill
    void clear()
    {
        size_.store(0, std::memory_order_relaxed);
        for (size_t i = 0; i < olds_; ++i)
            free(old_[i]);
        olds_ = 0;
    }

    const T &operator[](size_t i) const
    {
        return seen()[i];
    }

    const T *data() const
    {
        return seen();
    }

    const T *begin() const
    {
        return seen();
    }

    const T *end() const
    {
        return seen() + size();
    }

    const T &back() const
    {
        return seen()[size() - 1];
    }

    // The bytes it holds, the outgrown storages with them
    size_t storageBytes() const
    {
        return capacity_ * sizeof(T);
    }

private:
    T *mine() const
    {
        return data_.load(std::memory_order_relaxed);
    }

    const T *seen() const
    {
        return data_.load(std::memory_order_acquire);
    }

    void grow(size_t need)
    {
        const size_t capacity = std::max<size_t>({need, capacity_ * 2, 16 / sizeof(T) + 1});
        T *larger = static_cast<T *>(malloc(capacity * sizeof(T)));
        if (!larger)
            abort();
        T *had = mine();
        if (had)
            memcpy(static_cast<void *>(larger), had, size() * sizeof(T));
        if (had) {
            if (olds_ == kOld)
                abort();
            old_[olds_++] = had;
        }
        capacity_ = capacity;
        data_.store(larger, std::memory_order_release);
    }

    void release()
    {
        clear();
        free(mine());
        data_.store(nullptr, std::memory_order_relaxed);
        capacity_ = 0;
    }

    void take(GrowList &other)
    {
        data_.store(other.mine(), std::memory_order_relaxed);
        size_.store(other.size(), std::memory_order_relaxed);
        capacity_ = other.capacity_;
        olds_ = other.olds_;
        std::copy(other.old_, other.old_ + other.olds_, old_);
        other.data_.store(nullptr, std::memory_order_relaxed);
        other.size_.store(0, std::memory_order_relaxed);
        other.capacity_ = 0;
        other.olds_ = 0;
    }

    std::atomic<T *> data_{nullptr};
    std::atomic<size_t> size_{0};
    size_t capacity_ = 0;
    T *old_[kOld] = {};
    size_t olds_ = 0;
};

template <typename T> void swap(GrowList<T> &a, GrowList<T> &b) noexcept
{
    a.swap(b);
}

}  // namespace raw
