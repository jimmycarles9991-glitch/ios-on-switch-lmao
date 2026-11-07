// SPDX-License-Identifier: BSD-2-Clause-Patent
// One host1x engine's translated address space: its own ASID and directory, every table of an IOVA window installed up
// front (so mapping never allocates), the engine's kernel buffers identity-mapped (IOVA = PA, the falcon fetches its
// firmware and the driver points methods at them by physical address), and buffers mapped page by page at IOVAs from
// TegraSMMUSpace. The window sits below 2 GiB, where no RAM is, so it never meets an identity map.
//
// Each map and unmap cleans its table entries and flushes the SMMU's caches for the ASID; the flush includes a PTC
// flush of every ASID, so buffers are mapped once (when allocated) and unmapped once (after the engine is done with them),
// never per job.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOLocks.h>
#include "TegraSMMUDomain.h"
#include "TegraSMMUSpace.h"
#include "../../kits/SwitchDMA/SwitchDMABuffer.h"

struct TegraSMMUSwgroup {
    uint32_t asidReg, translationReg, translationBits;
};

struct TegraSMMUEngine {
    static constexpr uint64_t kPageLimit = 1ull << 34;  // a table entry's 22-bit page frame

    uint32_t asid = 0;
    TegraSMMUDomain domain;
    TegraSMMUSpace space;
    switch_dma_buffer tables = {};
    uint32_t *bitmap = nullptr, bitmapWords = 0;
    IOLock *lock = nullptr;
    const TegraSMMUSwgroup *groups = nullptr;
    uint32_t groupCount = 0;

    // The window [windowBase, windowBase + windowBytes) (4 MiB aligned, below 2 GiB) and room for identityTables more
    // tables. False, with nothing kept, when memory is short or the window is not one.
    bool init(uint32_t engineAsid, uint64_t windowBase, uint64_t windowBytes, uint32_t identityTables);
    void release();
    // Before attach: [pa, pa + bytes) at IOVA pa.
    bool identity(uint64_t pa, uint64_t bytes, bool writable);
    // Every swgroup of the engine to this directory (they stay listed for detach and resume). 0, or -1 with one line.
    int attach(const TegraSMMUSwgroup *engineGroups, uint32_t count, char *line, size_t size);
    void detach();
    // count hardware pages at the lowest free IOVA; 0 when the window has no room or a page is out of the SMMU's reach.
    uint64_t map(const uint64_t *pages, uint32_t count, bool writable);
    // A range map() returned; only once the engine can no longer touch it.
    void unmap(uint64_t iova, uint32_t count);
    void usage(uint64_t *total, uint64_t *free);
};
