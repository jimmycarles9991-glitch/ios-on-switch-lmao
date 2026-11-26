// SPDX-License-Identifier: BSD-2-Clause-Patent
// One Tegra210 MC SMMU address space (Linux iommu/tegra-smmu.c layout): a 4 KiB page directory of 1024 entries, each
// covering 4 MiB, and 4 KiB page tables of 1024 entries. A directory entry is its table's PFN | READ 31 | WRITE 30 |
// NONSECURE 29 | NEXT 28, or without NEXT a 4 MiB section: the section's PFN | READ | (WRITE) | NONSECURE (L4T's
// tegra-smmu.c, the Switch's Linux, maps, splits and walks them; mainline maps 4 KiB only); a table entry is the page's
// PFN | READ | (WRITE) | NONSECURE. Addresses are 32 bits.
//
// Plain state over the caller's physically contiguous, below-4-GiB table buffer: page 0 is the directory, the others are
// handed out as tables in order (so the tables reserve() installs for a range are consecutive pages). No IOKit: TegraVIC,
// the display's domain and test/smmu_domain_test.cpp share it. Every change records the bytes it wrote, the IOVAs whose
// page entries it wrote and whether it wrote the directory (takeChange): the caller cleans the bytes to memory, which
// the SMMU reads without snooping, then flushes the SMMU's caches of them (TegraSMMUPublish.h). The registers that
// select a directory are global, so attaching is TegraSMMU.h's job.
#pragma once
#include <stdint.h>

struct TegraSMMUDomain {
    static constexpr uint32_t kRead = 1u << 31, kWrite = 1u << 30, kNonSecure = 1u << 29, kNext = 1u << 28;
    static constexpr uint32_t kPfnMask = 0x3fffff, kEntries = 1024;
    static constexpr uint64_t kPage = 4096, kSpan = 4ULL << 20, kLimit = 1ULL << 32;
    uint32_t *va = nullptr;
    uint64_t pa = 0;
    uint32_t pages = 0, used = 0;
    uintptr_t dirtyLow = UINTPTR_MAX, dirtyHigh = 0;
    uint64_t iovaLow = UINT64_MAX, iovaHigh = 0;
    bool directoryDirty = false;

    // The table bytes [low, high) written since the last take, from physical address pa on; the IOVAs [iovaLow,
    // iovaHigh) whose page entries they hold; directory: a directory entry among them, so any IOVA may have changed.
    struct Change {
        uintptr_t low, high;
        uint64_t pa, iovaLow, iovaHigh;
        bool directory;
    };

    // tables: count 4 KiB pages at physical address tablesPa (page 0 becomes the empty directory). False when the buffer
    // cannot be one (unaligned, above 4 GiB, no room for a table).
    bool init(void *tables, uint64_t tablesPa, uint32_t count)
    {
        if (!tables || count < 2 || (tablesPa & (kPage - 1)) ||
            tablesPa + static_cast<uint64_t>(count) * kPage > kLimit)
            return false;
        va = static_cast<uint32_t *>(tables);
        pa = tablesPa;
        pages = count;
        used = 1;
        clear(va, kEntries);
        return true;
    }

    uint32_t *table(uint32_t index) const
    {
        return va + static_cast<uint64_t>(index) * kEntries;
    }

    uint64_t tablePa(uint32_t index) const
    {
        return pa + static_cast<uint64_t>(index) * kPage;
    }

    // PTB_DATA for this directory.
    uint32_t ptb() const
    {
        return static_cast<uint32_t>(pa >> 12) | kRead | kWrite | kNonSecure;
    }

    static uint32_t entry(uint64_t page, bool writable)
    {
        return static_cast<uint32_t>(page >> 12) | kRead | kNonSecure | (writable ? kWrite : 0);
    }

    // The table covering iova; installed (the next free page) when create. nullptr: out of range, out of tables, a
    // section, or a directory entry that is not one of this buffer's tables.
    uint32_t *tableFor(uint64_t iova, bool create)
    {
        if (!va || iova >= kLimit)
            return nullptr;
        uint32_t &pde = va[iova >> 22];
        if (pde != 0 && !(pde & kNext))
            return nullptr;
        if (!(pde & kNext)) {
            if (!create || used == pages)
                return nullptr;
            const uint32_t index = used++;
            clear(table(index), kEntries);
            pde = static_cast<uint32_t>(tablePa(index) >> 12) | kRead | kWrite | kNonSecure | kNext;
            touch(&pde, sizeof(pde));
        }
        const uint64_t at = static_cast<uint64_t>(pde & kPfnMask) << 12;
        if (at < pa + kPage || at >= pa + static_cast<uint64_t>(pages) * kPage)
            return nullptr;
        return table(static_cast<uint32_t>((at - pa) / kPage));
    }

    uint32_t *pte(uint64_t iova, bool create)
    {
        uint32_t *t = tableFor(iova, create);
        return t ? &t[(iova >> 12) & (kEntries - 1)] : nullptr;
    }

    // The tables [iova, iova + bytes) touches: what reserve() or identity() installs for it in an empty domain.
    static constexpr uint32_t tablesFor(uint64_t iova, uint64_t bytes)
    {
        return bytes ? static_cast<uint32_t>((iova + bytes - 1) / kSpan - iova / kSpan + 1) : 0;
    }

    // Tables for [iova, iova + bytes), installed now; a range not yet covered gets consecutive pages. False when out of tables.
    bool reserve(uint64_t iova, uint64_t bytes)
    {
        for (uint64_t at = iova & ~(kSpan - 1); at < iova + bytes; at += kSpan) {
            if (!tableFor(at, true))
                return false;
        }
        return true;
    }

    // The 4 MiB at iova (aligned, its directory entry empty) mapped to the 4 MiB at target (aligned) by one directory
    // entry.
    bool section(uint64_t iova, uint64_t target, bool writable)
    {
        if (!va || iova >= kLimit || ((iova | target) & (kSpan - 1)) != 0 || va[iova >> 22] != 0)
            return false;
        va[iova >> 22] = entry(target, writable);
        touch(&va[iova >> 22], sizeof(uint32_t));
        return true;
    }

    // [at, at + bytes) mapped to itself (bytes rounded up to pages).
    bool identity(uint64_t at, uint64_t bytes, bool writable)
    {
        for (uint64_t page = at & ~(kPage - 1); page < at + bytes; page += kPage) {
            uint32_t *p = pte(page, true);
            if (!p)
                return false;
            setPte(p, page, entry(page, writable));
        }
        return true;
    }

    // count pages (physical addresses) from iova on; their tables must exist or be installable.
    bool map(uint64_t iova, const uint64_t *pagesPa, uint32_t count, bool writable)
    {
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t at = iova + static_cast<uint64_t>(i) * kPage;
            uint32_t *p = pte(at, true);
            if (!p)
                return false;
            setPte(p, at, entry(pagesPa[i], writable));
        }
        return true;
    }

    void unmap(uint64_t iova, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i) {
            const uint64_t at = iova + static_cast<uint64_t>(i) * kPage;
            if (uint32_t *p = pte(at, false); p && *p)
                setPte(p, at, 0);
        }
    }

    // The physical address iova translates to; UINT64_MAX when unmapped (the SMMU would fault).
    uint64_t translate(uint64_t iova) const
    {
        if (!va || iova >= kLimit)
            return UINT64_MAX;
        const uint32_t pde = va[iova >> 22];
        if (!(pde & kNext))
            return (pde & kRead) ? (static_cast<uint64_t>(pde & kPfnMask) << 12) + (iova & (kSpan - 1)) : UINT64_MAX;
        const uint64_t at = static_cast<uint64_t>(pde & kPfnMask) << 12;
        if (at < pa + kPage || at >= pa + static_cast<uint64_t>(pages) * kPage)
            return UINT64_MAX;
        const uint32_t e = table(static_cast<uint32_t>((at - pa) / kPage))[(iova >> 12) & (kEntries - 1)];
        return (e & kRead) ? (static_cast<uint64_t>(e & kPfnMask) << 12) | (iova & (kPage - 1)) : UINT64_MAX;
    }

    // What changed since the last take (takeDirty or takeChange); false when nothing did.
    bool takeChange(Change *out)
    {
        if (dirtyLow >= dirtyHigh)
            return false;
        *out = {dirtyLow, dirtyHigh, pa + (dirtyLow - reinterpret_cast<uintptr_t>(va)),
                iovaLow,  iovaHigh,  directoryDirty};
        dirtyLow = UINTPTR_MAX;
        dirtyHigh = 0;
        iovaLow = UINT64_MAX;
        iovaHigh = 0;
        directoryDirty = false;
        return true;
    }

    // The bytes written since the last take, for a caller that cleans them and then attaches (which flushes all).
    bool takeDirty(uintptr_t *low, uintptr_t *high)
    {
        Change change{};
        if (!takeChange(&change))
            return false;
        *low = change.low;
        *high = change.high;
        return true;
    }

    void setPte(uint32_t *p, uint64_t iova, uint32_t value)
    {
        *p = value;
        touch(p, sizeof(*p));
        const uint64_t page = iova & ~(kPage - 1);
        if (page < iovaLow)
            iovaLow = page;
        if (page + kPage > iovaHigh)
            iovaHigh = page + kPage;
    }

    void touch(const void *p, uintptr_t bytes)
    {
        const uintptr_t at = reinterpret_cast<uintptr_t>(p), directory = reinterpret_cast<uintptr_t>(va);
        if (at < directory + kPage && at + bytes > directory)
            directoryDirty = true;
        if (at < dirtyLow)
            dirtyLow = at;
        if (at + bytes > dirtyHigh)
            dirtyHigh = at + bytes;
    }

    void clear(uint32_t *p, uint32_t count)
    {
        for (uint32_t i = 0; i < count; ++i)
            p[i] = 0;
        touch(p, static_cast<uintptr_t>(count) * sizeof(*p));
    }
};
