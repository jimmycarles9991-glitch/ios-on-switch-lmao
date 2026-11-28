// SPDX-License-Identifier: BSD-2-Clause-Patent
// ADMA2 descriptor tables for the SD driver: scatter-gather DMA straight between the card and a request's own pages.
//
// Format: SD Host Controller spec v4 ADMA2 with 64-bit addressing, the mode setup() leaves SDMMC1 in (HOST_CONTROL2 Host
// Version 4 Enable + 64-bit Addressing) and the one NVIDIA's L4T kernel uses on this controller (switchroot
// linux-4.9 sdhci-tegra.c soc_data_tegra210: SDHCI_QUIRK2_USE_64BIT_ADDR, so sdhci.c takes the 16-byte descriptor
// SDHCI_ADMA2_64_ADDR_DESC_SZ). Tegra210 quirks from the same table, both kept here:
// - SDHCI_QUIRK_BROKEN_ADMA_ZEROLEN_DESC: a length of 0 does not mean 64 KiB, so no descriptor is longer than
//   kAdmaMaxLength (below 64 KiB, a whole number of 4 KiB pages so splits stay page- and line-aligned).
// - SDHCI_QUIRK_NO_ENDATTR_IN_NOPDESC: End goes on the last transfer descriptor, never on a separate NOP.
// Mainline Linux gives Tegra210's SDMMC a 34-bit DMA mask (sdhci-tegra.c soc_data_tegra210.dma_mask) with no SMMU in
// front of it, which covers this board's DRAM (0x80000000-0x17fffffff); the driver's tables reach up to it (`limit`).
// `high` counts a table's bytes above 4 GiB.
//
// What may go straight to the card (anything else takes the bounce buffer; the builder says why):
// - every segment starts and ends on a 64-byte cache line: the driver cleans or invalidates whole lines of the client's
//   pages, which is only correct when no other data shares one (a read's invalidate would drop a neighbour's writes);
// - every page is managed RAM with the default (cacheable) attributes, as `pageOk` reports: the lines are reached through
//   the kernel's physical aperture, and a write-combined or device page is staged by copypv with its own attributes;
// - everything below `limit`, and the table has room.
#pragma once

#include <stdint.h>

// One descriptor: 128 bits, little-endian (the driver and the controller are both little-endian).
struct SwitchAdmaDescriptor {
    uint16_t attributes;
    uint16_t length;
    uint32_t addressLow;
    uint32_t addressHigh;
    uint32_t reserved;  // always 0
};

static_assert(sizeof(SwitchAdmaDescriptor) == 16, "ADMA2 64-bit addressing descriptors are 128 bits in v4 mode");

// Attribute bits: Valid, End, and Act = transfer (sdhci.h ADMA2_TRAN_VALID 0x21, | End = 0x23). No Int: the driver waits
// for transfer complete, never for a descriptor.
constexpr uint16_t kAdmaValid = 0x01, kAdmaEnd = 0x02, kAdmaTransfer = 0x20;
constexpr uint32_t kAdmaMaxLength = 0xf000;
constexpr uint64_t kAdmaLine = 64, kAdmaPage = 4096, kAdma4GiB = 1ULL << 32;

// Why a request went through the bounce buffer instead (stats index; kAdmaOk is "ADMA2 is off").
enum SwitchAdmaRefusal : uint8_t {
    kAdmaOk = 0,
    kAdmaAlign,    // a segment does not start and end on a cache line
    kAdmaMemory,   // a page is not managed, cacheable RAM
    kAdmaHigh,     // a segment reaches the address limit
    kAdmaFull,     // more descriptors than the table holds
    kAdmaMissing,  // a segment the memory descriptor does not have (or an empty one)
    kAdmaRefusals
};

struct SwitchAdmaTable {
    SwitchAdmaDescriptor *entries;
    uint32_t capacity;
    uint64_t limit;  // the first physical address the engine may not reach
    uint32_t count;
    uint64_t bytes;
    uint64_t high;    // of `bytes`, those at or above 4 GiB (the engine's high address word in use)
    uint32_t sealed;  // r14-sdcq: while count == sealed the last descriptor takes no more bytes (addSealed)
    uint64_t minPa = ~0ULL;
    uint64_t maxPa = 0;
    uint64_t unsealedMinPa = ~0ULL;
    uint64_t unsealedMaxPa = 0;
    uint64_t sealedMinPa = ~0ULL;
    uint64_t sealedMaxPa = 0;
    bool boundsValid = false;

    void reset()
    {
        count = 0;
        bytes = 0;
        high = 0;
        sealed = 0;
        minPa = ~0ULL;
        maxPa = 0;
        unsealedMinPa = ~0ULL;
        unsealedMaxPa = 0;
        sealedMinPa = ~0ULL;
        sealedMaxPa = 0;
        boundsValid = true;
    }

    void noteRange(uint64_t pa, uint64_t length, bool isSealed)
    {
        if (!length)
            return;
        const uint64_t end = pa + length;
        if (pa < minPa)
            minPa = pa;
        if (end > maxPa)
            maxPa = end;
        if (isSealed) {
            if (pa < sealedMinPa)
                sealedMinPa = pa;
            if (end > sealedMaxPa)
                sealedMaxPa = end;
        } else {
            if (pa < unsealedMinPa)
                unsealedMinPa = pa;
            if (end > unsealedMaxPa)
                unsealedMaxPa = end;
        }
    }

    // Why [pa, pa + length) may not go straight to the card, or kAdmaOk; changes nothing.
    template <typename PageOk> SwitchAdmaRefusal check(uint64_t pa, uint64_t length, PageOk pageOk) const
    {
        if (!length)
            return kAdmaMissing;
        if ((pa | length) & (kAdmaLine - 1))
            return kAdmaAlign;
        if (pa >= limit || length > limit - pa)
            return kAdmaHigh;
        for (uint64_t page = pa & ~(kAdmaPage - 1); page < pa + length; page += kAdmaPage) {
            if (!pageOk(page))
                return kAdmaMemory;
        }
        return kAdmaOk;
    }

    // Append [pa, pa + length) assuming check() already passed.
    SwitchAdmaRefusal addCheckedInternal(uint64_t pa, uint64_t length, bool isSealed)
    {
        if (!count) {
            minPa = unsealedMinPa = sealedMinPa = ~0ULL;
            maxPa = unsealedMaxPa = sealedMaxPa = 0;
            boundsValid = true;
        }
        if (pa + length > kAdma4GiB)
            high += pa >= kAdma4GiB ? length : pa + length - kAdma4GiB;
        while (length) {
            // No descriptor crosses a 4 GiB boundary (Linux's default DMA segment boundary is 0xffffffff too): what fits up
            // to the next one.
            const uint64_t boundary = (pa | (kAdma4GiB - 1)) + 1;
            SwitchAdmaDescriptor *last = count ? &entries[count - 1] : nullptr;
            uint64_t room = 0;
            if (last && count != sealed &&
                (static_cast<uint64_t>(last->addressHigh) << 32 | last->addressLow) + last->length == pa &&
                pa != (pa & ~(kAdma4GiB - 1)))
                room = kAdmaMaxLength - last->length;
            if (!room) {
                if (count == capacity)
                    return kAdmaFull;
                last = &entries[count++];
                // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): entries is allocated before the first add()
                *last = {kAdmaValid | kAdmaTransfer, 0, static_cast<uint32_t>(pa), static_cast<uint32_t>(pa >> 32), 0};
                room = kAdmaMaxLength;
            }
            if (room > boundary - pa)
                room = boundary - pa;
            const uint64_t n = length < room ? length : room;
            last->length = static_cast<uint16_t>(last->length + n);
            noteRange(pa, n, isSealed);
            pa += n;
            length -= n;
            bytes += n;
        }
        return kAdmaOk;
    }

    SwitchAdmaRefusal addChecked(uint64_t pa, uint64_t length)
    {
        return addCheckedInternal(pa, length, false);
    }

    // [pa, pa + length) as transfer descriptors, continuing the previous descriptor when the memory continues it.
    template <typename PageOk> SwitchAdmaRefusal add(uint64_t pa, uint64_t length, PageOk pageOk)
    {
        if (const SwitchAdmaRefusal refusal = check(pa, length, pageOk))
            return refusal;
        return addCheckedInternal(pa, length, false);
    }

    // r14-sdcq: [pa, pa + length) in descriptors of their own: they continue no earlier descriptor and no later bytes
    // continue them, whatever lies next to them in physical memory. For the driver's own buffers in a request's table
    // (a gap's sink, a run staged in the bounce buffer): the cache maintenance passes over those descriptors
    // by their start address, which must never take a request's page along (or a request's descriptor theirs).
    SwitchAdmaRefusal addSealed(uint64_t pa, uint64_t length)
    {
        sealed = count;
        if (const SwitchAdmaRefusal refusal = check(pa, length, [](uint64_t) {
                return true;
            })) {
            sealed = count;
            return refusal;
        }
        const SwitchAdmaRefusal refusal = addCheckedInternal(pa, length, true);
        sealed = count;
        return refusal;
    }

    // Append [pa, pa + length) splitting on cacheable/uncached page boundaries so syncLines never re-queries pmap.
    // `pageKind(page)` returns 0=refused, 1=cacheable, 2=uncached.
    template <typename PageKind>
    SwitchAdmaRefusal addClassified(uint64_t pa, uint64_t length, PageKind pageKind, uint64_t *uncachedBits,
                                    uint32_t uncachedWords, bool *hadUncached)
    {
        if (!length)
            return kAdmaMissing;
        if ((pa | length) & (kAdmaLine - 1))
            return kAdmaAlign;
        if (pa >= limit || length > limit - pa)
            return kAdmaHigh;
        const uint64_t end = pa + length;
        const uint64_t firstPage = pa & ~(kAdmaPage - 1);
        const uint64_t numPages = ((end - 1) >> 12) - (firstPage >> 12) + 1;
        constexpr uint32_t kKindCap = 260;
        uint8_t kinds[kKindCap];
        bool segUncached = false;
        const bool cachedKinds = numPages <= kKindCap;
        uint64_t idx = 0;
        for (uint64_t page = firstPage; page < end; page += kAdmaPage, ++idx) {
            const uint8_t kind = pageKind(page);
            if (!kind)
                return kAdmaMemory;
            if (kind == 2)
                segUncached = true;
            if (cachedKinds)
                kinds[idx] = kind;
        }
        if (!segUncached)
            return addCheckedInternal(pa, length, false);
        *hadUncached = true;
        const auto kindAt = [&](uint64_t addr) -> uint8_t {
            const uint64_t p = addr & ~(kAdmaPage - 1);
            return cachedKinds ? kinds[(p - firstPage) >> 12] : pageKind(p);
        };
        for (uint64_t at = pa; at < end;) {
            const uint64_t atPage = at & ~(kAdmaPage - 1);
            const uint8_t runKind = kindAt(atPage);
            uint64_t stop = atPage + kAdmaPage < end ? atPage + kAdmaPage : end;
            while (stop < end && kindAt(stop) == runKind) {
                const uint64_t next = stop + kAdmaPage;
                stop = next < end ? next : end;
            }
            if (runKind == 2)
                sealed = count;
            const uint32_t firstDesc = count;
            if (const SwitchAdmaRefusal refusal = addCheckedInternal(at, stop - at, false)) {
                if (runKind == 2)
                    sealed = count;
                return refusal;
            }
            if (runKind == 2) {
                sealed = count;
                for (uint32_t d = firstDesc; d < count; ++d) {
                    if ((d >> 6) < uncachedWords)
                        uncachedBits[d >> 6] |= 1ULL << (d & 63);
                }
            }
            at = stop;
        }
        return kAdmaOk;
    }

    // The first `length` bytes of `memory`, one physical segment at a time (getPhysicalSegment, CPU addresses).
    template <typename Memory, typename PageOk>
    SwitchAdmaRefusal addMemory(Memory *memory, uint64_t length, PageOk pageOk)
    {
        for (uint64_t done = 0; done < length;) {
            IOByteCount segment = 0;
            const uint64_t pa = memory->getPhysicalSegment(done, &segment, kIOMemoryMapperNone);
            if (!pa || !segment)
                return kAdmaMissing;
            if (segment > length - done)
                segment = length - done;
            if (const SwitchAdmaRefusal refusal = add(pa, segment, pageOk))
                return refusal;
            done += segment;
        }
        return kAdmaOk;
    }

    template <typename Memory, typename PageKind>
    SwitchAdmaRefusal addMemoryClassified(Memory *memory, uint64_t length, PageKind pageKind, uint64_t *uncachedBits,
                                          uint32_t uncachedWords, bool *hadUncached)
    {
        for (uint64_t done = 0; done < length;) {
            IOByteCount segment = 0;
            const uint64_t pa = memory->getPhysicalSegment(done, &segment, kIOMemoryMapperNone);
            if (!pa || !segment)
                return kAdmaMissing;
            if (segment > length - done)
                segment = length - done;
            if (const SwitchAdmaRefusal refusal =
                    addClassified(pa, segment, pageKind, uncachedBits, uncachedWords, hadUncached))
                return refusal;
            done += segment;
        }
        return kAdmaOk;
    }

    // End on the last descriptor; false for an empty table.
    bool finish() const
    {
        if (!count)
            return false;
        entries[count - 1].attributes |= kAdmaEnd;
        return true;
    }
};

inline uint64_t switchAdmaAddress(const SwitchAdmaDescriptor &descriptor)
{
    return static_cast<uint64_t>(descriptor.addressHigh) << 32 | descriptor.addressLow;
}

inline bool switchAdmaEffectiveBounds(const SwitchAdmaTable &t, uint64_t skip, uint64_t skipBytes, uint64_t *lo,
                                      uint64_t *hi)
{
    if (t.boundsValid) {
        if (!skipBytes) {
            *lo = t.minPa;
            *hi = t.maxPa;
            return *lo < *hi;
        }
        if (t.sealedMinPa >= t.sealedMaxPa || (t.sealedMinPa >= skip && t.sealedMaxPa - skip <= skipBytes)) {
            *lo = t.unsealedMinPa;
            *hi = t.unsealedMaxPa;
            return *lo < *hi;
        }
    }
    uint64_t minPa = ~0ULL, maxPa = 0;
    for (uint32_t i = 0; i < t.count; ++i) {
        const uint64_t pa = switchAdmaAddress(t.entries[i]);
        if (skipBytes && pa - skip < skipBytes)
            continue;
        if (pa < minPa)
            minPa = pa;
        const uint64_t end = pa + t.entries[i].length;
        if (end > maxPa)
            maxPa = end;
    }
    *lo = minPa;
    *hi = maxPa;
    return minPa < maxPa;
}

inline void switchAdmaSortPacked(uint64_t *items, uint32_t n)
{
    static constexpr uint32_t kGaps[] = {57, 23, 10, 4, 1};
    for (const uint32_t gap : kGaps) {
        for (uint32_t i = gap; i < n; ++i) {
            const uint64_t key = items[i];
            uint32_t j = i;
            while (j >= gap && items[j - gap] > key) {
                items[j] = items[j - gap];
                j -= gap;
            }
            items[j] = key;
        }
    }
}

// Whether any descriptor in `a` overlaps any descriptor in `b` (excluding [skip, skip + skipBytes), e.g. the gap sink).
// All descriptors are already 64-byte cache-line aligned, so byte-range overlap equals cache-line overlap.
inline bool switchAdmaTablesOverlap(const SwitchAdmaTable &a, const SwitchAdmaTable &b, uint64_t skip = 0,
                                    uint64_t skipBytes = 0)
{
    if (!a.count || !b.count)
        return false;
    uint64_t aMin = ~0ULL, aMax = 0, bMin = ~0ULL, bMax = 0;
    if (!switchAdmaEffectiveBounds(a, skip, skipBytes, &aMin, &aMax) ||
        !switchAdmaEffectiveBounds(b, skip, skipBytes, &bMin, &bMax))
        return false;
    if (aMax <= bMin || bMax <= aMin)
        return false;

    if (a.count > 8 && b.count > 8) {
        constexpr uint32_t kPackCap = 256;
        uint64_t aPacked[kPackCap], bPacked[kPackCap];
        uint32_t aCount = 0, bCount = 0;
        bool packable = true;
        const auto pack = [&](const SwitchAdmaTable &t, uint64_t otherMin, uint64_t otherMax, uint64_t *out,
                              uint32_t *outCount) {
            for (uint32_t i = 0; i < t.count; ++i) {
                const uint64_t lo = switchAdmaAddress(t.entries[i]);
                if (skipBytes && lo - skip < skipBytes)
                    continue;
                const uint32_t len = t.entries[i].length;
                if (lo + len <= otherMin || lo >= otherMax)
                    continue;
                if (*outCount == kPackCap || ((lo | len) & (kAdmaLine - 1)) != 0 || (len >> 6) > 0x3ffu ||
                    (lo >> 60) != 0) {
                    packable = false;
                    return;
                }
                out[(*outCount)++] = ((lo >> 6) << 10) | (len >> 6);
            }
        };
        pack(a, bMin, bMax, aPacked, &aCount);
        if (packable)
            pack(b, aMin, aMax, bPacked, &bCount);
        if (packable) {
            if (!aCount || !bCount)
                return false;
            switchAdmaSortPacked(aPacked, aCount);
            switchAdmaSortPacked(bPacked, bCount);
            uint32_t i = 0, j = 0;
            while (i < aCount && j < bCount) {
                const uint64_t aLo = aPacked[i] >> 10;
                const uint64_t aHi = aLo + (aPacked[i] & 0x3ffu);
                const uint64_t bLo = bPacked[j] >> 10;
                const uint64_t bHi = bLo + (bPacked[j] & 0x3ffu);
                if (aLo < bHi && bLo < aHi)
                    return true;
                if (aHi <= bHi)
                    ++i;
                else
                    ++j;
            }
            return false;
        }
    }

    for (uint32_t i = 0; i < a.count; ++i) {
        const uint64_t aLo = switchAdmaAddress(a.entries[i]);
        if (skipBytes && aLo - skip < skipBytes)
            continue;
        const uint64_t aHi = aLo + a.entries[i].length;
        if (aHi <= bMin || aLo >= bMax)
            continue;
        for (uint32_t j = 0; j < b.count; ++j) {
            const uint64_t bLo = switchAdmaAddress(b.entries[j]);
            if (skipBytes && bLo - skip < skipBytes)
                continue;
            const uint64_t bHi = bLo + b.entries[j].length;
            if (aLo < bHi && bLo < aHi)
                return true;
        }
    }
    return false;
}

// r12-sdio: a read chain's table where each physical segment goes straight to the card when it may (check()) and only the
// others go through the bounce buffer. Until r12-sdio one such segment sent the whole chain through the bounce buffer:
// every byte copied, and the chain split into 256 KiB SDMA commands. A refused segment is placed in the bounce buffer at
// its own offset in the chain (the chain is at most kBounceBytes), consecutive refused segments form one run there, and
// the table points at that run where the segments would have been; the driver copies each recorded stage out to its
// request after the command. A segment goes straight only when its offset in the chain is on a cache line too, so every
// run starts and ends on one (the chain's start, the end of a straight segment, the start of the next one, or the chain's
// end, a whole sector) and the whole table stays in whole lines. Writes do not use this: a write chain with a refused
// segment still goes through the bounce buffer whole, the path every write took before r10-adma2.
template <typename Memory> struct SwitchAdmaStaging {
    struct Stage {
        Memory *memory;
        uint64_t offset;  // in the memory
        uint32_t at;      // in the chain, so also in the bounce buffer
        uint32_t length;
    };

    static constexpr uint64_t kNoRun = ~0ULL;
    SwitchAdmaTable *table;
    uint64_t bouncePA;  // physically contiguous, cacheable RAM below 4 GiB, as long as the longest chain
    Stage *stages;
    uint32_t capacity, count;
    uint64_t at, run, staged;  // the chain offset reached, where the open run began (or kNoRun), bytes staged
    uint32_t refused[kAdmaRefusals];

    void reset(SwitchAdmaTable *t, uint64_t bounce, Stage *s, uint32_t n)
    {
        table = t;
        bouncePA = bounce;
        stages = s;
        capacity = n;
        count = 0;
        at = 0;
        run = kNoRun;
        staged = 0;
        for (uint32_t &r : refused)
            r = 0;
        table->reset();
    }

    // The open run's descriptor (it precedes whatever comes next).
    SwitchAdmaRefusal closeRun()
    {
        if (run == kNoRun)
            return kAdmaOk;
        const SwitchAdmaRefusal refusal = table->addSealed(bouncePA + run, at - run);
        run = kNoRun;
        return refusal;
    }

    // The first `length` bytes of `memory` as the chain's next bytes. kAdmaOk, or kAdmaFull / kAdmaMissing when the chain
    // cannot be built this way at all (then it goes through the bounce buffer whole).
    template <typename PageOk> SwitchAdmaRefusal addMemory(Memory *memory, uint64_t length, PageOk pageOk)
    {
        for (uint64_t done = 0; done < length;) {
            IOByteCount segment = 0;
            const uint64_t pa = memory->getPhysicalSegment(done, &segment, kIOMemoryMapperNone);
            if (!pa || !segment)
                return kAdmaMissing;
            if (segment > length - done)
                segment = length - done;
            const SwitchAdmaRefusal why = (at & (kAdmaLine - 1)) ? kAdmaAlign : table->check(pa, segment, pageOk);
            if (why == kAdmaMissing)
                return why;
            if (!why) {
                if (const SwitchAdmaRefusal refusal = closeRun())
                    return refusal;
                if (const SwitchAdmaRefusal refusal = table->addChecked(pa, segment))
                    return refusal;
            } else {
                if (run == kNoRun)
                    run = at;
                Stage *last = count ? &stages[count - 1] : nullptr;
                if (last && last->memory == memory && last->offset + last->length == done &&
                    last->at + last->length == at) {
                    last->length += static_cast<uint32_t>(segment);
                } else {
                    if (count == capacity)
                        return kAdmaFull;
                    // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): staging is on only once its stages are allocated
                    stages[count++] = {memory, done, static_cast<uint32_t>(at), static_cast<uint32_t>(segment)};
                }
                ++refused[why];
                staged += segment;
            }
            at += segment;
            done += segment;
        }
        return kAdmaOk;
    }

    template <typename PageKind>
    SwitchAdmaRefusal addMemoryClassified(Memory *memory, uint64_t length, PageKind pageKind, uint64_t *uncachedBits,
                                          uint32_t uncachedWords, bool *hadUncached)
    {
        for (uint64_t done = 0; done < length;) {
            IOByteCount segment = 0;
            const uint64_t pa = memory->getPhysicalSegment(done, &segment, kIOMemoryMapperNone);
            if (!pa || !segment)
                return kAdmaMissing;
            if (segment > length - done)
                segment = length - done;
            SwitchAdmaRefusal why = kAdmaOk;
            if (at & (kAdmaLine - 1)) {
                why = kAdmaAlign;
            } else if ((pa | segment) & (kAdmaLine - 1)) {
                why = kAdmaAlign;
            } else if (pa >= table->limit || segment > table->limit - pa) {
                why = kAdmaHigh;
            } else {
                const uint64_t end = pa + segment;
                const uint64_t firstPage = pa & ~(kAdmaPage - 1);
                const uint64_t numPages = ((end - 1) >> 12) - (firstPage >> 12) + 1;
                constexpr uint32_t kKindCap = 260;
                uint8_t kinds[kKindCap];
                bool segUncached = false;
                const bool cachedKinds = numPages <= kKindCap;
                uint64_t idx = 0;
                for (uint64_t page = firstPage; page < end; page += kAdmaPage, ++idx) {
                    const uint8_t kind = pageKind(page);
                    if (!kind) {
                        why = kAdmaMemory;
                        break;
                    }
                    if (kind == 2)
                        segUncached = true;
                    if (cachedKinds)
                        kinds[idx] = kind;
                }
                if (!why) {
                    if (const SwitchAdmaRefusal closed = closeRun())
                        return closed;
                    if (!segUncached) {
                        if (const SwitchAdmaRefusal refusal = table->addChecked(pa, segment))
                            return refusal;
                    } else {
                        *hadUncached = true;
                        const auto kindAt = [&](uint64_t addr) -> uint8_t {
                            const uint64_t p = addr & ~(kAdmaPage - 1);
                            return cachedKinds ? kinds[(p - firstPage) >> 12] : pageKind(p);
                        };
                        for (uint64_t cur = pa; cur < end;) {
                            const uint64_t curPage = cur & ~(kAdmaPage - 1);
                            const uint8_t runKind = kindAt(curPage);
                            uint64_t stop = curPage + kAdmaPage < end ? curPage + kAdmaPage : end;
                            while (stop < end && kindAt(stop) == runKind) {
                                const uint64_t next = stop + kAdmaPage;
                                stop = next < end ? next : end;
                            }
                            if (runKind == 2)
                                table->sealed = table->count;
                            const uint32_t firstDesc = table->count;
                            if (const SwitchAdmaRefusal refusal = table->addChecked(cur, stop - cur)) {
                                if (runKind == 2)
                                    table->sealed = table->count;
                                return refusal;
                            }
                            if (runKind == 2) {
                                table->sealed = table->count;
                                for (uint32_t d = firstDesc; d < table->count; ++d) {
                                    if ((d >> 6) < uncachedWords)
                                        uncachedBits[d >> 6] |= 1ULL << (d & 63);
                                }
                            }
                            cur = stop;
                        }
                    }
                }
            }
            if (why) {
                if (run == kNoRun)
                    run = at;
                Stage *last = count ? &stages[count - 1] : nullptr;
                if (last && last->memory == memory && last->offset + last->length == done &&
                    last->at + last->length == at) {
                    last->length += static_cast<uint32_t>(segment);
                } else {
                    if (count == capacity)
                        return kAdmaFull;
                    // NOLINTNEXTLINE(clang-analyzer-core.CallAndMessage): staging is on only once its stages are allocated
                    stages[count++] = {memory, done, static_cast<uint32_t>(at), static_cast<uint32_t>(segment)};
                }
                ++refused[why];
                staged += segment;
            }
            at += segment;
            done += segment;
        }
        return kAdmaOk;
    }

    // r14-sdcq: `length` bytes of a read chain's gap (SwitchQueue.h), read into the sink at `pa`, which nothing reads: a
    // descriptor of its own (the open run closed first), so the chain's offsets after it stay right. `pa` is physically
    // contiguous, cacheable RAM below 4 GiB, line-aligned, as long as the longest gap.
    SwitchAdmaRefusal addSink(uint64_t pa, uint64_t length)
    {
        if (const SwitchAdmaRefusal refusal = closeRun())
            return refusal;
        if (const SwitchAdmaRefusal refusal = table->addSealed(pa, length))
            return refusal;
        at += length;
        return kAdmaOk;
    }

    // The last run's descriptor and End. False for an empty table or a full one.
    bool finish()
    {
        return !closeRun() && table->finish();
    }
};

// ADMA2 reaches every page below kAdmaDmaLimit in both directions (this board's RAM is 0x80000000-0x17fffffff, half of
// it above 4 GiB); every boot from r12-sdio to 23 Nov compared its first reads and writes up there with SDMA and found
// them equal.
constexpr uint64_t kAdmaDmaLimit = 1ULL << 34;  // mainline Linux's DMA mask for this controller
