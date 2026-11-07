// SPDX-License-Identifier: BSD-2-Clause-Patent
// The kernel half of the copy-engine bounce (TegraVICBounce.cpp), for the engine thread's jobs.
#ifndef TEGRA_VIC_BOUNCE_ENGINE_H
#define TEGRA_VIC_BOUNCE_ENGINE_H
#include <IOKit/IOTypes.h>
#include "TegraVICRequest.h"
#include "../TegraGM20B/TegraGM20BCopy.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

class IOMemoryDescriptor;

// One side's scratch: memory kept from job to job and grown when a surface needs more, this job's geometry in it, and
// the copies back still reading it.
struct TegraVICBounce {
    switch_dma_pages scratch;
    TegraVICSurfaceDesc desc;
    uint32_t pages;     // the scratch pages this job maps
    uint32_t inFlight;  // copies back queued and not yet landed
};

// The copy engine's host1x syncpoint (allocated once) and its value after everything queued so far, which
// tegra_vic_bounce_order advances.
IOReturn tegra_vic_bounce_start(void);
uint32_t tegra_vic_bounce_syncpt(void);
uint32_t *tegra_vic_bounce_next(void);
// The scratch for surface `d` at pitches that are multiples of `align`.
IOReturn tegra_vic_bounce_stage(TegraVICBounce *b, const TegraVICSurfaceDesc *d, uint32_t align);
// Every plane of `from` (geometry fd) into `to` (td) on the copy engine, after `wait` (null: none); with `signal` the
// last plane raises the copy engine's syncpoint, and `landed` (null: none) runs once it has. Returns at once; both
// descriptors stay prepared until the copy has landed.
IOReturn tegra_vic_bounce_queue(IOMemoryDescriptor *from, const TegraVICSurfaceDesc *fd, IOMemoryDescriptor *to,
                                const TegraVICSurfaceDesc *td, const tegra_gm20b_ce_wait *wait, bool signal,
                                void (*landed)(void *, int), void *arg);
// A copy back out of `b` was queued (the deadline timer starts), and it landed.
void tegra_vic_bounce_back_queued(TegraVICBounce *b);
void tegra_vic_bounce_back_landed(TegraVICBounce *b);
#endif
