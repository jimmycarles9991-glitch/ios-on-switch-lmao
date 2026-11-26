// SPDX-License-Identifier: BSD-2-Clause-Patent
// RAM at fixed addresses in one XUSB swgroup's 32-bit space through the SMMU, mapped once (UsbSmmuSpace::init), and
// the window beside it for what must be mapped per request. A buffer at a fixed address is translated by arithmetic,
// with no page-table write and no flush. A 4 MiB region a bank covers whole is one directory entry (a section,
// TegraSMMUDomain.h); a region a bank covers in part, or that holds read-only pages, gets a table. Sections save table
// memory and a walk per TLB miss, not TLB reach: the TLB holds them as 4 KiB pages (T210 TRM 18.6.3.1, p.813).
//
// The device controller (device()) has all of RAM: the low bank at its own address and the high bank 4 GiB lower, into
// the 2 GiB below RAM where nothing else is, so the high bank's last IOVA and the low bank's first are adjacent and
// runs that end the one and start the other are one IOVA run. Its window is what is left above the low bank (3 MiB on
// the Switch, whose low bank ends at 0xffd00000). The host (host()) keeps the space below RAM for a large window, since
// its storage I/O is scattered pages, and has only the low bank at a fixed address, with the falcon's firmware image
// read only. No IOKit: test/xudc_iova_test.cpp drives the same code.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../TegraSMMU/TegraSMMUDomain.h"

// RAM's two banks.
struct UsbRam {
    uint64_t lowStart, lowEnd, highStart, highEnd;

    // The banks of the bootloader's committed ledger (/chosen/switch-ram-ledger, boot/loader/ram.h struct
    // switch_ram_ledger, little-endian: magic, version, count and committed as u32, then each bank's start and end as
    // u64). The VM's RAM is these banks (kernel/switch/ram/ram.c), so they hold every page a request can be in.
    static bool fromLedger(const void *bytes, size_t length, UsbRam *out)
    {
        constexpr uint32_t kMagic = 0x324d4152;
        constexpr uint32_t kVersion = 1;
        constexpr size_t kBanksAt = 16;
        uint32_t head[4];
        uint64_t banks[4];
        if (!bytes || length < kBanksAt + sizeof(banks))
            return false;
        memcpy(head, bytes, sizeof(head));
        memcpy(banks, static_cast<const uint8_t *>(bytes) + kBanksAt, sizeof(banks));
        if (head[0] != kMagic || head[1] != kVersion || head[3] != 1)
            return false;
        *out = {banks[0], banks[1], banks[2], banks[3]};
        return true;
    }
};

struct UsbStaticMap {
    static constexpr uint64_t kLimit = TegraSMMUDomain::kLimit, kSpan = TegraSMMUDomain::kSpan, kPage = 4096;
    // A window starts at a 64 KiB line, as a TRB's buffer may not cross one: a request mapped at the window's first
    // page starts on a line (XudcIova.h).
    static constexpr uint64_t kWindowAlign = 64ULL * 1024;
    // The host's window: 64 MiB of transfers in flight from 16 MiB up. xusb_host is an AHBSLV client, which cannot
    // reach the MC below 16 MiB (IROM_LOVEC; T210 TRM tables 4 and 5, p.30-31). xusb_dev is a 34-bit client, which
    // reaches every address up to 0x2_7FFF_FFFF (p.31), so the device's high bank from IOVA 0 is reachable.
    static constexpr uint64_t kHostWindowBase = 16ULL << 20, kHostWindowBytes = 64ULL << 20;

    UsbRam ram;
    bool high;  // the high bank at a fixed IOVA too, else reached only through the window
    uint64_t windowBase, windowBytes;
    uint64_t readOnlyStart, readOnlyEnd;  // fixed pages the controller may only read

    static constexpr UsbStaticMap device(const UsbRam &ram)
    {
        const uint64_t base = (ram.lowEnd + kWindowAlign - 1) & ~(kWindowAlign - 1);
        return {ram, true, base, base < kLimit ? kLimit - base : 0, 0, 0};
    }

    static constexpr UsbStaticMap host(const UsbRam &ram, uint64_t readOnlyPa, uint64_t readOnlyBytes)
    {
        return {ram,
                false,
                kHostWindowBase,
                kHostWindowBytes,
                readOnlyPa & ~(kPage - 1),
                (readOnlyPa + readOnlyBytes + kPage - 1) & ~(kPage - 1)};
    }

    // The low bank below 4 GiB; the high bank, when mapped, from 4 GiB up and no larger than the room below the low
    // one; the window aligned, inside the space and clear of every fixed address; the read-only pages in the low bank.
    constexpr bool fits() const
    {
        const uint64_t highTop = high ? ram.highEnd - kLimit : 0;
        const uint64_t windowEnd = windowBase + windowBytes;
        const bool highFits = ram.highStart == kLimit && ram.highStart <= ram.highEnd && highTop <= ram.lowStart;
        const bool banks = ram.lowStart < ram.lowEnd && ram.lowEnd <= kLimit && (!high || highFits);
        const bool window = (windowBase & (kWindowAlign - 1)) == 0 && windowEnd <= kLimit && windowBase >= highTop &&
                            (windowEnd <= ram.lowStart || windowBase >= ram.lowEnd);
        const bool pages = readOnlyStart == readOnlyEnd || (readOnlyStart >= ram.lowStart && readOnlyEnd <= ram.lowEnd);
        return banks && window && pages;
    }

    // The fixed IOVA of a RAM address; UINT64_MAX for anything else.
    constexpr uint64_t iova(uint64_t pa) const
    {
        if (pa >= ram.lowStart && pa < ram.lowEnd)
            return pa;
        if (high && pa >= ram.highStart && pa < ram.highEnd)
            return pa - kLimit;
        return UINT64_MAX;
    }

    // The fixed IOVA of [pa, pa + length) when it lies in one mapped bank; UINT64_MAX otherwise.
    constexpr uint64_t iova(uint64_t pa, uint64_t length) const
    {
        const uint64_t first = iova(pa);
        return length && first != UINT64_MAX && iova(pa + length - 1) == first + length - 1 ? first : UINT64_MAX;
    }

    // The RAM address at a fixed IOVA; UINT64_MAX for anything else (the window, the gaps).
    constexpr uint64_t pa(uint64_t iova) const
    {
        if (iova >= ram.lowStart && iova < ram.lowEnd)
            return iova;
        if (high && iova + kLimit >= ram.highStart && iova + kLimit < ram.highEnd)
            return iova + kLimit;
        return UINT64_MAX;
    }

    constexpr bool readOnly(uint64_t pa) const
    {
        return pa >= readOnlyStart && pa < readOnlyEnd;
    }

    // fn(iova, pa, bytes, section) over each mapped bank in 4 MiB regions: a section where the bank covers the region
    // whole and no read-only page is in it, else the bank's part of it. False as soon as fn is.
    template <class Fn> bool regions(Fn fn) const
    {
        const uint64_t banks[2][2] = {{ram.lowStart, ram.lowEnd}, {ram.highStart, high ? ram.highEnd : ram.highStart}};
        for (const auto &bank : banks) {
            for (uint64_t at = bank[0]; at < bank[1];) {
                const uint64_t region = at & ~(kSpan - 1);
                const uint64_t end = region + kSpan < bank[1] ? region + kSpan : bank[1];
                const bool whole = at == region && end == region + kSpan;
                const bool clear = readOnlyEnd <= region || readOnlyStart >= region + kSpan;
                if (!fn(iova(at), at, end - at, whole && clear))
                    return false;
                at = end;
            }
        }
        return true;
    }

    // The map and the window's tables into an empty domain of 1 + tables() pages; false when they do not fit.
    bool install(TegraSMMUDomain &domain) const
    {
        const bool mapped = regions([this, &domain](uint64_t iova, uint64_t pa, uint64_t bytes, bool section) {
            if (section)
                return domain.section(iova, pa, true);
            for (uint64_t done = 0; done < bytes; done += kPage) {
                const uint64_t page = pa + done;
                if (!domain.map(iova + done, &page, 1, !readOnly(page)))
                    return false;
            }
            return true;
        });
        return mapped && domain.reserve(windowBase, windowBytes);
    }

    // The tables the map needs: one for each 4 MiB region a bank covers in part, that holds read-only pages, or that
    // the window reaches.
    uint32_t tables() const
    {
        uint32_t marked[TegraSMMUDomain::kEntries / 32] = {};
        const auto mark = [&marked](uint64_t iova) {
            if (iova < kLimit)
                marked[iova / kSpan / 32] |= 1u << (iova / kSpan % 32);
        };
        regions([&mark](uint64_t iova, uint64_t, uint64_t, bool section) {
            if (!section)
                mark(iova);
            return true;
        });
        for (uint64_t at = windowBase; at < windowBase + windowBytes; at += kSpan - (at & (kSpan - 1)))
            mark(at);
        uint32_t count = 0;
        for (const uint32_t word : marked)
            count += static_cast<uint32_t>(__builtin_popcount(word));
        return count;
    }
};
