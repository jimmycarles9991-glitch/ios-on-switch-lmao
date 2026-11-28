// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraSMMUAttachments.h"

namespace switch_smmu {
constexpr uint32_t kPtbAsid = 0x1c, kPtbData = 0x20, kTlbFlush = 0x30, kPtcFlush = 0x34, kPtcFlushHi = 0x9b8;
constexpr uint32_t kAsidEnable = 1u << 31, kTlbAsidMatch = 1u << 31, kTlbMatchSection = 2, kTlbSectionField = 0xffc00;
constexpr uint32_t kPtcFlushAddress = 1, kPtcGroup = 32, kPtcSixteenEntries = 64;
constexpr uint32_t kFlushCollapseLines = 8;
constexpr uint64_t kTlbSection = 4ULL << 20;
// T210 TRM 18.11.1.11: neither flush type needs to wait for the hit FIFO to go idle (bit 3 kept at reset value, 1).
constexpr uint32_t kPtcNoWaitIdle = 1u << 3;

template <class Registers> void flush(Registers &io, uint32_t asid)
{
    io.write(kPtcFlush, kPtcNoWaitIdle);
    (void)io.read(kPtbAsid);
    io.write(kTlbFlush, kTlbAsidMatch | (asid << 24));
    (void)io.read(kPtbAsid);
}

// The swgroup register's value: enabled, with the ASID in the one field, in all four of a 34-bit client's swgroup
// (one per va[33:32]; T210 TRM 18.11.1.84-85, p.894-895) as L4T's tegra-smmu writes them, or a split attachment's
// ASID in each field.
inline uint32_t selection(const tegra_smmu_attachment &a)
{
    uint32_t value = kAsidEnable;
    for (uint32_t field = 0; field < tegra_smmu_attachment_fields(&a); ++field)
        value |= tegra_smmu_attachment_asid(&a, field) << (8 * field);
    return value;
}

// Whether field is the first of the attachment's fields to name its ASID (a wide attachment names one ASID four times).
inline bool firstUse(const tegra_smmu_attachment &a, uint32_t field)
{
    for (uint32_t earlier = 0; earlier < field; ++earlier) {
        if (tegra_smmu_attachment_asid(&a, earlier) == tegra_smmu_attachment_asid(&a, field))
            return false;
    }
    return true;
}

// What a detach leaves in the swgroup register: bypass, or for a swgroup that always translates every field on the
// fault ASID, whose directory is empty.
inline uint32_t parked(const tegra_smmu_attachment &a, uint32_t faultAsid)
{
    return a.alwaysTranslated ? kAsidEnable | faultAsid | faultAsid << 8 | faultAsid << 16 | faultAsid << 24 : 0u;
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
    const uint64_t ptcStart = pa & ~uint64_t{kPtcSixteenEntries - 1};
    const uint64_t ptcEnd = pa + bytes;
    const uint64_t tlbStart = iova & ~(kTlbSection - 1);
    const uint64_t tlbEnd = iova + iovaBytes;
    const uint64_t ptcSteps = ptcEnd > ptcStart ? (ptcEnd - ptcStart + kPtcGroup - 1) / kPtcGroup : 0;
    const uint64_t tlbSteps = tlbEnd > tlbStart ? (tlbEnd - tlbStart + kTlbSection - 1) / kTlbSection : 0;
    if (ptcSteps > kFlushCollapseLines || tlbSteps > kFlushCollapseLines) {
        flush(io, asid);
        return;
    }
    io.write(kPtcFlushHi, static_cast<uint32_t>(pa >> 32) & 3);
    for (uint64_t at = ptcStart; at < ptcEnd; at += kPtcGroup)
        io.write(kPtcFlush, static_cast<uint32_t>(at) | kPtcNoWaitIdle | kPtcFlushAddress);
    for (uint64_t at = tlbStart; at < tlbEnd; at += kTlbSection)
        io.write(kTlbFlush, kTlbAsidMatch | (asid << 24) | (static_cast<uint32_t>(at >> 12) & kTlbSectionField) |
                                kTlbMatchSection);
    (void)io.read(kPtbAsid);
}

// flushRange for each table's groups (both 32-byte steps of each) and its section, then one read. Every table lies
// below 4 GiB (TegraSMMUDomain::kLimit), so PTC_FLUSH_1 is written once.
template <class Registers, class Table>
void flushTables(Registers &io, uint32_t asid, const Table *tables, uint32_t count)
{
    if (!count)
        return;
    uint32_t ptcSteps = 0;
    for (uint32_t i = 0; i < count; ++i)
        ptcSteps += static_cast<uint32_t>(__builtin_popcountll(tables[i].groups)) * 2u;
    if (ptcSteps > kFlushCollapseLines || count > kFlushCollapseLines) {
        flush(io, asid);
        return;
    }
    io.write(kPtcFlushHi, static_cast<uint32_t>(tables[0].pa >> 32) & 3);
    for (uint32_t i = 0; i < count; ++i) {
        for (uint64_t groups = tables[i].groups; groups; groups &= groups - 1) {
            const uint32_t at = static_cast<uint32_t>(tables[i].pa) +
                                static_cast<uint32_t>(__builtin_ctzll(groups)) * kPtcSixteenEntries;
            io.write(kPtcFlush, at | kPtcNoWaitIdle | kPtcFlushAddress);
            io.write(kPtcFlush, (at + kPtcGroup) | kPtcNoWaitIdle | kPtcFlushAddress);
        }
        io.write(kTlbFlush,
                 kTlbAsidMatch | (asid << 24) | ((tables[i].iova >> 12) & kTlbSectionField) | kTlbMatchSection);
    }
    (void)io.read(kPtbAsid);
}

template <class Registers>
int program(Registers &io, const tegra_smmu_attachment &a, char *line, size_t size,
            const tegra_smmu_attachments *attachments = nullptr)
{
    if (attachments) {
        for (uint32_t i = 0; i < attachments->count; ++i) {
            const auto &client = attachments->entry[i];
            if (client.asidReg == a.asidReg && !tegra_smmu_attachment_same_directories(&client, &a)) {
                snprintf(line, size, "conflicting retained directory for swgroup %#x", a.asidReg);
                return -1;
            }
        }
    }
    for (uint32_t field = 0; field < tegra_smmu_attachment_fields(&a); ++field) {
        if (!firstUse(a, field))
            continue;
        const uint32_t asid = tegra_smmu_attachment_asid(&a, field), ptb = tegra_smmu_attachment_ptb(&a, field);
        io.write(kPtbAsid, asid);
        io.write(kPtbData, ptb);
        (void)io.read(kPtbAsid);
        io.write(kPtbAsid, asid);
        const uint32_t directory = io.read(kPtbData);
        if (directory != ptb) {
            snprintf(line, size, "ASID %u directory did not take (PTB_DATA %#x, wrote %#x)", asid, directory, ptb);
            return -1;
        }
        flush(io, asid);
    }
    // The per-client translation enables are secure-only (T210 TRM 18.11.1.66-69, p.886-887): reset and bl31 set them,
    // a write from here is dropped, and what reads back here is checked. An always-translated swgroup's are read-only
    // and the swgroup translates by its software ID whatever they read (GPUB's gpusrd/gpuswr, 18.11.1.68): not checked.
    const uint32_t count = attachments ? attachments->count : 1;
    for (uint32_t i = 0; i < count; ++i) {
        const auto &client = attachments ? attachments->entry[i] : a;
        if (client.asidReg != a.asidReg || client.alwaysTranslated)
            continue;
        const uint32_t enabled = io.read(client.translationReg);
        if ((enabled & client.translationBits) != client.translationBits) {
            snprintf(line, size,
                     "the clients' translation bits are off (MC %#x = %#x) and only the secure world sets them",
                     client.translationReg, enabled);
            return -1;
        }
    }
    const uint32_t previous = io.read(a.asidReg);
    io.write(a.asidReg, selection(a));
    const uint32_t selected = io.read(a.asidReg);
    if (selected != selection(a)) {
        io.write(a.asidReg, previous);
        (void)io.read(a.asidReg);
        snprintf(line, size, "swgroup ASID did not take (MC %#x = %#x)", a.asidReg, selected);
        return -1;
    }
    for (uint32_t field = 0; field < tegra_smmu_attachment_fields(&a); ++field) {
        if (firstUse(a, field))
            flush(io, tegra_smmu_attachment_asid(&a, field));
    }
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
            return program(io, a, line, size, &attachments);
        break;
    }
    snprintf(line, size, "no retained directory for ASID %u swgroup %#x", asid, asidReg);
    return -1;
}

// The TLB's and PTC's hit and miss counters (TegraSMMU.h switch_smmu_stats_window). They wrap and have no reset, so a
// window is the difference of two readings.
constexpr uint32_t kTlbConfig = 0x14, kPtcConfig = 0x18, kStatsEnable = 1u << 31, kStatsCounts = 0x1f0;

struct Stats {
    uint32_t tlbHit, tlbMiss, ptcHit, ptcMiss;
};

// Both caches' counters on, every other bit of their configuration kept; false when either was off (reset, SC7).
template <class Registers> bool statsOn(Registers &io)
{
    const uint32_t tlb = io.read(kTlbConfig), ptc = io.read(kPtcConfig);
    if (tlb & ptc & kStatsEnable)
        return true;
    io.write(kTlbConfig, tlb | kStatsEnable);
    io.write(kPtcConfig, ptc | kStatsEnable);
    return false;
}

template <class Registers> Stats stats(Registers &io)
{
    return {io.read(kStatsCounts), io.read(kStatsCounts + 4), io.read(kStatsCounts + 8), io.read(kStatsCounts + 12)};
}
}
