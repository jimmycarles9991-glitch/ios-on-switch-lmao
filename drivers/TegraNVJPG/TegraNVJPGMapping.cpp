// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "TegraNVJPG.h"
#include "TegraNVJPGMapping.h"
#include "TegraNVJPGStats.h"
#include "../../kits/SwitchDMA/SwitchHwPage.h"

namespace {
constexpr uint32_t kSinkPages = (1U << 20) / SWITCH_HW_PAGE_SIZE;
uint64_t gSink;

void destroy(void *context)
{
    auto *mapping = static_cast<SwitchNVJPGMapping *>(context);
    tegra_nvjpg_lock();
    while (mapping) {
        auto *next = mapping->next;
        tegra_nvjpg_pages_unmap(mapping->address, mapping->mapped);
        IODelete(mapping, SwitchNVJPGMapping, 1);
        mapping = next;
    }
    tegra_nvjpg_unlock();
}
}  // namespace

void switch_nvjpg_mapping_start(uint64_t sink)
{
    gSink = sink;
}

SwitchNVJPGMapping *switch_nvjpg_mapping(switch_gpu_backing *backing, uint32_t extent, int pid)
{
    if (!backing)
        return nullptr;
    auto *head =
        static_cast<SwitchNVJPGMapping *>(switch_gpu_backing_mapping_context(backing, SWITCH_GPU_ENGINE_NVJPG));
    for (auto *mapping = head; mapping; mapping = mapping->next) {
        if (mapping->count == extent)
            return mapping;
    }
    if (!extent || extent > kTegraNVJPGOutputBytes / SWITCH_HW_PAGE_SIZE - kSinkPages)
        return nullptr;
    switch_gpu_backing_info info = {};
    if (head) {
        info.pages = head->pages;
        info.page_count = head->available;
    } else {
        switch_gpu_backing_describe(backing, &info);
    }
    if (extent > info.page_count)
        return nullptr;
    const uint32_t count = extent + kSinkPages;
    auto *mapping = IONewZero(SwitchNVJPGMapping, 1);
    uint64_t *pages = IONewData(uint64_t, count);
    if (!mapping || !pages) {
        if (mapping)
            IODelete(mapping, SwitchNVJPGMapping, 1);
        if (pages)
            IODeleteData(pages, uint64_t, count);
        return nullptr;
    }
    memcpy(pages, info.pages, static_cast<uint64_t>(extent) * sizeof(*pages));
    for (uint32_t i = extent; i < count; ++i)
        pages[i] = gSink;
    tegra_nvjpg_lock();
    mapping->address = tegra_nvjpg_pages_map(TEGRA_NVJPG_WINDOW_OUT, pages, count, true);
    tegra_nvjpg_unlock();
    IODeleteData(pages, uint64_t, count);
    mapping->pages = info.pages;
    mapping->count = extent;
    mapping->mapped = count;
    mapping->available = info.page_count;
    if (!mapping->address ||
        (!head && switch_gpu_backing_mapping_install(backing, SWITCH_GPU_ENGINE_NVJPG, info.allocation_generation,
                                                     mapping, destroy))) {
        destroy(mapping);
        return nullptr;
    }
    if (head) {
        mapping->next = head->next;
        head->next = mapping;
    }
#if TEGRA_NVJPG_STATS
    if (pid)
        switch_nvjpg_count_mapping(pid);
#else
    (void)pid;
#endif
    return mapping;
}
