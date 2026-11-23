// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

struct SwitchNVJPGMapping {
    uint64_t address;
    const uint64_t *pages;
    uint32_t count, mapped, available;
    SwitchNVJPGMapping *next;
};

// A destination backing as NVJPG maps it: its pages, which its first NVJPG mapping fixes (the GPU driver then refuses
// to grow it), and the mappings it holds.
struct SwitchNVJPGBacking {
    switch_gpu_backing *backing;
    SwitchNVJPGMapping *head;
    const uint64_t *pages;
    uint32_t count;
    uint64_t generation;
};

SwitchNVJPGBacking switch_nvjpg_backing(switch_gpu_backing *backing);
// The backing's first `extent` pages, mapped writable in the output window for a decode that writes [0, writeEnd): the
// pages, then the sink for what runs past them. Engine thread; the caller holds the backing. nullptr with `why` a
// literal (the output window full, say) when there is none.
SwitchNVJPGMapping *switch_nvjpg_mapping(const SwitchNVJPGBacking &target, uint32_t extent, uint64_t writeEnd,
                                         const char **why, int pid = 0);

// A queue slot's scan buffer: grown to the longest scan the slot has held and kept for the boot, mapped read-only in
// the input window with the sink page after it (the engine may read a little past the scan's end).
struct SwitchNVJPGScan {
    switch_dma_pages buffer;
    uint64_t address;
};

// The slot of `freeSlots` (bit n: scans[n]; not 0) a scan of `bytes` takes: the shortest that holds it, else the
// longest.
uint32_t switch_nvjpg_scan_pick(const SwitchNVJPGScan *scans, uint32_t freeSlots, uint64_t bytes);
// The caller's slot holding at least `bytes`, grown when shorter: its old buffer unmapped, then freed. 0, -12 (no
// memory) or -28 (no room in the input window), the slot then empty.
int switch_nvjpg_scan_fit(SwitchNVJPGScan *scan, uint64_t bytes);
