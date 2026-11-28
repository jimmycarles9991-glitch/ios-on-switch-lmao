// SPDX-License-Identifier: BSD-2-Clause-Patent
// A DMA master's view of RAM through the SMMU at RAM's own addresses (IOVA = PA): a page translates while one of its
// users holds it (a count per page, so overlapping holders need no coordination) and faults at the MC otherwise
// (TegraMCErrors.cpp panics naming the client and address). For masters whose addresses come from arbitrary kernel
// pages (the SD card's ADMA2, GM20B's GMMU): the address the device is given stays the page's own, and the SMMU decides
// whether the device may use it right now.
//
// The Switch's RAM is 0x8000_0000-0x1_7fff_ffff. An SMMU directory covers 32 bits, so the swgroup register's fields
// split it by va[33:32] (TegraSMMU.h tegra_smmu_attach_split): field 0 through `low`'s directory (PA below 4 GiB),
// field 1 through `high`'s (PA - 4 GiB). Directories use TegraSMMUDomain.h's layout; page tables are made the first time
// a page of their 4 MiB is held and kept.
//
// Holds and releases only write memory; commit() publishes them: the table bytes cleaned to memory (the SMMU does not
// snoop the CPU's caches), then the SMMU's stale copies flushed, every table of the commit under one MC lock and one
// read-back (one directory change flushes that ASID whole). The device may use a held page only after the commit, and
// a released page may go back to the VM only after it.
//
// What is flushed: the SMMU caches page entries a group of 16 at a time, the PTC by the group's 64-byte memory atom and
// the TLB by a line of 16 entries (T210 TRM 18.6.3.3, 18.11.1.10), and fills either only on a client's access. A client
// accesses only pages that are held (any other access faults and panics), so only a group that had a held entry at the
// last commit can be in either. A change in such a group (a release; a hold next to a held page) flushes the group's
// atom from the PTC and its table's 4 MiB section from the TLB. A hold in any other group needs no flush: the commit
// cleans its entry and the first access fetches it. A boot's page-in storm holds scattered pages in empty groups, so
// its holds publish with a cache clean alone and only its releases reach the MC.
//
// Plain state: the caller locks and supplies Env (allocations, cache cleaning, flushes); the host tests drive it with a
// model.
#pragma once
#include <stdint.h>
#include "TegraSMMU.h"
#include "TegraSMMUDomain.h"

struct TegraSMMUIdentityTable {
    uint32_t *entries;  // 1024 page entries, a 4 KiB page below 4 GiB
    uint64_t pa;
    uint64_t base;                 // the first RAM address it covers
    uint16_t *users;               // holders per entry
    uint32_t dirtyLow, dirtyHigh;  // entries written since the last commit
    TegraSMMUIdentityTable *nextDirty;
    uint64_t held;       // bit g: group g (entries 16g to 16g + 15) has a held entry
    uint64_t published;  // held at the last commit: the groups the PTC or TLB may hold
    uint64_t touched;    // groups written since the last commit
    uint8_t valid[64];   // held entries per group
};

template <class Env> struct TegraSMMUIdentityCore {
    static constexpr uint64_t kRamBase = 0x80000000ULL, kRamEnd = 0x180000000ULL, kPage = 4096;
    static constexpr uint64_t kSpan = TegraSMMUDomain::kSpan;
    static constexpr uint32_t kTables = static_cast<uint32_t>((kRamEnd - kRamBase) / kSpan);
    static constexpr uint32_t kEntries = TegraSMMUDomain::kEntries, kMaxUsers = UINT16_MAX;
    static constexpr uint32_t kGroup = 16, kTablesAtOnce = 8;

    struct Half {
        uint32_t asid;
        uint32_t *directory;
        uint64_t directoryPa;
        bool directoryDirty;
    };

    Env *env = nullptr;
    Half halves[2] = {};
    TegraSMMUIdentityTable *tables[kTables] = {};
    TegraSMMUIdentityTable *dirty = nullptr;

    // Directories: two zeroed 4 KiB pages below 4 GiB, low's for PA below 4 GiB, high's for the rest.
    bool init(Env *e, uint32_t asidLow, uint32_t *low, uint64_t lowPa, uint32_t asidHigh, uint32_t *high,
              uint64_t highPa)
    {
        if (!e || !low || !high || ((lowPa | highPa) & (kPage - 1)) || lowPa >= TegraSMMUDomain::kLimit ||
            highPa >= TegraSMMUDomain::kLimit)
            return false;
        env = e;
        halves[0] = {asidLow, low, lowPa, false};
        halves[1] = {asidHigh, high, highPa, false};
        return true;
    }

    static uint32_t ptbOf(uint64_t pa)
    {
        return static_cast<uint32_t>(pa >> 12) | TegraSMMUDomain::kRead | TegraSMMUDomain::kWrite |
               TegraSMMUDomain::kNonSecure;
    }

    uint32_t ptb(uint32_t half) const
    {
        return ptbOf(halves[half].directoryPa);
    }

    static bool inRam(uint64_t pa, uint64_t bytes)
    {
        return bytes != 0 && pa >= kRamBase && pa < kRamEnd && bytes <= kRamEnd - pa;
    }

    // Every table of RAM below `limit`, made now, so no hold there allocates: for a holder whose path must not wait for
    // memory (a storage driver's I/O, the kernel's crypto hooks).
    bool makeTables(uint64_t limit = kRamEnd)
    {
        const uint64_t end = limit < kRamEnd ? limit : kRamEnd;
        for (uint32_t i = 0; kRamBase + static_cast<uint64_t>(i) * kSpan < end; ++i) {
            if (!table(kRamBase + static_cast<uint64_t>(i) * kSpan, true))
                return false;
        }
        return true;
    }

    // Users of [pa, pa + bytes) (rounded out to pages) each hold it once more. All or nothing: false when a page is
    // outside RAM, a table cannot be had, or a page already has kMaxUsers holders.
    bool hold(uint64_t pa, uint64_t bytes)
    {
        if (!inRam(pa, bytes))
            return false;
        const uint64_t first = pa & ~(kPage - 1);
        const uint64_t end = (pa + bytes + kPage - 1) & ~(kPage - 1);
        for (uint64_t at = first; at < end;) {
            TegraSMMUIdentityTable *t = table(at, true);
            if (!t)
                return undo(first, at, false);
            const uint64_t stop = segmentEnd(t, end);
            for (uint32_t e = entryOf(at); at < stop; ++e, at += kPage) {
                if (t->users[e] == kMaxUsers)
                    return undo(first, at, false);
                if (t->users[e]++ == 0)
                    write(t, e,
                          static_cast<uint32_t>(at >> 12) | TegraSMMUDomain::kRead | TegraSMMUDomain::kWrite |
                              TegraSMMUDomain::kNonSecure);
            }
        }
        return true;
    }

    // One hold of each page of [pa, pa + bytes) ends; the last one unmaps the page. false (nothing changed) when one of
    // them has no holder: the caller's accounting is wrong.
    bool release(uint64_t pa, uint64_t bytes)
    {
        if (!inRam(pa, bytes))
            return false;
        const uint64_t first = pa & ~(kPage - 1);
        const uint64_t end = (pa + bytes + kPage - 1) & ~(kPage - 1);
        for (uint64_t at = first; at < end;) {
            TegraSMMUIdentityTable *t = table(at, false);
            if (!t)
                return undo(first, at, true);
            const uint64_t stop = segmentEnd(t, end);
            for (uint32_t e = entryOf(at); at < stop; ++e, at += kPage) {
                if (t->users[e] == 0)
                    return undo(first, at, true);
                if (--t->users[e] == 0)
                    write(t, e, 0);
            }
        }
        return true;
    }

    uint32_t holders(uint64_t pa) const
    {
        if (!inRam(pa, 1))
            return 0;
        const TegraSMMUIdentityTable *t = tables[(pa - kRamBase) / kSpan];
        return t ? t->users[entryOf(pa)] : 0;
    }

    // What a device address translates to now; UINT64_MAX when the SMMU would fault.
    uint64_t translate(uint64_t pa) const
    {
        if (!inRam(pa, 1))
            return UINT64_MAX;
        const Half &h = halves[pa >> 32];
        const uint32_t pde = h.directory[(pa >> 22) & (kEntries - 1)];
        if (!(pde & TegraSMMUDomain::kNext))
            return UINT64_MAX;
        const TegraSMMUIdentityTable *t = tables[(pa - kRamBase) / kSpan];
        if (!t || (static_cast<uint64_t>(pde & TegraSMMUDomain::kPfnMask) << 12) != t->pa)
            return UINT64_MAX;
        const uint32_t pte = t->entries[entryOf(pa)];
        return (pte & TegraSMMUDomain::kRead)
                   ? (static_cast<uint64_t>(pte & TegraSMMUDomain::kPfnMask) << 12) | (pa & (kPage - 1))
                   : UINT64_MAX;
    }

    // Publishes every hold and release since the last commit (see the top of the file).
    void commit()
    {
        for (uint32_t i = 0; i < 2; ++i) {
            if (halves[i].directoryDirty)
                env->clean(halves[i].directory, kPage);
        }
        for (TegraSMMUIdentityTable *t = dirty; t; t = t->nextDirty)
            env->clean(t->entries + t->dirtyLow, (t->dirtyHigh - t->dirtyLow) * sizeof(uint32_t));
        env->sync();
        for (uint32_t i = 0; i < 2; ++i) {
            if (halves[i].directoryDirty)
                env->flushAsid(halves[i].asid);
        }
        switch_smmu_table_flush stale[2][kTablesAtOnce];
        uint32_t count[2] = {0, 0};
        while (TegraSMMUIdentityTable *t = dirty) {
            dirty = t->nextDirty;
            const uint32_t i = static_cast<uint32_t>(t->base >> 32);
            const uint64_t groups = t->touched & t->published;
            if (groups && !halves[i].directoryDirty) {
                stale[i][count[i]++] = {t->pa, groups, static_cast<uint32_t>(t->base)};
                if (count[i] == kTablesAtOnce) {
                    env->flush(halves[i].asid, stale[i], count[i]);
                    count[i] = 0;
                }
            }
            t->published = t->held;
            t->touched = 0;
            t->nextDirty = nullptr;
            t->dirtyLow = kEntries;
            t->dirtyHigh = 0;
        }
        for (uint32_t i = 0; i < 2; ++i) {
            if (count[i])
                env->flush(halves[i].asid, stale[i], count[i]);
        }
        halves[0].directoryDirty = halves[1].directoryDirty = false;
    }

private:
    static uint32_t entryOf(uint64_t pa)
    {
        return static_cast<uint32_t>(pa >> 12) & (kEntries - 1);
    }

    // The pages [first, upto) of a hold (or release) that stopped there, given back (taken again), and false.
    bool undo(uint64_t first, uint64_t upto, bool wasRelease)
    {
        if (upto > first)
            (void)(wasRelease ? hold(first, upto - first) : release(first, upto - first));
        return false;
    }

    static uint64_t segmentEnd(const TegraSMMUIdentityTable *t, uint64_t end)
    {
        return end < t->base + kSpan ? end : t->base + kSpan;
    }

    // The table covering pa; made and installed when create (env's allocation), else null when there is none.
    TegraSMMUIdentityTable *table(uint64_t pa, bool create)
    {
        TegraSMMUIdentityTable *&slot = tables[(pa - kRamBase) / kSpan];
        if (slot || !create)
            return slot;
        TegraSMMUIdentityTable *made = env->makeTable();
        if (!made)
            return nullptr;
        made->base = pa & ~(kSpan - 1);
        made->dirtyLow = kEntries;
        made->dirtyHigh = 0;
        made->nextDirty = nullptr;
        made->held = made->published = made->touched = 0;
        for (uint8_t &v : made->valid)
            v = 0;
        Half &h = halves[pa >> 32];
        h.directory[(pa >> 22) & (kEntries - 1)] = static_cast<uint32_t>(made->pa >> 12) | TegraSMMUDomain::kRead |
                                                   TegraSMMUDomain::kWrite | TegraSMMUDomain::kNonSecure |
                                                   TegraSMMUDomain::kNext;
        h.directoryDirty = true;
        slot = made;
        return made;
    }

    void write(TegraSMMUIdentityTable *t, uint32_t e, uint32_t value)
    {
        t->entries[e] = value;
        const uint32_t g = e / kGroup;
        const uint64_t group = 1ULL << g;
        if (value) {
            ++t->valid[g];
            t->held |= group;
        } else if (--t->valid[g] == 0) {
            t->held &= ~group;
        }
        t->touched |= group;
        if (t->dirtyLow >= t->dirtyHigh) {
            t->nextDirty = dirty;
            dirty = t;
            t->dirtyLow = e;
            t->dirtyHigh = e + 1;
            return;
        }
        if (e < t->dirtyLow)
            t->dirtyLow = e;
        if (e + 1 > t->dirtyHigh)
            t->dirtyHigh = e + 1;
    }
};
