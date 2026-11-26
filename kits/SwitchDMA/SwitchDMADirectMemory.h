// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDMADirect.h on IOMemoryDescriptor (kernel).
//
// Only IOGeneralMemoryDescriptors of virtual type walk their ranges: their _kIOMemorySourceSegment walk returns the
// ranges they were made from without touching the VM. Nothing on the PAGE_TABLES path calls what needs a descriptor
// wired (getPhysicalSegment without _kIOMemorySourceSegment, readBytes, writeBytes, performOperation): IOGMD's segment
// walk panics on an unwired one. zone_element_size() is nonzero only for an address in the zone map, whose every submap
// is carved from one range (xnu-10063 zalloc.c zone_init), so a span whose first and last words are in it lies in it.
// A user task's addresses are never in it. A non-pageable IOBufferMemoryDescriptor is kernel memory (IOBMD refuses a
// non-kernel task without kIOMemoryPageable) made with kIOMemoryAutoPrepare: prepared by its init, completed by its
// free, so its own segments, copies and cache operations are valid for as long as the request holds it.
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "SwitchDcache.h"
#include "SwitchDMADirect.h"

struct zone;
extern "C" vm_size_t zone_element_size(void *addr, struct zone **z, bool clear_oob, vm_offset_t *oob_offs);
extern "C" uint64_t kvtophys(vm_offset_t va);

inline bool switch_dma_direct_qualifies(IOMemoryDescriptor *md)
{
    return md != nullptr && OSDynamicCast(IOGeneralMemoryDescriptor, md) != nullptr &&
           (md->getFlags() & kIOMemoryTypeMask) == kIOMemoryTypeVirtual64;
}

inline bool switch_dma_wired_buffer(IOMemoryDescriptor *md)
{
    return md != nullptr && OSDynamicCast(IOBufferMemoryDescriptor, md) != nullptr &&
           (md->getFlags() & kIOMemoryBufferPageable) == 0;
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
    const uint64_t last = (va + length - 1) & ~static_cast<uint64_t>(sizeof(void *) - 1);
    return zone_element_size(reinterpret_cast<void *>(va), nullptr, false, nullptr) != 0 &&
           zone_element_size(reinterpret_cast<void *>(last), nullptr, false, nullptr) != 0;
}

// How the descriptor's [0, length) is reached (switch_dma_direct_access); its runs when through the page tables.
template <class Run>
inline switch_dma_access switch_dma_direct_access(IOMemoryDescriptor *md, uint32_t length, Run *runs, uint32_t maxRuns,
                                                  uint32_t *runCount)
{
    return switch_dma_direct_access(
        switch_dma_direct_qualifies(md), switch_dma_wired_buffer(md), switch_dma_direct_spans(md),
        &switch_dma_zone_memory,
        [](uint64_t va) {
            return kvtophys(static_cast<vm_offset_t>(va));
        },
        length, runs, maxRuns, runCount);
}

// Request bytes into (toBuffer) or out of a driver buffer, as readBytes/writeBytes move them.
inline void switch_dma_direct_copy(IOMemoryDescriptor *md, uint32_t offset, uint8_t *buffer, uint32_t length,
                                   bool toBuffer)
{
    (void)switch_dma_for_each_span(switch_dma_direct_spans(md), offset, length,
                                   [&buffer, toBuffer](uint64_t va, uint32_t take) {
                                       void *client = reinterpret_cast<void *>(va);
                                       if (toBuffer) {
                                           memcpy(buffer, client, take);
                                       } else {
                                           memcpy(client, buffer, take);
                                       }
                                       buffer += take;
                                   });
}

// Cache maintenance over request bytes [offset, offset + length), one barrier at the end. The data cache is physically
// tagged, so by virtual address (the PAGE_TABLES path) and through the physical aperture (switch_dma_segments_sync) are
// the same lines.
inline void switch_dma_direct_sync(IOMemoryDescriptor *md, uint32_t offset, uint32_t length, switch_dcache_op op)
{
    (void)switch_dma_for_each_span(switch_dma_direct_spans(md), offset, length, [op](uint64_t va, uint32_t take) {
        switch_dcache_lines(op, reinterpret_cast<const volatile void *>(static_cast<uintptr_t>(va)), take);
    });
    switch_dcache_sync();
}

// The same over the physical segments of a descriptor that is prepared or wired for its life. A segment outside
// managed RAM has no cacheable alias to maintain.
inline void switch_dma_segments_sync(IOMemoryDescriptor *md, uint32_t offset, uint32_t length, switch_dcache_op op)
{
    for (const uint32_t end = offset + length; offset < end;) {
        IOByteCount segment = 0;
        const addr64_t pa = md->getPhysicalSegment(offset, &segment, kIOMemoryMapperNone);
        if (!pa || !segment)
            break;
        const uint32_t take = segment < end - offset ? static_cast<uint32_t>(segment) : end - offset;
        if (switch_dma_pa_managed(pa))
            switch_dcache_phys_lines(op, pa, take);
        offset += take;
    }
    switch_dcache_sync();
}
