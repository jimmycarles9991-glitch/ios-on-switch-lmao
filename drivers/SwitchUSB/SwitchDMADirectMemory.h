// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDMADirect.h on IOMemoryDescriptor (kernel). Bound for kits/SwitchDMA.
//
// Only IOGeneralMemoryDescriptors of virtual type qualify: their _kIOMemorySourceSegment walk returns the ranges they
// were made from without touching the VM. Nothing here calls what needs a descriptor wired (getPhysicalSegment without
// _kIOMemorySourceSegment, readBytes, writeBytes, performOperation): IOGMD's segment walk panics on an unwired one.
// zone_element_size() is nonzero only for an address in the zone map, whose every submap is carved from one range
// (xnu-10063 zalloc.c zone_init), so a span whose first and last words are in it lies in it.
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "SwitchDMADirect.h"

struct zone;
extern "C" vm_size_t zone_element_size(void *addr, struct zone **z, bool clear_oob, vm_offset_t *oob_offs);
extern "C" uint64_t kvtophys(vm_offset_t va);

inline bool switch_dma_direct_qualifies(IOMemoryDescriptor *md)
{
    return md && OSDynamicCast(IOGeneralMemoryDescriptor, md) &&
           (md->getFlags() & kIOMemoryTypeMask) == kIOMemoryTypeVirtual64;
}

inline auto switch_dma_direct_spans(IOMemoryDescriptor *md)
{
    return [md](uint32_t offset, switch_dma_span *span) {
        IOByteCount length = 0;
        span->va = md->getPhysicalSegment(offset, &length, _kIOMemorySourceSegment);
        span->length = length;
        return span->va != 0;
    };
}

inline bool switch_dma_zone_memory(uint64_t va, uint64_t length)
{
    const uint64_t last = (va + length - 1) & ~(uint64_t)(sizeof(void *) - 1);
    return zone_element_size(reinterpret_cast<void *>(va), nullptr, false, nullptr) &&
           zone_element_size(reinterpret_cast<void *>(last), nullptr, false, nullptr);
}

// The descriptor's runs (switch_dma_direct_runs); 0 when it needs prepare().
template <class Run>
inline uint32_t switch_dma_direct_map(IOMemoryDescriptor *md, uint32_t length, Run *runs, uint32_t maxRuns)
{
    if (!length || !switch_dma_direct_qualifies(md))
        return 0;
    return switch_dma_direct_runs(
        switch_dma_direct_spans(md), &switch_dma_zone_memory,
        [](uint64_t va) {
            return kvtophys((vm_offset_t)va);
        },
        length, runs, maxRuns);
}

// Request bytes into (toBuffer) or out of a driver buffer, as readBytes/writeBytes move them.
inline void switch_dma_direct_copy(IOMemoryDescriptor *md, uint32_t offset, uint8_t *buffer, uint32_t length,
                                   bool toBuffer)
{
    (void)switch_dma_for_each_span(switch_dma_direct_spans(md), offset, length,
                                   [&buffer, toBuffer](uint64_t va, uint32_t take) {
                                       void *client = reinterpret_cast<void *>(va);
                                       if (toBuffer)
                                           memcpy(buffer, client, take);
                                       else
                                           memcpy(client, buffer, take);
                                       buffer += take;
                                   });
}

// performOperation's cache maintenance by virtual address, one barrier at the end: clean (kIOMemoryIncoherentIOStore)
// or clean and invalidate (kIOMemoryIncoherentIOFlush). The data cache is physically tagged, so these are the lines the
// physical walk maintains.
inline void switch_dma_direct_sync(IOMemoryDescriptor *md, uint32_t offset, uint32_t length, bool invalidate)
{
    const switch_dcache_op op = invalidate ? SWITCH_DCACHE_CLEAN_INVALIDATE : SWITCH_DCACHE_CLEAN;
    (void)switch_dma_for_each_span(switch_dma_direct_spans(md), offset, length, [op](uint64_t va, uint32_t take) {
        switch_dcache_lines(op, reinterpret_cast<const volatile void *>(static_cast<uintptr_t>(va)), take);
    });
    switch_dcache_sync();
}
