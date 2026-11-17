// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraSMMUAttachments.h"

namespace switch_smmu {
constexpr uint32_t kPtbAsid = 0x1c, kPtbData = 0x20, kTlbFlush = 0x30, kPtcFlush = 0x34, kPtcFlushHi = 0x9b8;
constexpr uint32_t kAsidEnable = 1u << 31, kTlbAsidMatch = 1u << 31, kTlbMatchSection = 2, kTlbSectionField = 0xffc00;
constexpr uint32_t kPtcFlushAddress = 1, kPtcGroup = 32, kPtcSixteenEntries = 64;
constexpr uint64_t kTlbSection = 4ULL << 20;

template <class Registers> void flush(Registers &io, uint32_t asid)
{
    io.write(kPtcFlush, 0);
    (void)io.read(kPtbAsid);
    io.write(kTlbFlush, kTlbAsidMatch | (asid << 24));
    (void)io.read(kPtbAsid);
}

// After page entries changed (TRM 18.6.3.3): the PTC's copies of the table bytes [pa, pa + bytes), then the ASID's TLB
// entries in each 4 MiB section of [iova, iova + iovaBytes); every other ASID keeps its cached translations. A PTC
// flush by address drops one PTE group, a memory atom: 32 bytes on this MC (18.3.1), where 18.6.3.3 speaks of 16
// PTEs (64 bytes). The steps are 32 bytes from the 64-byte boundary below the range, so each group is named at its
// own aligned address under either reading. PA[33:32] goes in PTC_FLUSH_1 once: the range never crosses 4 GiB (the
// domain's tables are below it). The read makes the flushes reach the MC before the caller lets the engine run.
template <class Registers>
void flushRange(Registers &io, uint32_t asid, uint64_t pa, uint64_t bytes, uint64_t iova, uint64_t iovaBytes)
{
    io.write(kPtcFlushHi, static_cast<uint32_t>(pa >> 32) & 3);
    for (uint64_t at = pa & ~uint64_t{kPtcSixteenEntries - 1}; at < pa + bytes; at += kPtcGroup)
        io.write(kPtcFlush, static_cast<uint32_t>(at) | kPtcFlushAddress);
    for (uint64_t at = iova & ~(kTlbSection - 1); at < iova + iovaBytes; at += kTlbSection)
        io.write(kTlbFlush, kTlbAsidMatch | (asid << 24) | (static_cast<uint32_t>(at >> 12) & kTlbSectionField) |
                                kTlbMatchSection);
    (void)io.read(kPtbAsid);
}

template <class Registers> int program(Registers &io, const tegra_smmu_attachment &a, char *line, size_t size)
{
    io.write(kPtbAsid, a.asid);
    io.write(kPtbData, a.ptb);
    (void)io.read(kPtbAsid);
    io.write(kPtbAsid, a.asid);
    const uint32_t directory = io.read(kPtbData);
    if (directory != a.ptb) {
        snprintf(line, size, "ASID %u directory did not take (PTB_DATA %#x, wrote %#x)", a.asid, directory, a.ptb);
        return -1;
    }
    flush(io, a.asid);
    io.write(a.translationReg, io.read(a.translationReg) | a.translationBits);
    if ((io.read(a.translationReg) & a.translationBits) != a.translationBits) {
        snprintf(line, size, "swgroup translation bits did not take (MC %#x)", a.translationReg);
        return -1;
    }
    io.write(a.asidReg, kAsidEnable | a.asid);
    const uint32_t selected = io.read(a.asidReg);
    if (selected != (kAsidEnable | a.asid)) {
        snprintf(line, size, "swgroup ASID did not take (MC %#x = %#x)", a.asidReg, selected);
        return -1;
    }
    flush(io, a.asid);
    return 0;
}

template <class Registers>
int restore(Registers &io, const tegra_smmu_attachments &attachments, uint32_t asid, uint32_t ptb, uint32_t asidReg,
            char *line, size_t size)
{
    for (uint32_t i = 0; i < attachments.count; ++i) {
        const auto &a = attachments.entry[i];
        if (a.asidReg != asidReg)
            continue;
        if (a.asid == asid && a.ptb == ptb)
            return program(io, a, line, size);
        break;
    }
    snprintf(line, size, "no retained directory for ASID %u swgroup %#x", asid, asidReg);
    return -1;
}
}
