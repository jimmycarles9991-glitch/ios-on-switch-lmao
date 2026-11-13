// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "SwitchNVDECPark.h"
#include "SwitchNVDECOutput.h"

namespace {
void giveBack(SwitchNVDECParked *entry, bool returnMemory)
{
    if (entry->map) {
        entry->map->unmap();
        entry->map->release();
    }
    if (returnMemory)
        tegra_nvdec_buffer_free(&entry->block);
    *entry = {};
}

void unpark(SwitchNVDECPark *park, uint32_t index)
{
    for (uint32_t i = index + 1; i < park->count; ++i)
        park->parked[i - 1] = park->parked[i];
    park->parked[--park->count] = {};
}
}  // namespace

bool switch_nvdec_park_take(SwitchNVDECPark *park, SwitchNVDECBuffer *buffer, SwitchNVDECReusable reusable)
{
    ++park->recycled;
    for (uint32_t i = 0; i < park->count;) {
        SwitchNVDECParked &entry = park->parked[i];
        const bool sameSize = entry.block.size == buffer->block.size;
        bool idle = false;
        const bool writable = reusable(&entry.block, &idle) == 0;
        if (writable && sameSize) {
            const SwitchNVDECParked spare = entry;
            entry = {buffer->block, buffer->map};
            buffer->block = spare.block;
            buffer->map = spare.map;
            ++park->taken;
            return true;
        }
        if (writable && idle && !sameSize) {
            giveBack(&entry, true);
            unpark(park, i);
            continue;
        }
        ++i;
    }
    return false;
}

void switch_nvdec_park_put(SwitchNVDECPark *park, const TegraNVDECBlock &block, IOMemoryMap *map)
{
    ++park->allocated;
    if (park->count == SwitchNVDECPark::kCapacity) {
        SwitchNVDECParked replaced = {block, map};
        replaced.map->release();
        tegra_nvdec_buffer_free(&replaced.block);
        return;
    }
    park->parked[park->count++] = {block, map};
    if (park->count > park->most)
        park->most = park->count;
}

void switch_nvdec_park_drop(SwitchNVDECPark *park, bool returnMemory)
{
    for (uint32_t i = 0; i < park->count; ++i)
        giveBack(&park->parked[i], returnMemory);
    park->count = 0;
}
