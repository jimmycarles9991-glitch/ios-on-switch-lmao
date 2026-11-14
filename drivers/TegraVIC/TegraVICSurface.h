// SPDX-License-Identifier: BSD-2-Clause-Patent
// IOSurfaces for the scaler (TegraVICSurface.cpp): lookup by ID in the caller's task, geometry, wiring.
#ifndef TEGRA_VIC_SURFACE_H
#define TEGRA_VIC_SURFACE_H
#include <IOKit/IOTypes.h>
#include <kern/task.h>
#include "TegraVICRequest.h"

class OSObject;
class IOMemoryDescriptor;
struct switch_gpu_backing;

struct TegraVICSurface {
    OSObject *surface;           // retained IOSurface
    IOMemoryDescriptor *memory;  // the surface's (not retained: the surface holds it)
    TegraVICSurfaceDesc desc;
    uint64_t baseOffset;
    uint32_t id;
    bool prepared;
};

extern "C" {
bool tegra_vic_surface_start(void);  // finds the IOSurfaceRoot (V3)
IOReturn tegra_vic_surface_lookup(uint32_t id, task_t task, TegraVICSurface *out);
// A retained kernel object needs no numeric ID; user requests still go through task-authorized lookup.
IOReturn switch_vic_surface_from_object(OSObject *surface, uint32_t id, TegraVICSurface *out);
uint32_t switch_vic_surface_matrix(OSObject *surface);
void switch_vic_surface_color(OSObject *surface, TegraVICSurfaceDesc *desc);
void tegra_vic_surface_release(TegraVICSurface *s);
IOReturn tegra_vic_surface_prepare(TegraVICSurface *s, uint64_t extent, uint64_t *pages, uint32_t max, uint32_t *count);
IOReturn tegra_vic_surface_pages(TegraVICSurface *s, uint64_t offset, uint64_t bytes, uint64_t *pages, uint32_t max,
                                 uint32_t *count);
void tegra_vic_surface_complete(TegraVICSurface *s);
void tegra_vic_surface_written(const TegraVICSurface *s);  // VIC wrote it: the seed moves (r18-planes)
}

switch_gpu_backing *switch_vic_surface_backing(const TegraVICSurface *surface);
// A backing the caller already holds for this surface (a display slot's, held as scanned since the slot took it):
// retained and adopted again, without the cache's lookup or its lock.
switch_gpu_backing *switch_vic_surface_backing_held(const TegraVICSurface *surface, switch_gpu_backing *held);
#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
// Debug kernels: the cache lock's takes since boot.
extern "C" uint64_t switch_vic_surface_cache_locks(void);
#endif
#endif
