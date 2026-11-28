// SPDX-License-Identifier: BSD-2-Clause-Patent
// First-fit allocation of an engine's IO virtual addresses: a window of 64 KiB granules tracked one bit each in the
// caller's bitmap. Plain state, no IOKit, so the host test drives it (test/smmu_space_test.cpp); TegraSMMUEngine.h
// locks it.
#pragma once
#include <stdint.h>

struct TegraSMMUSpace {
    static constexpr uint64_t kGranule = 64ULL * 1024;
    uint64_t base = 0;
    uint32_t granules = 0, free = 0, cursor = 0;
    uint32_t *used = nullptr;

    static constexpr uint32_t bitmapWords(uint64_t bytes)
    {
        return static_cast<uint32_t>((bytes / kGranule + 31) / 32);
    }

    static constexpr uint64_t round(uint64_t bytes)
    {
        return (bytes + kGranule - 1) & ~(kGranule - 1);
    }

    // [windowBase, windowBase + bytes): both granule aligned; bitmap holds bitmapWords(bytes) words.
    bool init(uint64_t windowBase, uint64_t bytes, uint32_t *bitmap, uint32_t words)
    {
        if (!bitmap || !bytes || (windowBase | bytes) & (kGranule - 1) || words < bitmapWords(bytes) ||
            windowBase + bytes > (1ULL << 32))
            return false;
        base = windowBase;
        granules = static_cast<uint32_t>(bytes / kGranule);
        free = granules;
        cursor = 0;
        used = bitmap;
        for (uint32_t i = 0; i < words; i++)
            used[i] = 0;
        return true;
    }

    bool taken(uint32_t i) const
    {
        return (used[i / 32] & (1u << (i % 32))) != 0u;
    }

    // NOLINTNEXTLINE(readability-make-member-function-const): marks the space's bitmap, through its pointer
    void mark(uint32_t first, uint32_t count, bool on)
    {
        uint32_t i = first;
        const uint32_t end = first + count;
        while (i < end) {
            const uint32_t bit = i & 31u;
            const uint32_t rem = end - i;
            const uint32_t n = rem < (32u - bit) ? rem : (32u - bit);
            const uint32_t mask = n == 32u ? 0xffffffffu : (((1u << n) - 1u) << bit);
            if (on)
                used[i >> 5] |= mask;
            else
                used[i >> 5] &= ~mask;
            i += n;
        }
    }

    // The lowest free run of `bytes` (rounded up to granules) leaving at least `reserveBytes` free; 0 when none.
    uint64_t alloc(uint64_t bytes, uint64_t reserveBytes = 0)
    {
        if (!used || !bytes)
            return 0;
        const uint64_t rounded = round(bytes);
        const uint64_t avail = bytesFree();
        if (rounded < bytes || avail < rounded || avail - rounded < reserveBytes)
            return 0;
        const uint32_t count = static_cast<uint32_t>(rounded / kGranule);
        uint32_t run = 0;
        uint32_t first = 0;
        uint32_t lowestFree = granules;
        bool seenFree = false;
        for (uint32_t i = cursor; i < granules && run < count;) {
            const uint32_t wordIdx = i >> 5;
            const uint32_t bit = i & 31u;
            const uint32_t w = used[wordIdx];
            if (!bit) {
                if (w == 0xffffffffu) {
                    run = 0;
                    i += 32;
                    continue;
                }
                if (w == 0u) {
                    if (!seenFree) {
                        seenFree = true;
                        lowestFree = i;
                    }
                    if (!run)
                        first = i;
                    const uint32_t availWord = granules - i < 32u ? granules - i : 32u;
                    const uint32_t take = count - run < availWord ? count - run : availWord;
                    run += take;
                    i += take;
                    continue;
                }
            }
            if (!run) {
                const uint32_t freeBits = (~w) & (~0u << bit);
                if (!freeBits) {
                    i = (wordIdx + 1u) << 5;
                    continue;
                }
                i = (wordIdx << 5) + static_cast<uint32_t>(__builtin_ctz(freeBits));
                if (i >= granules)
                    break;
                if (!seenFree) {
                    seenFree = true;
                    lowestFree = i;
                }
                first = i;
            }
            const uint32_t curBit = i & 31u;
            const uint32_t takenAbove = w >> curBit;
            if (takenAbove & 1u) {
                run = 0;
                const uint32_t freeAbove = (~takenAbove);
                if (!freeAbove) {
                    i = (wordIdx + 1u) << 5;
                } else {
                    i += static_cast<uint32_t>(__builtin_ctz(freeAbove));
                }
                continue;
            }
            const uint32_t freeRun = takenAbove ? static_cast<uint32_t>(__builtin_ctz(takenAbove)) : (32u - curBit);
            const uint32_t limit = granules - i < freeRun ? granules - i : freeRun;
            const uint32_t take = count - run < limit ? count - run : limit;
            run += take;
            i += take;
        }
        if (run < count) {
            if (seenFree)
                cursor = lowestFree;
            return 0;
        }
        mark(first, count, true);
        free -= count;
        cursor = first == lowestFree ? first + count : lowestFree;
        return base + static_cast<uint64_t>(first) * kGranule;
    }

    // A range alloc() returned, with the size it was asked for.
    bool release(uint64_t iova, uint64_t bytes)
    {
        if (!used || iova < base || (iova - base) % kGranule)
            return false;
        const uint32_t first = static_cast<uint32_t>((iova - base) / kGranule);
        const uint32_t count = static_cast<uint32_t>(round(bytes) / kGranule);
        if (!count || first + count > granules)
            return false;
        for (uint32_t i = first; i < first + count; i++) {
            if (!taken(i))
                return false;
        }
        mark(first, count, false);
        free += count;
        if (first < cursor)
            cursor = first;
        return true;
    }

    uint64_t bytesFree() const
    {
        return static_cast<uint64_t>(free) * kGranule;
    }

    uint64_t bytesTotal() const
    {
        return static_cast<uint64_t>(granules) * kGranule;
    }
};
