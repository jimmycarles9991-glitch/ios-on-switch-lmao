// SPDX-License-Identifier: MIT
// Automatic heaps reserve nonoverlapping ranges until release. Placement heaps validate caller-owned offsets and allow
// immediate aliasing without automatic occupancy accounting. Both stay within the heap's logical size.
#pragma once
#include <cstdint>
#include <iterator>
#include <map>

namespace raw {

class HeapPlacement {
public:
    explicit HeapPlacement(uint64_t bytes = 0, bool explicitPlacement = false)
        : bytes_(bytes), explicitPlacement_(explicitPlacement)
    {
        if (bytes && !explicitPlacement)
            free_[0] = bytes;
    }

    // `align` a power of two
    bool place(uint64_t bytes, uint64_t align, uint64_t &offset)
    {
        if (explicitPlacement_ || !bytes || !align || (align & (align - 1)))
            return false;
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if (it->first > UINT64_MAX - (align - 1))
                continue;
            const uint64_t start = it->first, end = start + it->second, at = (start + align - 1) & ~(align - 1);
            if (at > end || end - at < bytes)
                continue;
            free_.erase(it);
            if (at > start)
                free_[start] = at - start;
            if (at + bytes < end)
                free_[at + bytes] = end - at - bytes;
            used_ += bytes;
            offset = at;
            return true;
        }
        return false;
    }

    // Placement resources may overlap from creation; their owner orders alias use on the GPU.
    bool placeAt(uint64_t bytes, uint64_t align, uint64_t offset) const
    {
        return explicitPlacement_ && bytes && align && !(align & (align - 1)) && !(offset & (align - 1)) &&
               offset <= bytes_ && bytes <= bytes_ - offset;
    }

    void release(uint64_t offset, uint64_t bytes)
    {
        if (explicitPlacement_)
            return;
        used_ -= bytes;
        auto next = free_.lower_bound(offset);
        if (next != free_.end() && offset + bytes == next->first) {
            bytes += next->second;
            next = free_.erase(next);
        }
        if (next != free_.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second == offset) {
                prev->second += bytes;
                return;
            }
        }
        free_[offset] = bytes;
    }

    uint64_t used() const
    {
        return used_;
    }

    uint64_t bytes() const
    {
        return bytes_;
    }

    bool explicitPlacement() const
    {
        return explicitPlacement_;
    }

    // Each free range in offset order: f(offset, bytes)
    template <class F> void forEachFree(F &&f) const
    {
        for (const auto &[start, length] : free_)
            f(start, length);
    }

    // The largest placement `align` allows (maxAvailableSizeWithAlignment)
    uint64_t largestFree(uint64_t align) const
    {
        if (!align || (align & (align - 1)))
            return 0;
        uint64_t best = 0;
        for (const auto &[start, length] : free_) {
            if (start > UINT64_MAX - (align - 1))
                continue;
            const uint64_t at = (start + align - 1) & ~(align - 1);
            if (at < start + length && start + length - at > best)
                best = start + length - at;
        }
        return best;
    }

private:
    uint64_t bytes_ = 0, used_ = 0;
    bool explicitPlacement_ = false;
    std::map<uint64_t, uint64_t> free_;  // offset -> bytes, coalesced
};

}  // namespace raw
