// SPDX-License-Identifier: MIT
#pragma once
#include <cstddef>
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

namespace raw {

// A map whose clear keeps its storage: a command buffer's recording names the keys of the last one again, and inserts
// them with no allocation once the table has grown to a frame's.
template <typename Key, typename V, typename Hash> class StampMap {
public:
    V *find(const Key &key)
    {
        if (slots_.empty())
            return nullptr;
        Slot &slot = slots_[at(key)];
        return slot.stamp == stamp_ ? &slot.value : nullptr;
    }

    const V *find(const Key &key) const
    {
        return const_cast<StampMap *>(this)->find(key);
    }

    // The value at `key`, `value` put there where it had none (true then)
    std::pair<V *, bool> insert(const Key &key, const V &value)
    {
        if ((size_ + 1) * 2 > slots_.size())
            grow();
        Slot &slot = slots_[at(key)];
        if (slot.stamp == stamp_)
            return {&slot.value, false};
        slot = {key, stamp_, value};
        ++size_;
        return {&slot.value, true};
    }

    void clear()
    {
        size_ = 0;
        if (++stamp_)
            return;
        for (Slot &slot : slots_)
            slot.stamp = 0;
        stamp_ = 1;
    }

    size_t size() const
    {
        return size_;
    }

    size_t storageBytes() const
    {
        return slots_.capacity() * sizeof(Slot);
    }

    template <typename F> void forEach(F visit) const
    {
        for (const Slot &slot : slots_)
            if (slot.stamp == stamp_)
                visit(slot.key, slot.value);
    }

private:
    struct Slot {
        Key key{};
        uint32_t stamp = 0;
        V value{};
    };

    // The slot of `key`, or the empty one its probe reached
    size_t at(const Key &key) const
    {
        const size_t mask = slots_.size() - 1;
        const size_t hash = Hash{}(key);
        size_t i = hash & mask;
        while (slots_[i].stamp == stamp_ && !(slots_[i].key == key))
            i = (i + 1) & mask;
        return i;
    }

    void grow()
    {
        std::vector<Slot> old = std::move(slots_);
        slots_.assign(old.empty() ? 64 : old.size() * 2, Slot{});
        const uint32_t live = stamp_;
        stamp_ = 1;
        for (Slot &slot : old)
            if (slot.stamp == live)
                slots_[at(slot.key)] = {slot.key, stamp_, slot.value};
    }

    std::vector<Slot> slots_;
    uint32_t stamp_ = 1;
    size_t size_ = 0;
};

struct PointerHash {
    size_t operator()(const void *key) const
    {
        return size_t((uint64_t(reinterpret_cast<uintptr_t>(key)) >> 4) * 0x9e3779b97f4a7c15ull);
    }
};

// A key of a few integers, each mixed into the next (splitmix64's finalizer): the table takes the hash's low bits
struct WordsHash {
    template <typename... T> size_t operator()(const std::tuple<T...> &key) const
    {
        uint64_t h = 0;
        std::apply(
            [&](const auto &...word) {
                ((h = mix(h ^ uint64_t(word))), ...);
            },
            key);
        return size_t(h);
    }

    template <typename A, typename B> size_t operator()(const std::pair<A, B> &key) const
    {
        return (*this)(std::make_tuple(key.first, key.second));
    }

private:
    static uint64_t mix(uint64_t x)
    {
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
        return x ^ (x >> 31);
    }
};

template <typename V> using PointerMap = StampMap<const void *, V, PointerHash>;

}  // namespace raw
