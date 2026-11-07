// SPDX-License-Identifier: BSD-2-Clause-Patent
// First-fit allocation of an engine's IO virtual addresses: a window of 64 KiB granules tracked one bit each in the
// caller's bitmap. Plain state, no IOKit, so the host test drives it (test/smmu_space_test.cpp); TegraSMMUEngine.h
// locks it.
#pragma once
#include <stdint.h>

struct TegraSMMUSpace {
    static constexpr uint64_t kGranule = 64 * 1024;
    uint64_t base = 0;
    uint32_t granules = 0, free = 0;
    uint32_t *used = nullptr;

    static constexpr uint32_t bitmapWords(uint64_t bytes)
    {
        return (uint32_t)((bytes / kGranule + 31) / 32);
    }

    static constexpr uint64_t round(uint64_t bytes)
    {
        return (bytes + kGranule - 1) & ~(kGranule - 1);
    }

    // [windowBase, windowBase + bytes): both granule aligned; bitmap holds bitmapWords(bytes) words.
    bool init(uint64_t windowBase, uint64_t bytes, uint32_t *bitmap, uint32_t words)
    {
        if (!bitmap || !bytes || (windowBase | bytes) & (kGranule - 1) || words < bitmapWords(bytes) ||
            windowBase + bytes > (1ull << 32))
            return false;
        base = windowBase;
        granules = (uint32_t)(bytes / kGranule);
        free = granules;
        used = bitmap;
        for (uint32_t i = 0; i < words; i++)
            used[i] = 0;
        return true;
    }

    bool taken(uint32_t i) const
    {
        return used[i / 32] & (1u << (i % 32));
    }

    void mark(uint32_t first, uint32_t count, bool on)
    {
        for (uint32_t i = first; i < first + count; i++) {
            if (on)
                used[i / 32] |= 1u << (i % 32);
            else
                used[i / 32] &= ~(1u << (i % 32));
        }
    }

    // The lowest free run of `bytes` (rounded up to granules); 0 when none.
    uint64_t alloc(uint64_t bytes)
    {
        if (!used || !bytes || round(bytes) / kGranule > free)
            return 0;
        const uint32_t count = (uint32_t)(round(bytes) / kGranule);
        uint32_t run = 0, first = 0;
        for (uint32_t i = 0; i < granules && run < count;) {
            if (!run && !(i % 32) && used[i / 32] == 0xffffffffu) {
                i += 32;
                continue;
            }
            if (taken(i)) {
                run = 0;
            } else {
                if (!run)
                    first = i;
                run++;
            }
            i++;
        }
        if (run < count)
            return 0;
        mark(first, count, true);
        free -= count;
        return base + (uint64_t)first * kGranule;
    }

    // A range alloc() returned, with the size it was asked for.
    bool release(uint64_t iova, uint64_t bytes)
    {
        if (!used || iova < base || (iova - base) % kGranule)
            return false;
        const uint32_t first = (uint32_t)((iova - base) / kGranule), count = (uint32_t)(round(bytes) / kGranule);
        if (!count || first + count > granules)
            return false;
        for (uint32_t i = first; i < first + count; i++)
            if (!taken(i))
                return false;
        mark(first, count, false);
        free += count;
        return true;
    }

    uint64_t bytesFree() const
    {
        return (uint64_t)free * kGranule;
    }

    uint64_t bytesTotal() const
    {
        return (uint64_t)granules * kGranule;
    }
};
