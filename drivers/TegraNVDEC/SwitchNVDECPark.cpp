// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
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

bool grow(SwitchNVDECPark *park)
{
    const uint32_t capacity = park->capacity ? park->capacity * 2 : SwitchNVDECPark::kFirstCapacity;
    auto *grown = IONewZero(SwitchNVDECParked, capacity);
    if (!grown)
        return false;
    for (uint32_t i = 0; i < park->count; ++i)
        grown[i] = park->parked[i];
    if (park->parked)
        IODelete(park->parked, SwitchNVDECParked, park->capacity);
    park->parked = grown;
    park->capacity = capacity;
    return true;
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
    if (park->count == park->capacity && !grow(park)) {
        IOLog("TegraNVDEC: no memory to park more than %u buffers: an exported picture's buffer goes back\n",
              park->count);
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
    if (park->parked)
        IODelete(park->parked, SwitchNVDECParked, park->capacity);
    park->parked = nullptr;
    park->capacity = 0;
}
