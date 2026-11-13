// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraVICBounce.h"
#include "SwitchVICMapping.h"
class IOMemoryDescriptor;

struct SwitchVICBounceBuffer {
    IOMemoryDescriptor *memory;
    switch_gpu_backing *backing;
    SwitchVICMapping *mapping;
    TegraVICSurfaceDesc desc;
    uint64_t capacity;
};

struct SwitchVICBounceSlot {
    SwitchVICBounceBuffer buffers[2];
    bool busy;
};

int switch_vic_bounce_acquire(const TegraVICSurfaceDesc *source, const TegraVICSurfaceDesc *destination,
                              SwitchVICBounceSlot **slot);
void switch_vic_bounce_release(SwitchVICBounceSlot *slot);
