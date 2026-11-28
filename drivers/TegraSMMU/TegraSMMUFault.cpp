// SPDX-License-Identifier: BSD-2-Clause-Patent
// The fault ASID's directory (TegraSMMU.h tegra_smmu_fault_ptb): one zeroed page, kept for good.
#include "TegraSMMU.h"
#include "TegraSMMUDomain.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

namespace {
uint32_t gFaultPtb;
switch_dma_buffer gFaultDirectory;
}  // namespace

extern "C" uint32_t tegra_smmu_fault_ptb(void)
{
    const uint32_t made = __atomic_load_n(&gFaultPtb, __ATOMIC_ACQUIRE);
    if (made)
        return made;
    switch_dma_buffer directory = {};
    if (!switch_dma_alloc(&directory, TegraSMMUDomain::kPage))
        return 0;
    const uint32_t ptb = static_cast<uint32_t>(directory.pa >> 12) | TegraSMMUDomain::kRead | TegraSMMUDomain::kWrite |
                         TegraSMMUDomain::kNonSecure;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&gFaultPtb, &expected, ptb, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        switch_dma_free(&directory);
        return expected;
    }
    gFaultDirectory = directory;
    return ptb;
}
