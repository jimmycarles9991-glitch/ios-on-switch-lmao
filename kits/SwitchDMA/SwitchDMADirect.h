// SPDX-License-Identifier: BSD-2-Clause-Patent
// A DMA request's memory without IOMemoryDescriptor::prepare() where it is wired for as long as it exists. No IOKit:
// drivers/SwitchUSB/test/switch_dma_direct_test.cpp drives the same code on the host; SwitchDMADirectMemory.h binds it
// to IOMemoryDescriptor in the kernel.
//
// prepare() on a kernel-virtual descriptor (IOMemoryDescriptor::withAddressRanges over mbufs, as AppleUSBDeviceMux
// queues them) creates a UPL per range through vm_map_create_upl and commits it in complete(): page lookups and wiring
// under the VM object and page-queue locks, for memory that is already wired. Here the runs come from the page tables
// (one translation per 4 KiB); they are the maximal physically contiguous runs in request order, which is what the
// prepared descriptor's getPhysicalSegment walk gives.
#pragma once
#include <stdint.h>
#include "SwitchDcache.h"

constexpr uint32_t kSwitchDmaTranslateBytes = 4096;

struct switch_dma_run {
    uint64_t pa;
    uint32_t length;
};

// How a request's memory is reached:
// - PAGE_TABLES: kernel virtual ranges whose every span is zone memory (mbufs and their clusters, kalloc buffers) and
//   every 4 KiB of which translates; its runs from the page tables, cache maintenance and copies by virtual address.
// - SEGMENTS: a descriptor wired for its whole life (a non-pageable IOBufferMemoryDescriptor: IOKit prepares it when it
//   is made and completes it when it is freed), or no bytes at all; its runs, cache maintenance and copies through the
//   descriptor, which needs no prepare() of ours for them.
// - PREPARE: anything else (user memory, pageable buffers, kernel memory outside the zone map, sub- and multi-range
//   descriptors, a page with no translation, more runs than the caller holds).
enum switch_dma_access : uint8_t {
    SWITCH_DMA_ACCESS_PREPARE,
    SWITCH_DMA_ACCESS_PAGE_TABLES,
    SWITCH_DMA_ACCESS_SEGMENTS,
};

// The runs (any struct with .pa and .length) of a request of `total` bytes whose memory `next(offset, &span)` describes
// (false: no span there), when every span is wired zone memory (`wired(va, length)`) and every 4 KiB of it translates
// (`toPhys(va)`, 0: no mapping). Returns the run count, or 0 when the request does not qualify (a span that is not zone
// memory, a page with no translation, more than `maxRuns` runs, an empty request).
template <class Run, class Next, class Wired, class ToPhys>
inline uint32_t switch_dma_direct_runs(Next next, Wired wired, ToPhys toPhys, uint32_t total, Run *runs,
                                       uint32_t maxRuns)
{
    uint32_t count = 0;
    for (uint32_t offset = 0; offset < total;) {
        switch_dma_span span = {0, 0};
        if (!next(offset, &span) || !span.length)
            return 0;
        const uint32_t take = span.length < total - offset ? static_cast<uint32_t>(span.length) : total - offset;
        if (!wired(span.va, take))
            return 0;
        for (uint32_t done = 0; done < take;) {
            const uint64_t va = span.va + done;
            const uint32_t inPage =
                kSwitchDmaTranslateBytes - static_cast<uint32_t>(va & (kSwitchDmaTranslateBytes - 1));
            const uint32_t piece = inPage < take - done ? inPage : take - done;
            const uint64_t pa = toPhys(va);
            if (!pa)
                return 0;
            if (count && runs[count - 1].pa + runs[count - 1].length == pa) {
                runs[count - 1].length += piece;
            } else {
                if (count == maxRuns)
                    return 0;
                runs[count].pa = pa;
                runs[count].length = piece;
                ++count;
            }
            done += piece;
        }
        offset += take;
    }
    return count;
}

// The path for a request of `total` bytes: virtualRanges, its descriptor is kernel virtual ranges the source walk
// `next` can describe; wiredBuffer, it is wired for its life. PAGE_TABLES fills `runs` (*runCount of them).
template <class Run, class Next, class Wired, class ToPhys>
inline switch_dma_access switch_dma_direct_access(bool virtualRanges, bool wiredBuffer, Next next, Wired wired,
                                                  ToPhys toPhys, uint32_t total, Run *runs, uint32_t maxRuns,
                                                  uint32_t *runCount)
{
    *runCount = 0;
    if (!total)
        return SWITCH_DMA_ACCESS_SEGMENTS;
    if (virtualRanges)
        *runCount = switch_dma_direct_runs(next, wired, toPhys, total, runs, maxRuns);
    if (*runCount)
        return SWITCH_DMA_ACCESS_PAGE_TABLES;
    return wiredBuffer ? SWITCH_DMA_ACCESS_SEGMENTS : SWITCH_DMA_ACCESS_PREPARE;
}

// fn(va, length) over the virtual ranges of request bytes [offset, offset + length), in order. False when `next` has
// no span where one is needed.
template <class Next, class Fn> inline bool switch_dma_for_each_span(Next next, uint32_t offset, uint32_t length, Fn fn)
{
    for (const uint32_t end = offset + length; offset < end;) {
        switch_dma_span span = {0, 0};
        if (!next(offset, &span) || !span.length)
            return false;
        const uint32_t take = span.length < end - offset ? static_cast<uint32_t>(span.length) : end - offset;
        fn(span.va, take);
        offset += take;
    }
    return true;
}
