// SPDX-License-Identifier: BSD-2-Clause-Patent
// The development probe's parts (UsbDmaProbe.h): completed buffer reads and the table of mappings still held.
#pragma once

#if DEVELOPMENT || DEBUG
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/cpu_number.h>
#include <stdint.h>
#include <string.h>
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "UsbDmaCache.h"

extern "C" {
uint64_t kvtophys(vm_offset_t va);
}

namespace usb_dma_probe {
inline uint64_t firstBytes(uint64_t length)
{
    return length < 8 ? length : 8;
}

// The aperture's alias stays cacheable whatever the page's own attributes (no __ARM_PTE_PHYSMAP__ without the PPL),
// so only a cacheable descriptor's bytes are read through it.
inline bool cacheable(IOMemoryDescriptor *memory)
{
    return usbDmaCacheable(memory->getFlags());
}

struct Hex {
    char text[17];

    explicit Hex(const uint8_t bytes[8]) : text()
    {
        static const char kDigits[] = "0123456789abcdef";
        for (size_t i = 0; i < 8; ++i) {
            text[2 * i] = kDigits[bytes[i] >> 4];
            text[(2 * i) + 1] = kDigits[bytes[i] & 0xfu];
        }
    }
};

// Where XNU's and our maintenance issue their DC instructions for pa (phystokv_range, as dcache_incoherent_io_*64
// and switch_dcache_phys_lines do), and what the MMU translates that address back to.
struct Aperture {
    uint64_t va = 0, back = 0;

    explicit Aperture(uint64_t pa)
    {
        if (pa == 0u || !switch_dma_pa_managed(pa))
            return;
        size_t chunk = 1;
        va = phystokv_range(pa, &chunk);
        back = kvtophys(static_cast<vm_offset_t>(va));
    }
};

inline void readCached(uint64_t pa, uint64_t length, uint8_t out[8])
{
    if (pa == 0u || !switch_dma_pa_managed(pa))
        return;
    size_t chunk = static_cast<size_t>(firstBytes(length));
    const uint64_t va = phystokv_range(pa, &chunk);
    memcpy(out, reinterpret_cast<const void *>(va), chunk);
}

// UsbDmaProbeHeld.cpp. path: "own address", "window" or "bounced"; pa: the mapping's first byte (0 for a bounce).
void heldMap(IOMemoryDescriptor *memory, uint64_t address, uint64_t pa, const char *path);
void heldUnmap(const IOMemoryDescriptor *memory, uint64_t address);
}  // namespace usb_dma_probe
#endif
