// SPDX-License-Identifier: BSD-2-Clause-Patent
// A change to an attached domain (TegraSMMUDomain.h) made visible to its engines: the table bytes written go to memory
// (the SMMU does not snoop the CPU's caches; op is the caller's clean or clean-and-invalidate), then the SMMU drops its
// cached copies. Page entries alone flush only their PTE groups and the ASID's TLB sections they cover
// (switch_smmu_flush_range); a directory entry flushes everything (tegra_smmu_flush). Nothing written, nothing flushed.
#pragma once
#include "TegraSMMU.h"
#include "TegraSMMUDomain.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

inline void switch_smmu_publish(TegraSMMUDomain &domain, uint32_t asid, switch_dcache_op op)
{
    TegraSMMUDomain::Change change = {};
    if (!domain.takeChange(&change))
        return;
    switch_dcache_lines(op, reinterpret_cast<const void *>(change.low), change.high - change.low);
    switch_dcache_sync();
    if (change.directory)
        tegra_smmu_flush(asid);
    else
        switch_smmu_flush_range(asid, change.pa, change.high - change.low, change.iovaLow,
                                change.iovaHigh - change.iovaLow);
}
