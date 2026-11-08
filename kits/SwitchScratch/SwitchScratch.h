// SPDX-License-Identifier: BSD-2-Clause-Patent
// Recycled, wired scratch memory for staging a surface's bytes between a hardware engine and the CPU (the GPU driver's
// CPU views of block-linear surfaces). A scratch is cacheable pageable memory of its own VM object, wired for its life,
// with its 4 KiB pages listed for the engine, and charged as graphics memory (XNU's ledger, as IOSurface charges its
// own) to the task it is handed to, the kernel's while kept. A scratch given back is kept, volatile (out of every
// footprint), for the next of a size it fits (a lock per frame reuses one instead of wiring 4 MiB each time) until it
// has been idle kSwitchScratchIdleNs, or the kept ones pass kSwitchScratchKeepBytes.
#ifndef SWITCH_SCRATCH_H
#define SWITCH_SCRATCH_H

#include <stdint.h>
#include <mach/mach_types.h>

class IOBufferMemoryDescriptor;

constexpr uint64_t kSwitchScratchIdleNs = 10ULL * 1000 * 1000 * 1000;
constexpr uint64_t kSwitchScratchKeepBytes = 32ULL << 20;

struct switch_scratch {
    IOBufferMemoryDescriptor *memory;
    uint64_t *pas;  // one per 4 KiB page
    uint32_t npages;
    uint64_t bytes;
    uint64_t idleSince;  // ns, while kept
    switch_scratch *next;
};

// At least `bytes` (whole 64 KiB), charged to `owner`: a kept one that fits and is no more than twice as large, else a
// new one. Thread context.
switch_scratch *switch_scratch_get(uint64_t bytes, task_t owner);
void switch_scratch_put(switch_scratch *scratch);
// Releases a scratch that may still be mapped somewhere (its VM object then lives on in that mapping) instead of keeping it
void switch_scratch_discard(switch_scratch *scratch);
// Bytes handed out now, and the most ever (debug lines)
void switch_scratch_stats(uint64_t *held, uint64_t *peak);

#endif
