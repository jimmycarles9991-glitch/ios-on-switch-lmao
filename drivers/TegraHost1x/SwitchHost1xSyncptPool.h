// SPDX-License-Identifier: BSD-2-Clause-Patent
// The syncpoints TegraHost1x hands out (tegra_host1x_syncpt_alloc, _free): host1x's 192 (T210 TRM 14.2), less the ones
// reserved. Allocation never sleeps or locks (a scan and a compare-and-swap),
// so any context may allocate.
//
// Order: every id never handed out comes first, scanned from kFirstHandedOut (so the first 32 are the ones every boot
// used before the pool grew); after that, freed ids in the order they were freed. A freed id comes back only once every
// other free id has been handed out, which keeps it away from a late reader of its last owner for as long as the pool
// can. The value is never reset: each owner reads its base fresh.
#pragma once
#include <stdint.h>

struct SwitchHost1xSyncptPool {
    static constexpr uint32_t kSyncpts = 192, kWords = kSyncpts / 64, kFirstHandedOut = 160;

    uint64_t taken[kWords] = {};      // handed out now, or never to be (reserved)
    uint64_t owned[kWords] = {};      // handed out now
    uint64_t ever[kWords] = {};       // handed out since boot: SC7 saves and restores these
    uint64_t freedAt[kSyncpts] = {};  // when a free id was last freed (freeCount then); 0: never handed out
    uint64_t freeCount = 0;
    uint32_t inUse = 0, reserved = 0;

    static uint64_t bit(uint32_t id)
    {
        return 1ULL << (id % 64);
    }

    // Never handed out. False when it already is: the caller is too late to keep it out.
    bool reserve(uint32_t id)
    {
        if (id >= kSyncpts)
            return false;
        const uint64_t was = __atomic_fetch_or(&taken[id / 64], bit(id), __ATOMIC_ACQ_REL);
        if ((was & bit(id)) != 0)
            return (__atomic_load_n(&owned[id / 64], __ATOMIC_ACQUIRE) & bit(id)) == 0;
        __atomic_fetch_add(&reserved, 1, __ATOMIC_RELAXED);
        return true;
    }

    // The free id freed longest ago (never handed out counts as earliest), claimed; a racing claim makes it look again.
    bool alloc(uint32_t *id)
    {
        for (;;) {
            uint32_t best = kSyncpts;
            uint64_t bestAt = 0;
            for (uint32_t step = 0; step < kSyncpts; ++step) {
                const uint32_t at = (kFirstHandedOut + step) % kSyncpts;
                if ((__atomic_load_n(&taken[at / 64], __ATOMIC_ACQUIRE) & bit(at)) != 0)
                    continue;
                const uint64_t freed = __atomic_load_n(&freedAt[at], __ATOMIC_RELAXED);
                if (best == kSyncpts || freed < bestAt) {
                    best = at;
                    bestAt = freed;
                }
            }
            if (best == kSyncpts)
                return false;
            uint64_t word = __atomic_load_n(&taken[best / 64], __ATOMIC_ACQUIRE);
            if ((word & bit(best)) != 0 || !__atomic_compare_exchange_n(&taken[best / 64], &word, word | bit(best),
                                                                        false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                continue;
            __atomic_fetch_or(&owned[best / 64], bit(best), __ATOMIC_RELEASE);
            __atomic_fetch_or(&ever[best / 64], bit(best), __ATOMIC_RELEASE);
            __atomic_add_fetch(&inUse, 1, __ATOMIC_RELAXED);
            *id = best;
            return true;
        }
    }

    // False (nothing changed) when `id` is not handed out now: never handed out, reserved, or freed already.
    bool free(uint32_t id)
    {
        if (id >= kSyncpts)
            return false;
        const uint64_t was = __atomic_fetch_and(&owned[id / 64], ~bit(id), __ATOMIC_ACQ_REL);
        if ((was & bit(id)) == 0)
            return false;
        __atomic_store_n(&freedAt[id], __atomic_add_fetch(&freeCount, 1, __ATOMIC_RELAXED), __ATOMIC_RELAXED);
        __atomic_fetch_sub(&inUse, 1, __ATOMIC_RELAXED);
        __atomic_fetch_and(&taken[id / 64], ~bit(id), __ATOMIC_RELEASE);
        return true;
    }

    bool owns(uint32_t id) const
    {
        return id < kSyncpts && (__atomic_load_n(&owned[id / 64], __ATOMIC_ACQUIRE) & bit(id)) != 0;
    }

    bool everOwned(uint32_t id) const
    {
        return id < kSyncpts && (__atomic_load_n(&ever[id / 64], __ATOMIC_ACQUIRE) & bit(id)) != 0;
    }

    uint32_t capacity() const
    {
        return kSyncpts - __atomic_load_n(&reserved, __ATOMIC_RELAXED);
    }
};
