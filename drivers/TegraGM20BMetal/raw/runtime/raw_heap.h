// SPDX-License-Identifier: MIT
// Where an MTLHeap's resources lie in its one span (Device::arenaTexture, arenaBuffer): first fit at the resource's alignment,
// a range back at once when its resource dies or is made aliasable, which is Metal's heap contract (makeAliasable: later
// placements may take the memory). What is placed never exceeds the heap's size, so a full heap answers nil, as Metal's does.
#pragma once
#include <cstdint>
#include <iterator>
#include <map>

namespace raw {

class HeapPlacement {
public:
    explicit HeapPlacement(uint64_t bytes = 0) : bytes_(bytes)
    {
        if (bytes)
            free_[0] = bytes;
    }

    // `align` a power of two
    bool place(uint64_t bytes, uint64_t align, uint64_t &offset)
    {
        if (!bytes)
            return false;
        for (auto it = free_.begin(); it != free_.end(); ++it) {
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

    void release(uint64_t offset, uint64_t bytes)
    {
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

    // The largest placement `align` allows (maxAvailableSizeWithAlignment)
    uint64_t largestFree(uint64_t align) const
    {
        uint64_t best = 0;
        for (const auto &[start, length] : free_) {
            const uint64_t at = (start + align - 1) & ~(align - 1);
            if (at < start + length && start + length - at > best)
                best = start + length - at;
        }
        return best;
    }

private:
    uint64_t bytes_ = 0, used_ = 0;
    std::map<uint64_t, uint64_t> free_;  // offset -> bytes, coalesced
};

}  // namespace raw
