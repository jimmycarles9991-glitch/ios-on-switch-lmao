// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TegraNVJPGRepack.h. The buffer is registered as a GPU backing, so VIC maps and orders it as any engine buffer; NVJPG
// records nothing in it, and VIC's job waits for NVJPG's syncpoint instead.
#include <IOKit/IOLib.h>
#include "TegraNVJPGRepack.h"
#include "TegraNVJPGMapping.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../TegraGM20B/SwitchGpuSharedEvent.h"
#include "../TegraGM20B/TegraGM20BCopy.h"
#include "../TegraVIC/SwitchVICDecodeOutput.h"
#include "../TegraVIC/SwitchVICJobs.h"
#include "../TegraVIC/SwitchVICSurfaceAccess.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchMath/SwitchMath.h"

namespace {
// Grown to the largest decode yet and kept: decodes run one at a time on the engine thread, and none starts before the
// one before it has ended, its re-pack included (tegra_nvjpg_repack_end), so the next may write it.
switch_dma_pages *gPages;
switch_gpu_backing *gBacking;
// The last 16 bytes of gPages, past every byte a decode writes: the copy engine's re-pack runs only while the word at
// their start is not zero (tegra_gm20b_ce_wait.when). A cancelled decode zeroes it before its fence passes, so the
// re-pack copies none of the buffer, which may still hold an earlier decode's picture, another process's. 16 bytes and
// 16-byte aligned: the only hardware evidence for the predicate (Mesa's nak hw_tests) reads a 16-byte structure there.
constexpr uint64_t kGuardBytes = 16;

uint64_t guardOffset()
{
    return gPages->size - kGuardBytes;
}

void storeGuard(uint64_t value)
{
    auto *guard = reinterpret_cast<volatile uint64_t *>(gPages->va + guardOffset());
    guard[0] = value;
    guard[1] = 0;
}

// Before the copies are queued: published as a CPU write of the backing, so their submission invalidates the GPU L2's
// sysmem lines ahead of their syncpoint wait, and the predicate reads this value or the cancel's from DRAM, never a
// line an earlier decode's copy left there. AVOID_SYNC: the earlier decode's re-pack has ended, so no engine use is
// pending, and this thread never sleeps on one.
int openGuard(uint64_t value)
{
    constexpr uint32_t access =
        SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE | SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
    int error = switch_gpu_backing_cpu_acquire(gBacking, guardOffset(), kGuardBytes, access);
    if (error)
        return error;
    storeGuard(value);
    error = switch_gpu_backing_cpu_publish(gBacking, guardOffset(), kGuardBytes);
    const int released = switch_gpu_backing_cpu_release(gBacking, guardOffset(), kGuardBytes, access);
    return error ? error : released;
}

// On the engine thread after the copies are queued: no backing CPU access, whose write would wait for them, and they
// wait for this decode's fence. The L2 already dropped the guard's line at their submission (openGuard).
void closeGuard()
{
    storeGuard(0);
    switch_dcache_clean(gPages->va + guardOffset(), kGuardBytes);
}

void releasePages(void *owner)
{
    auto *pages = static_cast<switch_dma_pages *>(owner);
    switch_dma_pages_free(pages);
    IODelete(pages, switch_dma_pages, 1);
}

int grow(uint64_t bytes)
{
    if (gBacking && gPages->size >= bytes)
        return 0;
    auto *pages = IONewZero(switch_dma_pages, 1);
    if (!pages)
        return -12;
    if (!switch_dma_pages_alloc(pages, static_cast<size_t>(bytes))) {
        IODelete(pages, switch_dma_pages, 1);
        return -12;
    }
    auto *backing = switch_gpu_backing_register(pages->pages, pages->count, 0, pages, releasePages);
    if (!backing) {
        releasePages(pages);
        return -12;
    }
    if (gBacking)
        switch_gpu_backing_release(gBacking);
    gPages = pages;
    gBacking = backing;
    return 0;
}

// The signal is a completion's last use of the run: once it has run, the run's owner may end the decode and free it.
void repackEnded(TegraNVJPGRepackRun *run)
{
    run->signal.ended(run->signal.context);
}

void repacked(void *context, int status)
{
    auto *run = static_cast<TegraNVJPGRepackRun *>(context);
    __atomic_store_n(&run->status, status, __ATOMIC_RELEASE);
    repackEnded(run);
}

void ceCompleted(void *context, int status)
{
    auto *run = static_cast<TegraNVJPGRepackRun *>(context);
    if (status) {
        int expected = 0;
        __atomic_compare_exchange_n(&run->status, &expected, status, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    if (__atomic_sub_fetch(&run->pending, 1u, __ATOMIC_ACQ_REL))
        return;
    repackEnded(run);
}

// NVJPG's follow, run inside tegra_nvjpg_start right after the decode's submission, on the engine thread
void queueRepack(void *context, uint32_t syncpt, uint32_t fence)
{
    auto *run = static_cast<TegraNVJPGRepackRun *>(context);
    const TegraNVJPGPlan *p = run->plan;
    const TegraVICAcquire after = {syncpt, fence};
    SwitchVICDecodeOutput output = {};
    output.source = gBacking;
    for (uint32_t i = 0; i < 3; ++i) {
        output.planeOffset[i] = p->planeStart[i];
        output.pitch[i] = p->target.pitch[i];
        output.rows[i] = p->extent.rows[i];
    }
    output.chroma444 = p->target.format == TEGRA_NVJPG_OUT_NV24 ? 1 : 0;
    output.width = run->request->out_width;
    output.height = run->request->out_height;
    output.x = run->request->x_offset;
    output.y = run->request->y_offset;
    output.after = &after;
    output.fence = &run->vicFence;
    const char *why = nullptr;
    run->queued = switch_vic_decode_output(run->destination, &output, repacked, run, &why) == 0;
    run->why = why;
}

void queueCeRepack(void *context, uint32_t syncpt, uint32_t fence)
{
    auto *run = static_cast<TegraNVJPGRepackRun *>(context);
    const TegraNVJPGPlan *p = run->plan;
    const tegra_gm20b_ce_wait after = {syncpt, fence, 1, guardOffset()};
    const tegra_gm20b_ce_done done = {TEGRA_GM20B_CE_NO_SYNCPT, ceCompleted, run};
    switch_gpu_cpu_lease *lease = run->access.owned;
    const switch_gpu_copy_ownership ownership = {lease ? &lease : nullptr, lease ? 1u : 0u, &run->access.guard, 1};
    const switch_gpu_event_submission none = {};
    // One count for the queueing itself, so no plane's completion ends the re-pack while later planes are queued
    __atomic_store_n(&run->pending, 1u, __ATOMIC_RELEASE);
    bool queued = false;
    const char *why = nullptr;
    const int guard = openGuard(run->guardClosed ? 0 : 1);
    if (guard) {
        __atomic_store_n(&run->status, guard, __ATOMIC_RELEASE);
        why = "the copy guard was not published";
    }
    for (uint32_t i = 0; !guard && i < p->planes; ++i) {
        const tegra_gm20b_ce_plane src = {gPages->memory, p->planeStart[i], p->target.pitch[i]};
        const tegra_gm20b_ce_plane dst = {run->destination->memory, run->destination->baseOffset + p->copyDstStart[i],
                                          p->copyDstPitch[i]};
        __atomic_add_fetch(&run->pending, 1u, __ATOMIC_RELAXED);
        const int error = tegra_gm20b_ce_copy_plane_checked(&src, &dst, p->copyRowBytes[i], p->copyRows[i], &after,
                                                            &done, gBacking, p->planeStart[i], run->access.backing,
                                                            run->access.offset + p->copyDstStart[i], &ownership, &none);
        if (error) {
            int expected = 0;
            __atomic_compare_exchange_n(&run->status, &expected, error, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
            __atomic_sub_fetch(&run->pending, 1u, __ATOMIC_ACQ_REL);
            if (!queued)
                why = "copy engine refused the re-pack";
            break;
        }
        queued = true;
    }
    switch_vic_surface_unreserve(&run->access);
    run->queued = queued;
    run->why = why;
    if (queued && !__atomic_sub_fetch(&run->pending, 1u, __ATOMIC_ACQ_REL))
        repackEnded(run);
}

// The decode was cancelled: the copies queued behind its fence launch with the guard zero and write nothing
void ceCancelled(void *)
{
    closeGuard();
}

// The decode was cancelled: VIC's job queued behind its fence renders its background, not the buffer
void vicCancelled(void *context)
{
    auto *run = static_cast<TegraNVJPGRepackRun *>(context);
    if (run->queued)
        (void)switch_vic_job_background(run->vicFence);
}
}  // namespace

int tegra_nvjpg_repack_start(TegraNVJPGRepackRun *run, TegraNVJPGJob *engine, const TegraNVJPGPlan *plan,
                             const TegraNVJPGDecodeRequest *request, const TegraVICSurface *destination,
                             switch_gpu_backing *backing, uint64_t bitstream, const TegraNVJPGSignal *ended,
                             const TegraNVJPGSignal *repacked, bool guardClosed, const char **why)
{
    *why = nullptr;
    *run = {};
    run->guardClosed = guardClosed;
    run->plan = plan;
    run->request = request;
    run->destination = destination;
    run->signal = *repacked;
    run->ce = plan->repack == TEGRA_NVJPG_REPACK_CE;
    if (run->ce) {
        if (!destination->memory || !plan->planes || plan->planes > 2) {
            *why = "destination memory unavailable for copy-engine re-pack";
            return -22;
        }
        for (uint32_t i = 0; i < plan->planes; ++i) {
            if ((destination->baseOffset + plan->copyDstStart[i]) & 15) {
                *why = "destination plane offset not 16-byte aligned";
                return -95;
            }
        }
        if (const int error = switch_vic_surface_access(destination, true, &run->access, false, backing)) {
            *why = "the destination's write could not be acquired";
            return error;
        }
    }
    if (const int error = grow(SWITCH_MAX(plan->repackBytes, plan->writeEnd) + kGuardBytes)) {
        switch_vic_surface_release_access(&run->access);
        *why = run->ce ? "no memory for the planes Copy Engine re-packs" : "no memory for the planes VIC re-packs";
        return error;
    }
    const SwitchNVJPGMapping *mapping =
        switch_nvjpg_mapping(switch_nvjpg_backing(gBacking), gPages->count, plan->writeEnd, why);
    if (!mapping) {
        switch_vic_surface_release_access(&run->access);
        return -28;
    }
    engine->bitstream = bitstream;
    for (uint32_t i = 0; i < plan->planes; ++i)
        engine->plane[i] = mapping->address + plan->planeStart[i];
    const TegraNVJPGFollow follow = {run->ce ? queueCeRepack : queueRepack, run, run->ce ? ceCancelled : vicCancelled};
    tegra_nvjpg_lock();
    const int ret = tegra_nvjpg_start(engine, ended, &follow);
    if (ret)
        tegra_nvjpg_unlock();
    return ret;
}

int tegra_nvjpg_repack_end(TegraNVJPGRepackRun *run, int ret)
{
    const int status = __atomic_load_n(&run->status, __ATOMIC_ACQUIRE);
    switch_vic_surface_release_access(&run->access);
    if (run->queued) {
        if (status)
            IOLog("TegraNVJPG: %s re-pack into the destination failed (%d); this decode fails\n",
                  run->ce ? "Copy Engine's" : "VIC's", status);
        return ret ? ret : status;
    }
    if (run->why) {
        IOLog("TegraNVJPG: %s refused the re-pack into the destination (%s); this decode fails\n",
              run->ce ? "Copy Engine" : "VIC", run->why);
        return ret ? ret : -5;
    }
    return ret;
}
