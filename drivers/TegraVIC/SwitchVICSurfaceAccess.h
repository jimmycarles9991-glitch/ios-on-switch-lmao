// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "TegraVICSurface.h"
#include "../TegraGM20B/SwitchGpuBacking.h"

struct SwitchVICSurfaceAccess {
    IOMemoryDescriptor *descriptor = nullptr;
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *owned = nullptr;
    switch_gpu_backing_guard guard = {};
    uint64_t offset = 0;
};

struct SwitchVICWriteReservation {
    switch_gpu_backing *backing = nullptr;
    switch_gpu_cpu_lease *lease = nullptr;
};

// held: the backing the caller holds for this surface, used in place of the IOSurface cache's.
int switch_vic_surface_access(const TegraVICSurface *surface, bool write, SwitchVICSurfaceAccess *access,
                              bool avoid = false, switch_gpu_backing *held = nullptr);
void switch_vic_surface_unreserve(SwitchVICSurfaceAccess *access);
void switch_vic_surface_release_access(SwitchVICSurfaceAccess *access);
void switch_vic_surface_reserve_write(const TegraVICSurface *surface, SwitchVICWriteReservation *reservation);
void switch_vic_surface_release_write(SwitchVICWriteReservation *reservation);
