// SPDX-License-Identifier: BSD-2-Clause-Patent
// Scattered DMA memory for a master behind an SMMU: a wired, cacheable kernel buffer of any RAM pages, and the 4 KiB
// hardware page list the SMMU maps (also the list of any prepared descriptor, an IOSurface's say).
#ifndef SWITCH_DMA_PAGES_H
#define SWITCH_DMA_PAGES_H

#include <stddef.h>
#include <stdint.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

struct switch_dma_pages {
    IOBufferMemoryDescriptor *memory;  // what a user client maps
    uint8_t *va;
    uint64_t *pages;  // `count` hardware pages, in order
    uint32_t count;
    size_t size;  // whole hardware pages
};

// The hardware pages behind [offset, offset + bytes) of a prepared descriptor that starts on a page boundary, from the
// page holding `offset`; a segment shorter than a page (the descriptor's tail) counts as its whole page. False when a
// segment is not page aligned or more than `max` pages are needed.
bool switch_dma_page_list(IOMemoryDescriptor *memory, uint64_t offset, uint64_t bytes, uint64_t *pages, uint32_t max,
                          uint32_t *count);

// Zeroed and cleaned+invalidated on return, as switch_dma_alloc. extraOptions adds IOKit options
// (kIOMemoryKernelUserShared for a buffer a client maps). Thread context.
bool switch_dma_pages_alloc(switch_dma_pages *out, size_t size, IOOptionBits extraOptions = 0);
void switch_dma_pages_free(switch_dma_pages *buffer);

#endif
