// SPDX-License-Identifier: BSD-2-Clause-Patent
// The copy-engine bounce (TegraVICBounce.h): scratch memory kept across jobs, and the copies into and out of it, queued
// without waiting. The copy engine and VIC order each other on the hardware through host1x syncpoints: VIC's channel
// waits for the fills, the copy back waits for VIC's fence. The engine thread queues and moves on; a copy back finishes
// its job from the copy's completion. A copy back that never completes is a one-line panic from a deadline timer.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <sys/errno.h>
#include "TegraVICBounce.h"
#include "TegraVICBounceEngine.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace {
constexpr uint32_t kLandedWithinMs = 2000;  // a whole 128 MiB window copies in well under 100 ms

IOLock *gLock;  // the copies in flight and the watchdog
uint32_t gSyncpt, gNext;
uint32_t gBackQueued, gBackLanded, gBackSeen;  // copies back queued and completed; completions the watchdog last saw
thread_call_t gWatchdog;

void watchLocked()
{
    uint64_t deadline = 0;
    clock_interval_to_deadline(kLandedWithinMs, kMillisecondScale, &deadline);
    gBackSeen = gBackLanded;
    thread_call_enter_delayed(gWatchdog, deadline);
}

void watchdog(thread_call_param_t, thread_call_param_t)
{
    IOLockLock(gLock);
    const uint32_t pending = gBackQueued - gBackLanded;
    if (pending && gBackLanded == gBackSeen) {
        panic("TegraVIC: copy-engine bounce lost its completion: %u copies back outstanding after %u ms (syncpoint "
              "%u at %u)",
              pending, kLandedWithinMs, gSyncpt, tegra_host1x_syncpt_read(gSyncpt));
    }
    if (pending)
        watchLocked();
    IOLockUnlock(gLock);
}

IOReturn ioreturnOf(int err)
{
    if (err == -EBUSY)
        return kIOReturnBusy;
    if (err == -ENOMEM)
        return kIOReturnNoMemory;
    return err ? kIOReturnIOError : kIOReturnSuccess;
}
}  // namespace

IOReturn tegra_vic_bounce_start()
{
    if (gLock)
        return kIOReturnSuccess;
    IOLock *lock = IOLockAlloc();
    thread_call_t watch = thread_call_allocate(watchdog, nullptr);
    if (!lock || !watch || tegra_host1x_syncpt_alloc(&gSyncpt)) {
        if (lock)
            IOLockFree(lock);
        if (watch)
            thread_call_free(watch);
        return kIOReturnNoResources;
    }
    gNext = tegra_host1x_syncpt_read(gSyncpt);
    gWatchdog = watch;
    gLock = lock;
    return kIOReturnSuccess;
}

uint32_t tegra_vic_bounce_syncpt()
{
    return gSyncpt;
}

uint32_t *tegra_vic_bounce_next()
{
    return &gNext;
}

IOReturn tegra_vic_bounce_stage(TegraVICBounce *b, const TegraVICSurfaceDesc *d, uint32_t align)
{
    tegra_vic_bounce_desc(d, align, &b->desc);
    if (b->scratch.size < b->desc.length) {
        // The scratch a copy back still reads is freed only once it has landed (a new size is rare: no steady-state
        // job waits here).
        IOLockLock(gLock);
        while (b->inFlight)
            IOLockSleep(gLock, &b->inFlight, THREAD_UNINT);
        IOLockUnlock(gLock);
        switch_dma_pages_free(&b->scratch);
        if (!switch_dma_pages_alloc(&b->scratch, static_cast<size_t>(b->desc.length)))
            return kIOReturnNoMemory;
    }
    b->pages = static_cast<uint32_t>(b->desc.length / TEGRA_VIC_BOUNCE_PAGE);
    return kIOReturnSuccess;
}

IOReturn tegra_vic_bounce_queue(IOMemoryDescriptor *from, const TegraVICSurfaceDesc *fd, IOMemoryDescriptor *to,
                                const TegraVICSurfaceDesc *td, const tegra_gm20b_ce_wait *wait, bool signal,
                                void (*landed)(void *, int), void *arg)
{
    const TegraVICFormat *f = tegra_vic_format(fd->fourcc);
    const tegra_gm20b_ce_done done = {signal ? gSyncpt : TEGRA_GM20B_CE_NO_SYNCPT, landed, arg};
    for (uint32_t p = 0; p < f->planes; p++) {
        // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): a format's planes never outnumber a descriptor's
        const tegra_gm20b_ce_plane src = {from, fd->planeOffset[p], static_cast<uint32_t>(fd->planeBytesPerRow[p])};
        const tegra_gm20b_ce_plane dst = {to, td->planeOffset[p], static_cast<uint32_t>(td->planeBytesPerRow[p])};
        const bool last = p + 1 == f->planes;
        const int err = tegra_gm20b_ce_copy_plane(&src, &dst, tegra_vic_bounce_row_bytes(f, fd->width, p),
                                                  static_cast<uint32_t>(fd->planeHeight[p]), p ? nullptr : wait,
                                                  last && (signal || landed) ? &done : nullptr);
        if (err)
            return ioreturnOf(err);
    }
    return kIOReturnSuccess;
}

void tegra_vic_bounce_back_queued(TegraVICBounce *b)
{
    IOLockLock(gLock);
    b->inFlight++;
    if (gBackQueued++ == gBackLanded)
        watchLocked();
    IOLockUnlock(gLock);
}

void tegra_vic_bounce_back_landed(TegraVICBounce *b)
{
    IOLockLock(gLock);
    gBackLanded++;
    if (--b->inFlight == 0)
        IOLockWakeup(gLock, &b->inFlight, false);
    IOLockUnlock(gLock);
}
