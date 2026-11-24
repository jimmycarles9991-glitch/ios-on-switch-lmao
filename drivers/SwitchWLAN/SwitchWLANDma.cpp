// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchWLANDma.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUDomain.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

namespace {
constexpr uint32_t kAsid = TEGRA_SMMU_ASID_AFI;
constexpr uint32_t kTablePages = 3;
constexpr uint64_t kMaximumPool = 2ULL << 20;

// The MC retains this directory across driver failures and sleep; the endpoint must never reach its table pages.
switch_dma_buffer gTables;
TegraSMMUDomain gDomain;
uint64_t gPoolAddress;
size_t gPoolBytes;
bool gAttached;

void publishTables()
{
    uintptr_t low = 0, high = 0;
    if (gDomain.takeDirty(&low, &high))
        switch_dcache_clean(reinterpret_cast<const void *>(low), high - low);
    tegra_smmu_flush(kAsid);
}
}

bool switch_wlan_dma_start(char *why, size_t size)
{
    if (!gTables.memory && !switch_dma_alloc(&gTables, kTablePages * TegraSMMUDomain::kPage)) {
        snprintf(why, size, "no memory for PCIe DMA isolation tables");
        return false;
    }
    if (!gDomain.va && !gDomain.init(gTables.va, gTables.pa, kTablePages)) {
        snprintf(why, size, "invalid PCIe DMA isolation tables");
        return false;
    }
    publishTables();
    if (gAttached)
        return !switch_smmu_restore(kAsid, gDomain.ptb(), TEGRA_SMMU_AFI_ASID, why, size);
    if (tegra_smmu_attach(kAsid, gDomain.ptb(), TEGRA_SMMU_AFI_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_0, 1u << 14, why,
                          size) ||
        tegra_smmu_attach(kAsid, gDomain.ptb(), TEGRA_SMMU_AFI_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_1, 1u << 17, why,
                          size))
        return false;
    gAttached = true;
    return true;
}

bool switch_wlan_dma_map(uint64_t address, size_t bytes, char *why, size_t size)
{
    if (!gAttached || !bytes || bytes > kMaximumPool || ((address | bytes) & (TegraSMMUDomain::kPage - 1)) ||
        address >= TegraSMMUDomain::kLimit || bytes > TegraSMMUDomain::kLimit - address) {
        snprintf(why, size, "invalid PCIe DMA pool range");
        return false;
    }
    if (gPoolBytes && (gPoolAddress != address || gPoolBytes != bytes)) {
        snprintf(why, size, "PCIe DMA pool changed without retiring its mapping");
        return false;
    }
    if (address < gTables.pa + gTables.size && gTables.pa < address + bytes) {
        snprintf(why, size, "PCIe DMA pool overlaps its isolation tables");
        return false;
    }
    for (uint64_t page = address; page < address + bytes; page += TegraSMMUDomain::kPage) {
        if (!switch_dma_pa_managed(page)) {
            snprintf(why, size, "PCIe DMA pool is outside managed RAM");
            return false;
        }
    }
    if (!gPoolBytes) {
        if (!gDomain.identity(address, bytes, true)) {
            snprintf(why, size, "PCIe DMA pool exceeds its isolation tables");
            return false;
        }
        publishTables();
        gPoolAddress = address;
        gPoolBytes = bytes;
    }
    if (switch_smmu_restore(kAsid, gDomain.ptb(), TEGRA_SMMU_AFI_ASID, why, size))
        return false;
    return true;
}
