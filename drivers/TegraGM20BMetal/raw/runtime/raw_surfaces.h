// SPDX-License-Identifier: MIT
// Exact backing uses for one job: an oversized list is refused, never cut short.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "TegraGM20BABI.h"

namespace raw {
enum class JobVisibility : uint8_t {
    Legacy,
    Internal,
    External
};

class SurfaceList {
public:
    void clear()
    {
        uses_.clear();
        ranges_.clear();
        extents_.clear();
        index_.fill(0);
        overflow_ = false;
        last_ = 0;
    }

    void add(uint64_t handle, bool write)
    {
        addAccess(handle, write ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead);
    }

    void addAddress(uint64_t va, uint64_t bytes, bool write, uint32_t metadata = 0)
    {
        if (!bytes || bytes > UINT64_MAX - va || ((metadata & TegraGM20BSurfaceRange) && bytes > UINT32_MAX)) {
            overflow_ = true;
            return;
        }
        const uint32_t access =
            TegraGM20BSurfaceGpuAddress | (write ? TegraGM20BSurfaceWrite : TegraGM20BSurfaceRead) | metadata;
        const size_t slot = findSlot(va, TegraGM20BSurfaceGpuAddress);
        if (index_[slot]) {
            last_ = index_[slot] - 1;
            const uint64_t previous = extents_[last_];
            mergeAddress(last_, access, bytes);
            if (bytes <= previous)
                return;
            extents_[last_] = bytes;
            if (bytes > 1)
                ranges_.push_back({va, va + bytes, last_});
            return;
        }
        for (const auto &range : ranges_)
            if (va >= range.begin && va + bytes <= range.end) {
                mergeAddress(range.index, access, extents_[range.index]);
                return;
            }
        addAccess(va, access);
        if (valid()) {
            mergeAddress(last_, access, bytes);
            if (bytes > 1)
                ranges_.push_back({va, va + bytes, last_});
        }
    }

    void addAccess(uint64_t handle, uint32_t access)
    {
        if (!handle) {
            overflow_ = true;
            return;
        }
        const uint32_t kind = access & TegraGM20BSurfaceGpuAddress;
        if (last_ < uses_.size() && uses_[last_].handle == handle &&
            (uses_[last_].access & TegraGM20BSurfaceGpuAddress) == kind) {
            if ((access & TegraGM20BSurfaceWrite) && !(access & TegraGM20BSurfaceRange)) {
                uses_[last_].access &= ~uint32_t(TegraGM20BSurfaceRange);
                uses_[last_].reserved = 0;
            }
            uses_[last_].access |= access;
            return;
        }
        const size_t slot = findSlot(handle, kind);
        if (index_[slot]) {
            last_ = index_[slot] - 1;
            if ((access & TegraGM20BSurfaceWrite) && !(access & TegraGM20BSurfaceRange)) {
                uses_[last_].access &= ~uint32_t(TegraGM20BSurfaceRange);
                uses_[last_].reserved = 0;
            }
            uses_[last_].access |= access;
            return;
        }
        if (uses_.size() == TegraGM20BMaxJobUses) {
            overflow_ = true;
            return;
        }
        last_ = uses_.size();
        uses_.push_back({handle, access, 0});
        extents_.push_back(0);
        index_[slot] = uint16_t(last_ + 1);
    }

    bool valid() const
    {
        return !overflow_;
    }

    const std::vector<TegraGM20BAbiSurfaceUse> &uses() const
    {
        return uses_;
    }

    // Ownership is per backing, including every pooled suballocation. A CPU geometry reader must not coexist
    // with a write in the same job. Physical aliases under another handle are additionally checked by the kernel.
    bool writesBacking(uint64_t handle, uint64_t va, uint64_t bytes) const
    {
        if (!valid())
            return true;
        for (const auto &use : uses_) {
            if (!(use.access & TegraGM20BSurfaceWrite))
                continue;
            if (use.access & TegraGM20BSurfaceGpuAddress) {
                if (use.handle >= va && use.handle - va < bytes)
                    return true;
            } else if (use.handle == handle) {
                return true;
            }
        }
        return false;
    }

private:
    void mergeAddress(size_t index, uint32_t access, uint64_t bytes)
    {
        auto &use = uses_[index];
        const bool oldWrite = (use.access & TegraGM20BSurfaceWrite) != 0;
        const bool addedWrite = (access & TegraGM20BSurfaceWrite) != 0;
        const bool oldExact = (use.access & TegraGM20BSurfaceRange) != 0;
        const bool addedExact = (access & TegraGM20BSurfaceRange) != 0;
        const bool exact = addedWrite ? addedExact && (!oldWrite || oldExact)
                           : oldWrite ? oldExact
                                      : oldExact || addedExact;
        extents_[index] = std::max(extents_[index], bytes);
        use.access = (use.access | access) & ~uint32_t(TegraGM20BSurfaceRange);
        use.reserved = 0;
        if (exact) {
            if (extents_[index] > UINT32_MAX) {
                overflow_ = true;
                return;
            }
            use.access |= TegraGM20BSurfaceRange;
            use.reserved = uint32_t(extents_[index]);
        }
    }

    static constexpr size_t kIndexSize = 2 * TegraGM20BMaxJobUses;
    static_assert((kIndexSize & (kIndexSize - 1)) == 0 && TegraGM20BMaxJobUses < UINT16_MAX);

    size_t findSlot(uint64_t handle, uint32_t kind) const
    {
        uint64_t hash = handle ^ (uint64_t(kind) << 61);
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdULL;
        hash ^= hash >> 33;
        hash *= 0xc4ceb9fe1a85ec53ULL;
        hash ^= hash >> 33;
        size_t slot = hash & (kIndexSize - 1);
        while (index_[slot]) {
            const auto &use = uses_[index_[slot] - 1];
            if (use.handle == handle && (use.access & TegraGM20BSurfaceGpuAddress) == kind)
                break;
            slot = (slot + 1) & (kIndexSize - 1);
        }
        return slot;
    }

    struct Range {
        uint64_t begin, end;
        size_t index;
    };

    std::array<uint16_t, kIndexSize> index_{};
    std::vector<uint64_t> extents_;
    std::vector<Range> ranges_;
    std::vector<TegraGM20BAbiSurfaceUse> uses_;
    size_t last_ = 0;
    bool overflow_ = false;
};
}  // namespace raw
