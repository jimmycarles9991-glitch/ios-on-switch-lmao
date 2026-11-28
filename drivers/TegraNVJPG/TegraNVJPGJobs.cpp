// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TegraNVJPGJobs.h.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <os/log.h>
#include <sys/proc.h>
#include <string.h>
#include "../TegraVIC/TegraVICSurface.h"
#include "TegraNVJPG.h"
#include "TegraNVJPGMapping.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../../kits/SwitchDMA/SwitchHwPage.h"
#include "../../kits/SwitchMath/SwitchMath.h"
#include "../../kits/SwitchTime/SwitchTime.h"
#include "TegraNVJPGRepack.h"
#include "TegraNVJPGRequest.h"
#include "TegraNVJPGRoute.h"
#include "TegraNVJPGStats.h"
#include "TegraNVJPGJobs.h"
#include "../TegraVIC/SwitchVICDecodeOutput.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

extern "C" void *tegra_gm20b_surface_hold(uint64_t pa, int *tiled) __attribute__((weak));
extern "C" void tegra_gm20b_surface_unhold(void *hold) __attribute__((weak));
extern "C" int tegra_gm20b_surface_layout(uint64_t pa, struct tgm_layout *out) __attribute__((weak));
extern "C" int tegra_gm20b_surface_compressed(uint64_t pa) __attribute__((weak));

extern "C" {
vm_offset_t phystokv(uint64_t pa);
}

namespace tegra_nvjpg {
constexpr uint32_t kQueueMax = 16;  // decodes in flight: each holds a scan slot from its copy to its run

IOLock *gQueueLock;
SwitchNVJPGScan gScans[kQueueMax];
uint32_t gScansFree = (1u << kQueueMax) - 1;  // bit n: gScans[n]; gQueueLock
uint32_t gScanWaiters;                        // gQueueLock
Job gJobPool[kQueueMax];
uint32_t gJobsFree = (1u << kQueueMax) - 1;  // bit n: gJobPool[n]; gQueueLock
bool gEngineBusy;                            // gQueueLock: single engine owner (engineThread or direct sync caller)
bool gEngineSleeping;                        // gQueueLock: engineThread asleep on &gHead

IOReturn ioreturn(int err)
{
    switch (err) {
    case 0:
        return kIOReturnSuccess;
    case -12:
        return kIOReturnNoMemory;
    case -22:
        return kIOReturnBadArgument;
    case -60:
        return kIOReturnTimeout;
    case -19:
        return kIOReturnNotReady;
    default:
        return kIOReturnIOError;
    }
}

uint32_t gFirstLogged;  // bit n: a decode into TEGRA_NVJPG_OUT_n has been logged (engine thread)

void refused(const char *why, uint32_t format)
{
#if TEGRA_NVJPG_PROBES
    tegra_nvjpg_count_refused(proc_selfpid(), format);
#else
    (void)format;
#endif
    switch_refusal("TegraNVJPG", why, "decode refused, the caller decodes on the CPU: %s", why);
}

Job *gHead, *gTail;

Job *allocJob()
{
    IOLockLock(gQueueLock);
    if (gJobsFree) {
        const uint32_t slot = static_cast<uint32_t>(__builtin_ctz(gJobsFree));
        gJobsFree &= ~(1u << slot);
        IOLockUnlock(gQueueLock);
        Job *job = &gJobPool[slot];
        bzero(job, sizeof(*job));
        job->pooled = true;
        return job;
    }
    IOLockUnlock(gQueueLock);
    return IONewZero(Job, 1);
}

SwitchNVJPGScan *takeScan(uint64_t bytes)
{
    IOLockLock(gQueueLock);
    if (!gScansFree) {
        ++gScanWaiters;
        while (!gScansFree)
            IOLockSleep(gQueueLock, &gScansFree, THREAD_UNINT);
        --gScanWaiters;
    }
    const uint32_t slot = switch_nvjpg_scan_pick(gScans, gScansFree, bytes);
    gScansFree &= ~(1u << slot);
    IOLockUnlock(gQueueLock);
    return &gScans[slot];
}

void giveScanLocked(Job *job)
{
    if (!job->scan)
        return;
    gScansFree |= 1u << (job->scan - gScans);
    if (gScanWaiters)
        IOLockWakeup(gQueueLock, &gScansFree, false);
    job->scan = nullptr;
}

void giveScan(Job *job)
{
    if (!job->scan)
        return;
    IOLockLock(gQueueLock);
    giveScanLocked(job);
    IOLockUnlock(gQueueLock);
}

void dropJob(Job *job)
{
    if (job->backing) {
        switch_gpu_backing_release(job->backing);
        job->backing = nullptr;
    }
    tegra_vic_surface_release(&job->dst);
    const bool pooled = job->pooled;
    if (job->scan || pooled) {
        IOLockLock(gQueueLock);
        giveScanLocked(job);
        if (pooled)
            gJobsFree |= 1u << (job - gJobPool);
        IOLockUnlock(gQueueLock);
    }
    if (!pooled)
        IODelete(job, Job, 1);
}

// The CPU's part of a grey picture into '420f': neutral chroma over the picture's rows, written through the physical
// aperture of the destination's wired pages, cleaned to DRAM for readers that do not snoop.
void fillChroma(const TegraNVJPGPlan &p, const uint64_t *addresses, uint32_t pages)
{
    const auto fill = [&] {
        for (uint64_t row = 0; row < p.fillRows; row++) {
            for (uint64_t at = p.fillStart + row * p.fillPitch, end = at + p.fillRowBytes; at < end;) {
                const uint32_t page = static_cast<uint32_t>(at >> SWITCH_HW_PAGE_SHIFT);
                const uint64_t stop = SWITCH_MIN(switch_align_down(at, SWITCH_HW_PAGE_SIZE) + SWITCH_HW_PAGE_SIZE, end);
                if (page >= pages || !switch_dma_pa_managed(addresses[page]))
                    return;  // the plan bounded it
                uint8_t *va = reinterpret_cast<uint8_t *>(phystokv(addresses[page])) + (at & SWITCH_HW_PAGE_MASK);
                memset(va, 0x80, stop - at);
                switch_dcache_clean_lines(va, stop - at);
                at = stop;
            }
        }
    };
    fill();
    switch_dcache_sync();
}

TegraNVJPGSurface surfaceDesc(const TegraVICSurfaceDesc &v)
{
    TegraNVJPGSurface d = {};
    d.fourcc = v.fourcc, d.width = v.width, d.height = v.height, d.planes = v.planes;
    for (uint32_t i = 0; i < 2; i++) {
        d.planeOffset[i] = v.planeOffset[i];
        d.planeBytesPerRow[i] = v.planeBytesPerRow[i];
        d.planeHeight[i] = v.planeHeight[i];
    }
    d.length = v.length;
    d.protection = v.protection;
    return d;
}

// The destination as GM20B laid it out (TegraGM20BTiled.cpp decides once per surface), held until the decode and any
// re-pack have ended so that no import moves its texels meanwhile. The caller has moved the surface's seed first: a GPU
// sampler whose first ask comes after this read then finds a written surface and keeps it pitch, instead of tiling in
// place the rows this decode is writing. NULL with the plan moved onto that layout (TegraNVJPGRoute.h), or why it
// cannot be written.
const char *routeJob(Job *job, const SwitchNVJPGBacking &target, void **hold, uint32_t *route)
{
    *hold = nullptr;
    TegraNVJPGDestinationLayout layout = {};
    if (tegra_gm20b_surface_hold && tegra_gm20b_surface_unhold && tegra_gm20b_surface_layout) {
        int tiled = 0;
        *hold = tegra_gm20b_surface_hold(target.pages[0], &tiled);
        layout.held = tiled != 0;
        layout.described = layout.held && tegra_gm20b_surface_layout(target.pages[0], &layout.layout) == 1;
        layout.compressed =
            layout.described && tegra_gm20b_surface_compressed && tegra_gm20b_surface_compressed(target.pages[0]);
    }
    const TegraNVJPGSurface d = surfaceDesc(job->dst.desc);
#if TEGRA_NVJPG_PROBES
    const char *why =
        job->tileProbe
            ? tegra_nvjpg_route_tile_probe(&job->request.base, job->dst.desc.length, kTegraNVJPGOutputBytes, &layout,
                                           job->tileMode, job->tileGob, &job->plan, &job->engine.setup, route)
            : tegra_nvjpg_route(&job->request.base, &d, kTegraNVJPGOutputBytes, job->dst.memory != nullptr, &layout,
                                &job->plan, &job->engine.setup, route);
#else
    const char *why = tegra_nvjpg_route(&job->request.base, &d, kTegraNVJPGOutputBytes, job->dst.memory != nullptr,
                                        &layout, &job->plan, &job->engine.setup, route);
#endif
#if TEGRA_NVJPG_STATS
    if (!why) {
        tegra_nvjpg_note_route(*route, layout.held, layout.layout.row_bytes, layout.layout.y_log2, target.count,
                               &job->engine.setup, job->plan.extent.width, job->plan.extent.height);
    }
#endif
#if TEGRA_NVJPG_PROBES
    if (!why)
        tegra_nvjpg_count_route(job->pid, *route);
#endif
    return why;
}

IOReturn ended(Job *job, int error, const nvjpg_dec_status &status, uint64_t busy)
{
#if TEGRA_NVJPG_PROBES
    if (tegra_nvjpg_cancellations() != job->cancelsBefore)
        tegra_nvjpg_count_cancelled(job->pid);
#endif
    IOReturn ret = ioreturn(error);
    if (!ret && (status.error_status || !status.bytes_offset))
        ret = kIOReturnIOError;
    // AppleJPEGDriver ends its destination's write with IOSurface's device write end, as VIC does: the seed moves, so a
    // surface a decode wrote never reads as one nothing wrote (TegraGM20BRecord.h) and seed-keyed caches see the picture
    if (!ret)
        tegra_vic_surface_written(&job->dst);
    job->engineUs = busy;
    job->bytesUsed = status.bytes_offset;
    job->cycles = status.reserved0;  // the cycle count (DUMP_CYCLE_COUNT)
    if (ret == kIOReturnIOError && status.error_status) {
        IOLog("TegraNVJPG: decode error %#x after %u bytes (MCU %u,%u) of a %ux%u picture\n", status.error_status,
              status.bytes_offset, status.mcu_x, status.mcu_y, static_cast<unsigned>(job->engine.setup.frame_width),
              static_cast<unsigned>(job->engine.setup.frame_height));
    }
    return ret;
}

// NVJPG's end of the decode in flight, from its fence's work loop or the falcon line's.
void jobEngineEnded(void *context)
{
    auto *job = static_cast<Job *>(context);
    __atomic_store_n(&job->engineEnded, true, __ATOMIC_RELEASE);
    IOLockLock(gQueueLock);
    job->woken = true;
    IOLockWakeup(gQueueLock, job->directSync ? static_cast<void *>(job) : static_cast<void *>(&gHead), false);
    IOLockUnlock(gQueueLock);
}

// The end of the re-pack queued behind it, from VIC's or the copy engine's completion.
void jobRepackEnded(void *context)
{
    auto *job = static_cast<Job *>(context);
    __atomic_store_n(&job->repacked, true, __ATOMIC_RELEASE);
    IOLockLock(gQueueLock);
    job->woken = true;
    IOLockWakeup(gQueueLock, job->directSync ? static_cast<void *>(job) : static_cast<void *>(&gHead), false);
    IOLockUnlock(gQueueLock);
}

// Engine owner: whether the re-pack queued behind NVJPG has yet to signal its end.
bool repackPending(Job *job)
{
    if (!job->repack.queued)
        return false;
    return !__atomic_load_n(&job->repacked, __ATOMIC_ACQUIRE);
}

void directWritePages(const TegraNVJPGPlan &p, uint32_t totalPages, uint32_t *firstPage, uint32_t *pageCount)
{
    uint64_t firstByte = p.planes ? p.planeStart[0] : 0;
    for (uint32_t i = 1; i < p.planes; ++i) {
        if (p.planeStart[i] < firstByte)
            firstByte = p.planeStart[i];
    }
    const uint32_t first = static_cast<uint32_t>(firstByte >> SWITCH_HW_PAGE_SHIFT);
    const uint32_t end =
        SWITCH_MIN(totalPages, static_cast<uint32_t>(switch_div_round_up(p.writeEnd, SWITCH_HW_PAGE_SIZE)));
    *firstPage = first < end ? first : end;
    *pageCount = end - *firstPage;
}

// Engine thread, once nothing is pending: a re-packed decode's result.
IOReturn completeRepack(Job *job)
{
    const int error = tegra_nvjpg_repack_end(&job->repack, job->engineResult);
    const nvjpg_dec_status &status = job->engineStatus;
    if (!error && job->plan.fillChroma && !status.error_status && status.bytes_offset && job->backing) {
        const SwitchNVJPGBacking target = switch_nvjpg_backing(job->backing);
        fillChroma(job->plan, target.pages, target.count);
    }
    return ended(job, error, status, job->engineBusy);
}

// Engine thread: a VIC ('422f'/'444f') or Copy Engine (layout-staged) re-packed decode (TegraNVJPGRepack.h) started.
// True: it is in flight; false: it has ended, job->status its result.
bool beginRepack(Job *job)
{
    const TegraNVJPGSignal ended = {jobEngineEnded, job}, repacked = {jobRepackEnded, job};
    const char *why = nullptr;
#if TEGRA_NVJPG_PROBES
    const bool guardClosed = job->guardProbe;
#else
    const bool guardClosed = false;
#endif
    const int error = tegra_nvjpg_repack_start(&job->repack, &job->engine, &job->plan, &job->request.base, &job->dst,
                                               job->backing, job->scan->address, &ended, &repacked, guardClosed, &why);
    if (!error) {
        job->phase = Job::Phase::OnEngine;
        return true;
    }
    if (why) {
        refused(why, job->plan.target.format);
        job->status = kIOReturnUnsupported;
        return false;
    }
    job->engineResult = error;
    job->phase = Job::Phase::Repacking;
    if (repackPending(job))
        return true;
    job->status = completeRepack(job);
    return false;
}

// Engine owner: NVJPG's direct decode has ended and is finished.
IOReturn completeDirect(Job *job)
{
    const TegraNVJPGPlan &p = job->plan;
    uint32_t firstPage = 0, pageCount = 0;
    directWritePages(p, job->mapping->count, &firstPage, &pageCount);
    if (pageCount) {
        // Drop any clean lines Cortex-A57 speculatively prefetched while NVJPG wrote DRAM; never clean-invalidate
        // post-DMA.
        switch_dcache_hw_pages(SWITCH_DCACHE_INVALIDATE, job->mapping->pages + firstPage, pageCount);
        switch_dcache_sync();
    }
    const nvjpg_dec_status &status = job->engineStatus;
    if (!job->engineResult && p.fillChroma && !status.error_status && status.bytes_offset)
        fillChroma(p, job->mapping->pages, job->mapping->count);
    return ended(job, job->engineResult, status, job->engineBusy);
}

// Engine owner: NVJPG straight into the destination's pages, started. True: in flight; false: ended (job->status).
bool beginDirect(Job *job, const SwitchNVJPGBacking &target, uint32_t extent)
{
    const TegraNVJPGPlan &p = job->plan;
    const char *why = nullptr;
    job->mapping = switch_nvjpg_mapping(target, extent, p.writeEnd, &why, job->pid);
    if (!job->mapping) {
        refused(why, p.target.format);
        job->status = kIOReturnUnsupported;
        return false;
    }
    bool needPreClean = true;
    if (job->backing) {
        switch_gpu_backing_info info = {};
        switch_gpu_backing_describe(job->backing, &info);
        needPreClean = (info.dirty & SWITCH_GPU_BACKING_CPU_DIRTY) != 0 || info.cpu_locks != 0;
    }
    if (needPreClean) {
        uint32_t firstPage = 0, pageCount = 0;
        directWritePages(p, job->mapping->count, &firstPage, &pageCount);
        if (pageCount) {
            // NVJPG does not snoop CPU caches: clean-invalidate CPU-dirty destination pages before DMA so no dirty
            // line is evicted over NVJPG's writes.
            switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, job->mapping->pages + firstPage, pageCount);
            switch_dcache_sync();
        }
    }
    job->engine.bitstream = job->scan->address;
    for (uint32_t i = 0; i < p.planes; i++)
        job->engine.plane[i] = job->mapping->address + p.planeStart[i];
    const TegraNVJPGSignal ended = {jobEngineEnded, job};
    tegra_nvjpg_lock();
    const int error = tegra_nvjpg_start(&job->engine, &ended);
    if (!error) {
        job->phase = Job::Phase::OnEngine;
        return true;
    }
    tegra_nvjpg_unlock();
    job->engineResult = error;
    job->status = completeDirect(job);
    return false;
}

// Engine thread: one decode started. VIC's re-pack writes planar YUV, which GM20B never lays out block-linear, through
// VIC's own view of the destination. True: in flight; false: ended, job->status its result.
bool beginJob(Job *job)
{
#if TEGRA_NVJPG_PROBES
    job->cancelsBefore = tegra_nvjpg_cancellations();
#endif
    if (job->plan.repack == TEGRA_NVJPG_REPACK_VIC)
        return beginRepack(job);
    uint32_t extent = static_cast<uint32_t>(switch_div_round_up(job->dst.desc.length, SWITCH_HW_PAGE_SIZE));
    const SwitchNVJPGBacking target = switch_nvjpg_backing(job->backing);
    if (target.count < extent) {
        refused("destination longer than its backing", job->plan.target.format);
        job->status = kIOReturnUnsupported;
        return false;
    }
    tegra_vic_surface_written(&job->dst);
    uint32_t route = TEGRA_NVJPG_ROUTE_ROWS;
    if (const char *why = routeJob(job, target, &job->hold, &route)) {
        refused(why, job->plan.target.format);
        job->status = kIOReturnUnsupported;
        return false;
    }
    return job->plan.repack ? beginRepack(job) : beginDirect(job, target, extent);
}

// Engine owner, after a signal: the decode in flight carried on. True: it has ended, job->status its result.
bool advanceJob(Job *job)
{
    const bool engineEnded = __atomic_load_n(&job->engineEnded, __ATOMIC_ACQUIRE);
    if (job->phase == Job::Phase::OnEngine) {
        if (!engineEnded)
            return false;
        job->engineResult = tegra_nvjpg_finish(&job->engine, &job->engineStatus, &job->engineBusy);
        tegra_nvjpg_unlock();
        if (!job->plan.repack) {
            job->status = completeDirect(job);
            return true;
        }
        job->phase = Job::Phase::Repacking;
    }
    if (repackPending(job))
        return false;
    job->status = completeRepack(job);
    return true;
}

void finish(Job *job);

void giveHold(Job *job)
{
    if (job->hold) {
        tegra_gm20b_surface_unhold(job->hold);
        job->hold = nullptr;
    }
}

void noteConcluded(const Job *job)
{
#if TEGRA_NVJPG_PROBES
    tegra_nvjpg_count_job(job->pid, job->plan.target.format, job->status != 0, job->engineUs, job->cpuUs,
                          static_cast<uint64_t>(job->plan.extent.width) * job->plan.extent.height,
                          static_cast<uint64_t>(job->engine.setup.bitstream_size));
#endif
    const uint32_t format = 1u << job->plan.target.format;
    const bool first = (job->status == 0) && (gFirstLogged & format) == 0u;
    if (first)
        gFirstLogged |= format;
    if (first) {
        IOLog("TegraNVJPG: first hardware decode done: %ux%u to %ux%u (format %u, 1/%u) in %llu us engine (%u "
              "cycles), %llu us CPU\n",
              static_cast<unsigned>(job->engine.setup.frame_width),
              static_cast<unsigned>(job->engine.setup.frame_height), job->plan.extent.width, job->plan.extent.height,
              job->plan.target.format, 1u << job->plan.target.scale, job->engineUs, job->cycles, job->cpuUs);
    }
#if TEGRA_NVJPG_STATS
    tegra_nvjpg_stats_line();
#endif
}

// Engine thread: the ended decode's layout hold given back, its scan slot freed, its counts kept, its client told.
void conclude(Job *job)
{
    giveHold(job);
    noteConcluded(job);
    IOLockLock(gQueueLock);
    giveScanLocked(job);
    gEngineBusy = false;
    IOLockUnlock(gQueueLock);
    finish(job);
}

// One decode at a time, NVJPG's part and its re-pack ending by signal (jobEngineEnded, jobRepackEnded): between its
// start and its ends this thread sleeps on the queue's lock, not on the engine, and a signal only wakes it to carry the
// decode on (advanceJob). A queued decode starts once the one before it has ended.
void engineThread(void *, wait_result_t)
{
    Job *current = nullptr;
    for (;;) {
        Job *next = nullptr;
        IOLockLock(gQueueLock);
        gEngineSleeping = true;
        while (current ? !current->woken : (gEngineBusy || !gHead))
            IOLockSleep(gQueueLock, &gHead, THREAD_UNINT);
        gEngineSleeping = false;
        if (current) {
            current->woken = false;
        } else {
            next = gHead;
            gHead = next->next;
            if (!gHead)
                gTail = nullptr;
            gEngineBusy = true;
        }
        IOLockUnlock(gQueueLock);
        if (next) {
            current = next;
            if (beginJob(current))
                continue;
        } else if (!advanceJob(current)) {
            continue;
        }
        conclude(current);
        current = nullptr;
    }
}

void pushQueuedLocked(Job *job)
{
    job->next = nullptr;
    if (gTail) {
        gTail->next = job;
    } else {
        gHead = job;
    }
    gTail = job;
    if (!gEngineBusy && gEngineSleeping)
        IOLockWakeup(gQueueLock, &gHead, false);
}

void queueJob(Job *job)
{
    IOLockLock(gQueueLock);
    pushQueuedLocked(job);
    IOLockUnlock(gQueueLock);
}

bool runSyncJob(Job *job)
{
    IOLockLock(gQueueLock);
    if (gEngineBusy || gHead) {
        pushQueuedLocked(job);
        IOLockUnlock(gQueueLock);
        return false;
    }
    gEngineBusy = true;
    job->directSync = true;
    IOLockUnlock(gQueueLock);
    for (bool inFlight = beginJob(job); inFlight; inFlight = !advanceJob(job)) {
        IOLockLock(gQueueLock);
        while (!job->woken)
            IOLockSleep(gQueueLock, job, THREAD_UNINT);
        job->woken = false;
        IOLockUnlock(gQueueLock);
    }
    giveHold(job);
    noteConcluded(job);
    IOLockLock(gQueueLock);
    giveScanLocked(job);
    gEngineBusy = false;
    if (gHead && gEngineSleeping)
        IOLockWakeup(gQueueLock, &gHead, false);
    IOLockUnlock(gQueueLock);
    return true;
}

}  // namespace tegra_nvjpg
