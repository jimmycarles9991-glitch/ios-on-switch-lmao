// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

class IOMemoryDescriptor;
class OSObject;
struct SwitchNativeScanout;

namespace switch_display {
// One of window B's IOVA slots. A GPU surface's (memory): as a DirectSlot, the surface retained, its memory prepared and
// its layout held while mapped. An engine allocation's (native): its pages mapped once, named by mappedAllocation and
// mappedGeneration; the frame it shows is leased through native->lease, and the mapping outlives that lease.
struct PlaneSlot {
    IOMemoryDescriptor *memory;
    OSObject *surface;
    void *layout;
    uint64_t low, high;  // the surface bytes mapped (page-aligned) at the slot's IOVA
    uint32_t used;
    bool tiled;  // block-linear: window B scans it so
    SwitchNativeScanout *native = nullptr;
    uint64_t mappedAllocation = 0, mappedGeneration = 0;
};
}  // namespace switch_display
