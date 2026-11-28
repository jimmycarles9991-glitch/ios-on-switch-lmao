// SPDX-License-Identifier: BSD-2-Clause-Patent
// Development builds: what the host DMA mapper (UsbDmaMapper.cpp) was asked to do and what the controller's buffers
// hold, to tell from one boot where an IN transfer's data went stale.
//
// Every map and unmap is counted by path (own address, window, bounced) and by descriptor class, with the cache modes
// each class arrived in, and every mapping is remembered until its unmap. A summary prints those counts, the mappings
// still held, the live instances of the USB host classes, and how many held mappings must be completed per transfer
// whenever the port leaves or takes the host role, when the controller powers
// off, once the first lines are used up and every so often after.
//
// The first device-writable mappings also get a line at prepare (path, class, physical address, line offset, length
// and capacity, options, direction, cache mode, CPU, maintenance address) and one at complete, after the unmap's
// maintenance: the first bytes read through the cache then (what the client reads; for a bounce, the pool page the
// controller wrote) and, for the window, the slot's page table entry and the MC's XUSB_HOST ASID and
// TRANSLATION_ENABLE_2. Nothing reads a mapping's data before its unmap's maintenance has run: a CPU access to a page
// between the controller's write and the invalidate is the pattern the maintenance exists to rule out, so the probe
// never makes one. The summary's MC errors are TegraMCErrors.cpp's (tegra_mc_error_latest), never read from the MC
// here.
//
#pragma once
#include <IOKit/IOMemoryDescriptor.h>
#include <stdint.h>

#if SWITCH_STATS_IO
// After a successful map, whichever path took it.
void usb_dma_probe_map(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, uint32_t mapOptions,
                       uint64_t physical, uint64_t address, const char *path);
// Every iovmUnmapMemory call, before it takes any path.
void usb_dma_probe_unmap_call(const IOMemoryDescriptor *memory, uint64_t address);
// The window slot's first page table entry, read before the unmap clears it (0 for any other address).
uint32_t usb_dma_probe_window_entry(uint64_t address);
// After the unmap's maintenance. bounce: the pool page's bytes, or nullptr to read the descriptor at offset.
void usb_dma_probe_unmap(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, uint64_t address, uint64_t pa,
                         const char *path, const uint8_t *bounce, uint32_t entry);
void usb_dma_probe_physical_after(IOMemoryDescriptor *memory, uint64_t pa, uint64_t length);
void usb_dma_probe_summary(const char *when);
#else
inline void usb_dma_probe_map(IOMemoryDescriptor *, uint64_t, uint64_t, uint32_t, uint64_t, uint64_t, const char *)
{}

inline void usb_dma_probe_unmap_call(const IOMemoryDescriptor *, uint64_t)
{}

inline uint32_t usb_dma_probe_window_entry(uint64_t)
{
    return 0;
}

inline void usb_dma_probe_unmap(IOMemoryDescriptor *, uint64_t, uint64_t, uint64_t, uint64_t, const char *,
                                const uint8_t *, uint32_t)
{}

inline void usb_dma_probe_physical_after(IOMemoryDescriptor *, uint64_t, uint64_t)
{}

inline void usb_dma_probe_summary(const char *)
{}
#endif
