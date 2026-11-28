// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "TegraNVJPG.h"
#include "TegraNVJPGMapping.h"
#include "TegraNVJPGStats.h"
#include "../TegraSMMU/TegraSMMUSpace.h"
#include "../../kits/SwitchDMA/SwitchHwPage.h"
#include "../../kits/SwitchMath/SwitchMath.h"

namespace {
// The GPU driver's backing retirement, before the pages go back (no job holds the backing). It takes the map lock
// only: every engine's backing frees wait behind it, so it never waits for a decode.
void destroy(void *context)
{
    auto *mapping = static_cast<SwitchNVJPGMapping *>(context);
    while (mapping) {
        auto *next = mapping->next;
        tegra_nvjpg_pages_unmap(TEGRA_NVJPG_WINDOW_OUT, mapping->address, mapping->mapped);
        IODelete(mapping, SwitchNVJPGMapping, 1);
        mapping = next;
    }
}
}  // namespace

SwitchNVJPGBacking switch_nvjpg_backing(switch_gpu_backing *backing)
{
    SwitchNVJPGBacking target = {};
    target.backing = backing;
    target.head =
        static_cast<SwitchNVJPGMapping *>(switch_gpu_backing_mapping_context(backing, SWITCH_GPU_ENGINE_NVJPG));
    if (target.head) {
        target.pages = target.head->pages;
        target.count = target.head->available;
        return target;
    }
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(backing, &info);
    target.pages = info.pages;
    target.count = info.page_count;
    target.generation = info.allocation_generation;
    return target;
}

SwitchNVJPGMapping *switch_nvjpg_mapping(const SwitchNVJPGBacking &target, uint32_t extent, uint64_t writeEnd,
                                         const char **why, int pid)
{
    const uint32_t mapped =
        SWITCH_MAX(extent, static_cast<uint32_t>(switch_div_round_up(writeEnd, SWITCH_HW_PAGE_SIZE)));
    for (auto *mapping = target.head; mapping; mapping = mapping->next) {
        if (mapping->count == extent && mapping->mapped >= mapped)
            return mapping;
    }
    auto *mapping = IONewZero(SwitchNVJPGMapping, 1);
    if (!mapping) {
        *why = "no memory for a destination mapping";
        return nullptr;
    }
    mapping->address = tegra_nvjpg_pages_map(TEGRA_NVJPG_WINDOW_OUT, target.pages, extent, mapped - extent, true);
    if (!mapping->address) {
        IODelete(mapping, SwitchNVJPGMapping, 1);
        *why = "output window full";
        return nullptr;
    }
    mapping->pages = target.pages;
    mapping->count = extent;
    mapping->mapped = mapped;
    mapping->available = target.count;
    if (!target.head && switch_gpu_backing_mapping_install(target.backing, SWITCH_GPU_ENGINE_NVJPG, target.generation,
                                                           mapping, destroy)) {
        destroy(mapping);
        *why = "destination backing changed before it was mapped";
        return nullptr;
    }
    if (target.head) {
        mapping->next = target.head->next;
        target.head->next = mapping;
    }
#if TEGRA_NVJPG_PROBES
    if (pid)
        switch_nvjpg_count_mapping(pid);
#else
    (void)pid;
#endif
    return mapping;
}

uint32_t switch_nvjpg_scan_pick(const SwitchNVJPGScan *scans, uint32_t freeSlots, uint64_t bytes)
{
    uint32_t pick = static_cast<uint32_t>(__builtin_ctz(freeSlots));
    for (uint32_t rest = freeSlots & (freeSlots - 1); rest; rest &= rest - 1) {
        const uint32_t i = static_cast<uint32_t>(__builtin_ctz(rest));
        const uint64_t size = scans[i].buffer.size, best = scans[pick].buffer.size;
        const bool fits = size >= bytes, bestFits = best >= bytes;
        if (fits != bestFits ? fits : (fits ? size < best : size > best))
            pick = i;
    }
    return pick;
}

int switch_nvjpg_scan_fit(SwitchNVJPGScan *scan, uint64_t bytes)
{
    if (scan->buffer.size >= bytes)
        return 0;
    if (scan->address)
        tegra_nvjpg_pages_unmap(TEGRA_NVJPG_WINDOW_IN, scan->address, scan->buffer.count + 1);
    switch_dma_pages_free(&scan->buffer);
    scan->address = 0;
    if (!switch_dma_pages_alloc(&scan->buffer, switch_align_up(bytes, TegraSMMUSpace::kGranule)))
        return -12;
    scan->address = tegra_nvjpg_pages_map(TEGRA_NVJPG_WINDOW_IN, scan->buffer.pages, scan->buffer.count, 1, false);
    if (scan->address)
        return 0;
    switch_dma_pages_free(&scan->buffer);
    return -28;
}
