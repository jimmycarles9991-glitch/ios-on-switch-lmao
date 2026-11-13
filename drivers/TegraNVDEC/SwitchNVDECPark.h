// SPDX-License-Identifier: BSD-2-Clause-Patent
// A connection's decoder buffers that an exported picture still holds (the native output's read lease) after the
// decoder moved its surface on to another buffer. They keep their pages, NVDEC address and task mapping, and the next
// recycle of the same size takes back one that nothing holds any more instead of allocating and mapping a replacement.
// A session then cycles a fixed set of buffers, so each is mapped once by NVDEC, the task and every engine whose mapping
// follows the backing. A buffer goes back only when no engine has work recorded on it, since a queued job holds no
// reference of its own: one of another size when a recycle passes it (the stream changed size). A full park parks
// nothing more, and a replaced buffer then goes back as before, kept alive by the export that holds it.
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "TegraNVDEC.h"

struct SwitchNVDECBuffer;

struct SwitchNVDECParked {
    TegraNVDECBlock block;
    IOMemoryMap *map;
};

struct SwitchNVDECPark {
    static constexpr uint32_t kCapacity = 16;
    SwitchNVDECParked parked[kCapacity];
    uint32_t count;
    uint64_t recycled, taken, allocated;  // recycles that needed another buffer; of them, parked ones and new ones
    uint32_t most;                        // the most buffers parked at once
};

// 0 when the next job may write the block (*idle: and no engine has work recorded on it), -16 while something holds it,
// another error otherwise.
typedef int (*SwitchNVDECReusable)(const TegraNVDECBlock *block, bool *idle);

// Swaps `buffer`'s held block and map for a parked pair of its size that nothing holds. False when there is none.
bool switch_nvdec_park_take(SwitchNVDECPark *park, SwitchNVDECBuffer *buffer, SwitchNVDECReusable reusable);
// Keeps the block and map a recycle just replaced with a new allocation, or with a full park gives them back.
void switch_nvdec_park_put(SwitchNVDECPark *park, const TegraNVDECBlock &block, IOMemoryMap *map);
// Unmaps every parked buffer from the task and, with returnMemory (no job can touch them), gives it back.
void switch_nvdec_park_drop(SwitchNVDECPark *park, bool returnMemory);
