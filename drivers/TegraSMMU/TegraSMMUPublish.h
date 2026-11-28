// SPDX-License-Identifier: BSD-2-Clause-Patent
// A change to an attached domain (TegraSMMUDomain.h) made visible to its engines: the table bytes written go to memory
// (the SMMU does not snoop the CPU's caches; op is the caller's clean or clean-and-invalidate), then the SMMU drops its
// cached copies. Page entries alone flush only their PTE groups and the ASID's TLB sections they cover
// (switch_smmu_flush_range); a directory entry flushes everything (tegra_smmu_flush). Nothing written, nothing flushed.
#pragma once
#include "TegraSMMU.h"
#include "TegraSMMUDomain.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

namespace switch_smmu_detail {
constexpr uint64_t kPtcGroupBytes = 32, kPtcSixteenBytes = 64, kTlbSectionBytes = 4ULL << 20;
constexpr uint64_t kFlushCollapseLines = 8;

inline void cleanChanges(const TegraSMMUDomain::Changes &changes, switch_dcache_op op)
{
    if (changes.directory || changes.overflow || !changes.count) {
        switch_dcache_lines(op, reinterpret_cast<const void *>(changes.dirtyLow), changes.dirtyHigh - changes.dirtyLow);
    } else {
        for (uint32_t i = 0; i < changes.count; ++i)
            switch_dcache_lines(op, reinterpret_cast<const void *>(changes.ranges[i].low),
                                changes.ranges[i].high - changes.ranges[i].low);
    }
    switch_dcache_sync();
}

inline void flushChanges(const TegraSMMUDomain::Changes &changes, uint32_t asid, bool wholeGranules)
{
    if (changes.directory || changes.overflow || !changes.count) {
        tegra_smmu_flush(asid);
        return;
    }
    if (changes.count == 1) {
        const auto &r = changes.ranges[0];
        uint64_t startPa = r.pa, bytes = r.high - r.low;
        if (wholeGranules) {
            const uint64_t endPa = (startPa + bytes + kPtcSixteenBytes - 1) & ~(kPtcSixteenBytes - 1);
            startPa &= ~(kPtcSixteenBytes - 1);
            bytes = endPa - startPa;
        }
        switch_smmu_flush_range(asid, startPa, bytes, r.iovaLow, r.iovaHigh - r.iovaLow);
        return;
    }
    uint64_t ptcSteps = 0, tlbSteps = 0;
    for (uint32_t i = 0; i < changes.count; ++i) {
        const auto &r = changes.ranges[i];
        const uint64_t ptcStart = r.pa & ~(kPtcSixteenBytes - 1);
        uint64_t ptcEnd = r.pa + (r.high - r.low);
        if (wholeGranules)
            ptcEnd = (ptcEnd + kPtcSixteenBytes - 1) & ~(kPtcSixteenBytes - 1);
        const uint64_t tlbStart = r.iovaLow & ~(kTlbSectionBytes - 1);
        ptcSteps += (ptcEnd - ptcStart + kPtcGroupBytes - 1) / kPtcGroupBytes;
        tlbSteps += (r.iovaHigh - tlbStart + kTlbSectionBytes - 1) / kTlbSectionBytes;
        if (ptcSteps > kFlushCollapseLines || tlbSteps > kFlushCollapseLines) {
            tegra_smmu_flush(asid);
            return;
        }
    }
    for (uint32_t i = 0; i < changes.count; ++i) {
        const auto &r = changes.ranges[i];
        uint64_t startPa = r.pa, bytes = r.high - r.low;
        if (wholeGranules) {
            const uint64_t endPa = (startPa + bytes + kPtcSixteenBytes - 1) & ~(kPtcSixteenBytes - 1);
            startPa &= ~(kPtcSixteenBytes - 1);
            bytes = endPa - startPa;
        }
        switch_smmu_flush_range(asid, startPa, bytes, r.iovaLow, r.iovaHigh - r.iovaLow);
    }
}
}  // namespace switch_smmu_detail

inline void switch_smmu_publish(TegraSMMUDomain &domain, uint32_t asid, switch_dcache_op op)
{
    TegraSMMUDomain::Changes changes = {};
    if (!domain.takeChanges(&changes))
        return;
    switch_smmu_detail::cleanChanges(changes, op);
    switch_smmu_detail::flushChanges(changes, asid, false);
}

// Fresh 64 KiB-granule mappings in a pre-reserved window: Tegra210 SMMU fills PTC/TLB only on client access and unmap
// flushes both 32-byte atoms of every freed granule before reuse, so no stale PTC or TLB entry exists.
inline void switch_smmu_publish_fresh(TegraSMMUDomain &domain, uint32_t asid, switch_dcache_op op)
{
    TegraSMMUDomain::Changes changes = {};
    if (!domain.takeChanges(&changes))
        return;
    switch_smmu_detail::cleanChanges(changes, op);
    if (changes.directory)
        tegra_smmu_flush(asid);
}

// Unmap of 64 KiB granules: flush full 64-byte PTE groups (both 32-byte atoms) before granules return to the space.
inline void switch_smmu_publish_granules(TegraSMMUDomain &domain, uint32_t asid, switch_dcache_op op)
{
    TegraSMMUDomain::Changes changes = {};
    if (!domain.takeChanges(&changes))
        return;
    switch_smmu_detail::cleanChanges(changes, op);
    switch_smmu_detail::flushChanges(changes, asid, true);
}
