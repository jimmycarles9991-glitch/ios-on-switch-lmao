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
// front of it, which covers this board's DRAM (0x80000000-0x17fffffff); the driver still checks addresses above 4 GiB on
// the hardware before it lets the data path use them (limit): setup()'s check when its test pages land there, else the
// first read chains that reach them (r12-sdio: `high` counts their bytes).
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

    void reset()
    {
        count = 0;
        bytes = 0;
        high = 0;
        sealed = 0;
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

    // [pa, pa + length) as transfer descriptors, continuing the previous descriptor when the memory continues it.
    template <typename PageOk> SwitchAdmaRefusal add(uint64_t pa, uint64_t length, PageOk pageOk)
    {
        if (const SwitchAdmaRefusal refusal = check(pa, length, pageOk))
            return refusal;
        if (pa + length > kAdma4GiB)
            high += pa >= kAdma4GiB ? length : pa + length - kAdma4GiB;
        while (length) {
            // No descriptor crosses a 4 GiB boundary (r12-sdio: the engine's address word pair is only proven per
            // descriptor; Linux's default DMA segment boundary is 0xffffffff too): what fits up to the next one.
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
            pa += n;
            length -= n;
            bytes += n;
        }
        return kAdmaOk;
    }

    // r14-sdcq: [pa, pa + length) in descriptors of their own: they continue no earlier descriptor and no later bytes
    // continue them, whatever lies next to them in physical memory. For the driver's own buffers in a request's table
    // (a gap's sink, a run staged in the bounce buffer): the cache maintenance and the proofs pass over those descriptors
    // by their start address, which must never take a request's page along (or a request's descriptor theirs).
    SwitchAdmaRefusal addSealed(uint64_t pa, uint64_t length)
    {
        sealed = count;
        const SwitchAdmaRefusal refusal = add(pa, length, [](uint64_t) {
            return true;
        });
        sealed = count;
        return refusal;
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
                if (const SwitchAdmaRefusal refusal = table->add(pa, segment, pageOk))
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

// r12-sdio: whether ADMA2 may reach the requests' pages above 4 GiB (this board's RAM is 0x80000000-0x17fffffff, so half
// of it). setup()'s check decides it when some of its test pages land up there; at boot the VM hands out pages in rising
// physical order, so they usually all sit below 4 GiB (r7p5: "0 of 0 test pages above 4 GiB"), and from ~20 s of uptime
// 60% of requests then went through the bounce buffer for that alone. So the proof moves to the data path:
// - Proving: a read chain may use pages above 4 GiB, except those whose 4 GiB-lower alias is IRAM (0x40000000, 256 KiB):
//   if the engine dropped the high address word, the card's bytes would land 4 GiB lower, which is below DRAM and
//   outside the memory controller's EMEM aperture, everywhere but there. The driver poisons those pages, reads, reads
//   the same blocks again by SDMA and compares; `kProofs` such chains that match turn it on. Writes stay below 4 GiB.
// - On: tables reach up to `kAdmaDmaLimit` in both directions; the first `kWriteProofs` writes with pages above 4 GiB are
//   read back (a dropped address bit on the engine's memory reads would put the wrong bytes on the card).
// - Off (a proof differed, or the engine stopped on an ADMA error): everything above 4 GiB through the bounce buffer
//   until reboot, as before r12-sdio. setup()'s own check can start in On (its high test pages matched) or Off (they kept
//   their fill: the engine drops the high word).
constexpr uint64_t kAdmaDmaLimit = 1ULL << 34;  // mainline Linux's DMA mask for this controller
constexpr uint64_t kAdmaIramAlias = 0x140000000ULL, kAdmaIramBytes = 0x40000;

struct SwitchAdmaHigh {
    enum State : uint8_t {
        kProving,
        kOn,
        kOff
    };

#if DEVELOPMENT || DEBUG
    static constexpr bool kLiveProofs = true;
#else
    static constexpr bool kLiveProofs = false;
#endif
    static constexpr uint32_t kProofs = 16, kWriteProofs = 16;
    State state = kLiveProofs ? kProving : kOn;
    uint32_t proofsLeft = kLiveProofs ? kProofs : 0, writeProofsLeft = kLiveProofs ? kWriteProofs : 0;
    uint64_t provenBytes = 0;  // above 4 GiB, in the proofs that matched

    // The table's address limit for a command in this direction.
    uint64_t limit(bool write) const
    {
        return state == kOn || (state == kProving && !write) ? kAdmaDmaLimit : kAdma4GiB;
    }

    // Whether a page may go straight to the card in this direction (on top of the driver's own RAM check).
    bool pageOk(uint64_t page, bool write) const
    {
        return state != kProving || write || page - kAdmaIramAlias >= kAdmaIramBytes;
    }

    // A direct read whose table reaches above 4 GiB, while proving: compare it with SDMA before it completes.
    bool proofDue(bool write, uint64_t high) const
    {
        return state == kProving && !write && (high != 0u);
    }

    // A matching proof; true when it was the last one needed (the caller logs that once).
    bool proved(uint64_t high)
    {
        provenBytes += high;
        if (state != kProving || --proofsLeft)
            return false;
        state = kOn;
        return true;
    }

    void refuse()
    {
        state = kOff;
    }

    // A direct write whose table reaches above 4 GiB, while its read-backs last.
    bool writeCheckDue(bool write, uint64_t high) const
    {
        return state == kOn && write && (high != 0u) && (writeProofsLeft != 0u);
    }

    void writeChecked()
    {
        if (writeProofsLeft)
            --writeProofsLeft;
    }

    // Whether proofs are still to come (reads; with `writes`, ADMA2 writes being on, the writes' read-backs too).
    bool checking(bool writes) const
    {
        return state == kProving || (writes && state == kOn && (writeProofsLeft != 0u));
    }
};
