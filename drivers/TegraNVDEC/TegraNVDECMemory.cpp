// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVDEC's memory, behind the MC SMMU (ASID 123, drivers/TegraSMMU/TegraSMMUEngine.h). Every buffer a session asks for is
// any RAM pages, wired and mapped into NVDEC's 1 GiB window when allocated and unmapped when freed, so a 4K stream's
// pictures (12.5 MB each, as many as its DPB declares) need no contiguous memory. The firmware and the kernel's setup
// slots are identity-mapped: the falcon fetches its apps and the driver points SET_DRV_PIC_SETUP_OFFSET by physical
// address. What all sessions together may wire is capped (kWiredLimit), as the arena this replaces capped it.
#include <IOKit/IOLib.h>
#include "TegraNVDEC.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUEngine.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

namespace {
constexpr uint64_t kWindowBase = 1ULL << 30, kWindowBytes = 1ULL << 30;
constexpr uint32_t kIdentityTables = 4;  // the firmware and the setup slots: at most two 4 MiB sections each
constexpr uint64_t kWiredLimit = 512ULL << 20;
constexpr uint32_t kTranslation = (1u << 24) | (1u << 25);  // nvdecsrd, nvdecswr

// NVDEC's only memory clients are nvdecsrd and nvdecswr, both in swgroup "nvdec". MC_SMMU_NVDEC1_ASID (0xae0) has no
// client, and on the T210B01 it drops ENABLE when written, which failed N1 on every boot.
const TegraSMMUSwgroup kGroups[] = {
    {TEGRA_SMMU_NVDEC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_3, kTranslation},
};

TegraSMMUEngine gSpace;
uint64_t gWired;  // atomic: bytes of every live buffer

struct SwitchNVDECAllocation {
    switch_dma_pages pages;
    uint64_t iova, bytes;
};

void releaseAllocation(void *context)
{
    auto *allocation = static_cast<SwitchNVDECAllocation *>(context);
    gSpace.unmap(allocation->iova, allocation->pages.count);
    switch_dma_pages_free(&allocation->pages);
    __atomic_sub_fetch(&gWired, allocation->bytes, __ATOMIC_RELAXED);
    IODelete(allocation, SwitchNVDECAllocation, 1);
}
}  // namespace

extern "C" int tegra_nvdec_memory_init(const switch_dma_buffer *firmware, const switch_dma_buffer *setup, char *line,
                                       size_t size)
{
    if (!gSpace.init(TEGRA_SMMU_ASID_NVDEC, kWindowBase, kWindowBytes, kIdentityTables)) {
        snprintf(line, size, "N1 FAIL no contiguous memory below 4 GiB for NVDEC's SMMU tables");
        return -1;
    }
    if (!gSpace.identity(firmware->pa, firmware->size, false) || !gSpace.identity(setup->pa, setup->size, false)) {
        snprintf(line, size, "N1 FAIL the firmware and setup slots need more than %u identity tables", kIdentityTables);
        gSpace.release();
        return -1;
    }
    return tegra_nvdec_memory_attach(line, size);
}

extern "C" int tegra_nvdec_memory_attach(char *line, size_t size)
{
    char why[160];
    if (gSpace.attach(kGroups, sizeof(kGroups) / sizeof(kGroups[0]), why, sizeof(why))) {
        snprintf(line, size, "N1 FAIL nvdec swgroup: %s", why);
        return -1;
    }
    return 0;
}

extern "C" void tegra_nvdec_memory_detach(void)
{
    gSpace.detach();
}

extern "C" void tegra_nvdec_memory_release(void)
{
    gSpace.detach();
    gSpace.release();
}

extern "C" int tegra_nvdec_buffer_alloc(uint64_t size, TegraNVDECBlock *block)
{
    *block = {};
    const uint64_t bytes = TegraSMMUSpace::round(size);
    if (!size || !gSpace.lock)
        return -12;
    if (__atomic_add_fetch(&gWired, bytes, __ATOMIC_RELAXED) > kWiredLimit) {
        __atomic_sub_fetch(&gWired, bytes, __ATOMIC_RELAXED);
        return -12;
    }
    if (!switch_dma_pages_alloc(&block->pages, (size_t)bytes, kIOMemoryKernelUserShared)) {
        __atomic_sub_fetch(&gWired, bytes, __ATOMIC_RELAXED);
        return -12;
    }
    block->iova = gSpace.map(block->pages.pages, block->pages.count, true);
    if (!block->iova) {
        switch_dma_pages_free(&block->pages);
        __atomic_sub_fetch(&gWired, bytes, __ATOMIC_RELAXED);
        return -12;
    }
    block->size = bytes;
    block->va = block->pages.va;
    auto *allocation = IONew(SwitchNVDECAllocation, 1);
    if (allocation) {
        *allocation = {block->pages, block->iova, bytes};
        block->backing =
            switch_gpu_backing_register(block->pages.pages, block->pages.count, 0, allocation, releaseAllocation);
    }
    if (!block->backing) {
        if (allocation) {
            releaseAllocation(allocation);
        } else {
            gSpace.unmap(block->iova, block->pages.count);
            switch_dma_pages_free(&block->pages);
            __atomic_sub_fetch(&gWired, bytes, __ATOMIC_RELAXED);
        }
        *block = {};
        return -12;
    }
    return 0;
}

extern "C" void tegra_nvdec_buffer_free(TegraNVDECBlock *block)
{
    if (!block->iova)
        return;
    switch_gpu_backing_release(block->backing);
    *block = {};
}

extern "C" void tegra_nvdec_memory_usage(uint64_t *limit, uint64_t *free)
{
    const uint64_t wired = __atomic_load_n(&gWired, __ATOMIC_RELAXED);
    *limit = kWiredLimit;
    *free = wired < kWiredLimit ? kWiredLimit - wired : 0;
}
