// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif
#include "SwitchVICMapping.h"
#include "../TegraGM20B/SwitchGpuBackingMaps.h"

struct SwitchVICMapping : SwitchGpuBackingMap {};

namespace {
struct VICSpace {
    static uint64_t map(const uint64_t *pages, uint32_t count, bool writable)
    {
        return switch_vic_mapping_create(pages, count, writable);
    }

    static void unmap(uint64_t address, uint32_t count)
    {
        switch_vic_mapping_destroy(address, count);
    }
};

// A decoder's pool and its client's surfaces at 4K, with the encoder's and the display's conversions beside them,
// within VIC's 1 GiB window, which also holds each mapping's tail pages (TegraVIC.cpp).
constinit SwitchGpuBackingMaps<SwitchVICMapping, VICSpace, SWITCH_GPU_ENGINE_VIC, 128, 768ULL << 20> gMappings;

void forget(uint64_t allocation)
{
    gMappings.forget(allocation);
}
}  // namespace

bool switch_vic_mapping_start()
{
    if (!gMappings.start(forget))
        return false;
    switch_vic_totals_register();
    return true;
}

void switch_vic_mapping_totals(uint64_t *made, uint64_t *remade)
{
    const SwitchGpuBackingMapTotals totals = gMappings.readTotals();
    *made = totals.made;
    *remade = totals.remade;
}

SwitchVICMapping *switch_vic_mapping_acquire_range(switch_gpu_backing *backing, bool writable, uint32_t firstPage,
                                                   uint32_t pageCount)
{
    return gMappings.acquire(backing, writable, firstPage, pageCount);
}

SwitchVICMapping *switch_vic_mapping_acquire_described(switch_gpu_backing *backing, const switch_gpu_backing_info &info,
                                                       bool writable)
{
    return gMappings.acquire(backing, info, writable, 0, info.page_count);
}

SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *backing, bool writable)
{
    return gMappings.acquire(backing, writable);
}

void switch_vic_mapping_release(SwitchVICMapping *mapping)
{
    gMappings.release(mapping);
}

uint64_t switch_vic_mapping_take_made()
{
    return gMappings.takeMade();
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
// For the GPU stats line's memory-controller error: where a VIC read (client 0x6c, vicsrd) the SMMU refused fell,
// against the mappings VIC holds now (a refused read past a surface's pages names the over-fetch a521ab19b maps).
extern "C" int switch_vic_mapping_describe(uint32_t iova, char *out, size_t size)
{
    if (!gMappings.lock || !out || !size)
        return -22;
    IOLockLock(gMappings.lock);
    const SwitchVICMapping *nearest = nullptr;
    for (const auto &mapping : gMappings.maps) {
        if (mapping.address && mapping.address <= iova && (!nearest || mapping.address > nearest->address))
            nearest = &mapping;
    }
    if (!nearest) {
        snprintf(out, size, "no VIC mapping below %#x", iova);
    } else {
        const uint64_t offset = iova - nearest->address;
        const uint64_t bytes = static_cast<uint64_t>(nearest->pages) << 12;
        snprintf(out, size, "VIC %s mapping %#llx of %u pages: %#llx %s", nearest->writable ? "write" : "read",
                 static_cast<unsigned long long>(nearest->address), nearest->pages,
                 static_cast<unsigned long long>(offset < bytes ? offset : offset - bytes),
                 offset < bytes ? "into its pages" : "past its last page");
    }
    IOLockUnlock(gMappings.lock);
    return 0;
}
#endif

uint64_t switch_vic_mapping_address(const SwitchVICMapping *mapping)
{
    return mapping ? mapping->address : 0;
}
