// SPDX-License-Identifier: BSD-2-Clause-Patent
// A surface's native publication (SwitchNativeSurface.h), as SwitchNativeSurface.cpp and the pool that recycles it
// (SwitchNativeSurfacePool.cpp) share it.
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "SwitchNativeSurface.h"
#include "TegraVICConfig.h"

class SwitchNativeSurface : public OSObject {
OSDeclareDefaultStructors(SwitchNativeSurface) public : bool initialize()
    {
        return init();
    }

    switch_gpu_backing *native = nullptr;
    switch_gpu_cpu_lease *readLease = nullptr;
    switch_gpu_backing_token producer = {};
    TegraVICPicture picture = {};
    TegraVICSurfaceDesc pitch = {};
    switch_gpu_backing_layout layout = {};
    SwitchNativeSurfaceCrop crop = {};
    SwitchNativeSurfaceTurn turn = {};
    TegraVICOp turnOp = {};
    uint32_t chromaSite = 0;
    SwitchVICEvents *events = nullptr;  // a recorded turn's request events
    uint32_t delivered = 0;             // its signals went (a reader's VIC job, the display, or at once)
    // generation equals live while the object is a surface's publication. A reused object's live is one past its last
    // generation, so a view kept from an earlier publication of it stays stale.
    alignas(8) volatile uint64_t generation = 1;
    uint64_t live = 1;
    bool pitchValid = false;
    bool preparing = false;
    void free() override;
};

// One lock for every surface's `preparing` flag and the retired ring, each surface its own sleep event: a publication
// per decoded picture allocates no lock. Made by the first caller.
IOLock *switch_native_surface_prepare_lock();

// A retired publication no view holds any more, emptied and live again under a new generation; null when there is none.
SwitchNativeSurface *switch_native_surface_take_retired();
// `old` was replaced and invalidated: its read lease is gone, and nothing reads its decoder backing through it again.
void switch_native_surface_retire(SwitchNativeSurface *old);
// The object's publication fields emptied, and live again one past its last generation, by its one owner: the caller
// holds it alone, or holds its surface exclusively with its `preparing` claimed.
void switch_native_surface_empty(SwitchNativeSurface *object);
// Debug kernels: a publication made, whether it needed a new object, and whether its surface had no publication before.
void switch_native_surface_count(bool allocated, bool firstIntoSurface);
