// SPDX-License-Identifier: MIT
// Automatic heaps reserve nonoverlapping ranges until release. Placement heaps validate caller-owned offsets and allow
// immediate aliasing without automatic occupancy accounting. Both stay within the heap's logical size and record the
// PTE kind of each placed range so aliasing across kinds triggers an L2 kind switch at use time.
#pragma once
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <map>
#include <utility>

namespace gm20b {

// Coalesced GOB-aligned intervals carrying the PTE kind (and an optional generation stamp) of each range's last placement
// or use. Two resources whose placements share a 512-byte GOB under different PTE kinds overlap here; side-by-side
// block-aligned resources do not.
class KindRanges {
public:
    static constexpr uint64_t kGobBytes = 512;

    struct Span {
        uint64_t bytes = 0;
        uint32_t kind = 0;
        uint64_t stamp = 0;
    };

    static std::pair<uint64_t, uint64_t> gobRange(uint64_t offset, uint64_t bytes)
    {
        if (!bytes)
            return {0, 0};
        const uint64_t lo = offset & ~(kGobBytes - 1);
        const uint64_t end = offset > UINT64_MAX - bytes ? UINT64_MAX : offset + bytes;
        const uint64_t hi = end > UINT64_MAX - (kGobBytes - 1) ? UINT64_MAX : (end + kGobBytes - 1) & ~(kGobBytes - 1);
        return {lo, hi};
    }

    bool overlapsOther(uint64_t offset, uint64_t bytes, uint32_t kind, uint64_t minStamp = 0) const
    {
        const auto [lo, hi] = gobRange(offset, bytes);
        if (lo >= hi || spans_.empty())
            return false;
        auto it = spans_.lower_bound(lo);
        if (it != spans_.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second.bytes > lo)
                it = prev;
        }
        for (; it != spans_.end() && it->first < hi; ++it)
            if (it->second.kind != kind && it->second.stamp >= minStamp)
                return true;
        return false;
    }

    bool record(uint64_t offset, uint64_t bytes, uint32_t kind, uint64_t stamp = 0)
    {
        const auto [lo, hi] = gobRange(offset, bytes);
        if (lo >= hi)
            return false;
        bool crossed = false;
        auto it = spans_.lower_bound(lo);
        if (it != spans_.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second.bytes > lo)
                it = prev;
        }
        while (it != spans_.end() && it->first < hi) {
            const uint64_t s = it->first, e = s + it->second.bytes;
            const Span cur = it->second;
            if (cur.kind != kind)
                crossed = true;
            it = spans_.erase(it);
            if (s < lo)
                spans_[s] = {lo - s, cur.kind, cur.stamp};
            if (e > hi)
                spans_[hi] = {e - hi, cur.kind, cur.stamp};
        }
        uint64_t insLo = lo, insHi = hi;
        auto next = spans_.lower_bound(insLo);
        if (next != spans_.end() && next->first == insHi && next->second.kind == kind && next->second.stamp == stamp) {
            insHi += next->second.bytes;
            next = spans_.erase(next);
        }
        if (next != spans_.begin()) {
            auto prev = std::prev(next);
            if (prev->first + prev->second.bytes == insLo && prev->second.kind == kind && prev->second.stamp == stamp) {
                prev->second.bytes = insHi - prev->first;
                return crossed;
            }
        }
        spans_[insLo] = {insHi - insLo, kind, stamp};
        return crossed;
    }

    void pruneBefore(uint64_t minStamp)
    {
        for (auto it = spans_.begin(); it != spans_.end();)
            it = it->second.stamp < minStamp ? spans_.erase(it) : std::next(it);
    }

    bool empty() const
    {
        return spans_.empty();
    }

    void clear()
    {
        spans_.clear();
    }

    template <class F> void each(F &&f) const
    {
        for (const auto &[start, span] : spans_)
            f(start, span.bytes, span.kind, span.stamp);
    }

private:
    std::map<uint64_t, Span> spans_;
};

class HeapPlacement {
public:
    explicit HeapPlacement(uint64_t bytes = 0, bool explicitPlacement = false)
        : bytes_(bytes), explicitPlacement_(explicitPlacement)
    {
        if (bytes && !explicitPlacement)
            free_[0] = bytes;
    }

    // `align` a power of two; `kind` is the PTE kind of the view the placed resource uses
    bool place(uint64_t bytes, uint64_t align, uint64_t &offset, uint32_t kind = 0, bool *crossedKind = nullptr)
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
            const bool crossed = kinds_.record(at, bytes, kind);
            multiKind_ |= crossed;
            if (crossedKind)
                *crossedKind = crossed;
            return true;
        }
        return false;
    }

    // Placement resources may overlap from creation; their owner orders alias use on the GPU.
    bool placeAt(uint64_t bytes, uint64_t align, uint64_t offset, uint32_t kind = 0, bool *crossedKind = nullptr)
    {
        if (!explicitPlacement_ || !bytes || !align || (align & (align - 1)) || (offset & (align - 1)) ||
            offset > bytes_ || bytes > bytes_ - offset)
            return false;
        const bool crossed = kinds_.record(offset, bytes, kind);
        multiKind_ |= crossed;
        if (crossedKind)
            *crossedKind = crossed;
        return true;
    }

    bool multiKind() const
    {
        return multiKind_;
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
    bool explicitPlacement_ = false, multiKind_ = false;
    std::map<uint64_t, uint64_t> free_;  // offset -> bytes, coalesced
    KindRanges kinds_;
};

}  // namespace gm20b
