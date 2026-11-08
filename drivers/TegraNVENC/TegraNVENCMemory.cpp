// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVENC's memory, behind the MC SMMU (ASID 122, drivers/TegraSMMU/TegraSMMUEngine.h), as NVDEC's: every buffer a session
// asks for is any RAM pages, wired and mapped into NVENC's window when allocated and unmapped when freed; the firmware,
// the setup slots and the scratch buffer are identity-mapped, since the driver points methods at them by physical
// address. What all sessions together may wire is capped (kWiredLimit).
#include <IOKit/IOLib.h>
#include "TegraNVENC.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUEngine.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

namespace {
constexpr uint64_t kWindowBase = 1ULL << 30, kWindowBytes = 1ULL << 30;
constexpr uint32_t kIdentityTables = 6;  // the firmware, setup slots and scratch: at most two 4 MiB sections each
constexpr uint64_t kWiredLimit = 256ULL << 20;

// The swgroup's one ASID register; its two clients' translation bits sit in two registers (nvencsrd, nvencswr).
const TegraSMMUSwgroup kGroups[] = {
    {TEGRA_SMMU_NVENC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_0, 1u << 28},
    {TEGRA_SMMU_NVENC_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_1, 1u << 11},
};

TegraSMMUEngine gSpace;
uint64_t gWired;  // atomic: bytes of every live buffer
}  // namespace

extern "C" int tegra_nvenc_memory_init(const switch_dma_buffer *firmware, const switch_dma_buffer *setup,
                                       const switch_dma_buffer *scratch, char *line, size_t size)
{
    if (!gSpace.init(TEGRA_SMMU_ASID_NVENC, kWindowBase, kWindowBytes, kIdentityTables)) {
        snprintf(line, size, "E1 FAIL no contiguous memory below 4 GiB for NVENC's SMMU tables");
        return -1;
    }
    if (!gSpace.identity(firmware->pa, firmware->size, false) || !gSpace.identity(setup->pa, setup->size, false) ||
        !gSpace.identity(scratch->pa, scratch->size, true)) {
        snprintf(line, size, "E1 FAIL the kernel's buffers need more than %u identity tables", kIdentityTables);
        gSpace.release();
        return -1;
    }
    return tegra_nvenc_memory_attach(line, size);
}

extern "C" int tegra_nvenc_memory_attach(char *line, size_t size)
{
    char why[160];
    if (gSpace.attach(kGroups, sizeof(kGroups) / sizeof(kGroups[0]), why, sizeof(why))) {
        snprintf(line, size, "E1 FAIL nvenc swgroup: %s", why);
        return -1;
    }
    return 0;
}

extern "C" void tegra_nvenc_memory_detach(void)
{
    gSpace.detach();
}

extern "C" void tegra_nvenc_memory_release(void)
{
    gSpace.detach();
    gSpace.release();
}

extern "C" int tegra_nvenc_buffer_alloc(uint64_t size, TegraNVENCBlock *block)
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
    return 0;
}

extern "C" void tegra_nvenc_buffer_free(TegraNVENCBlock *block)
{
    if (!block->iova)
        return;
    gSpace.unmap(block->iova, block->pages.count);
    switch_dma_pages_free(&block->pages);
    __atomic_sub_fetch(&gWired, block->size, __ATOMIC_RELAXED);
    *block = {};
}

extern "C" void tegra_nvenc_memory_usage(uint64_t *limit, uint64_t *free)
{
    const uint64_t wired = __atomic_load_n(&gWired, __ATOMIC_RELAXED);
    *limit = kWiredLimit;
    *free = wired < kWiredLimit ? kWiredLimit - wired : 0;
}
