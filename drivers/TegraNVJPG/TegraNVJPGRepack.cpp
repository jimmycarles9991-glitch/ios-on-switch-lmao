// SPDX-License-Identifier: BSD-2-Clause-Patent
// See TegraNVJPGRepack.h. The buffer is registered as a GPU backing, so VIC maps and orders it as any engine buffer; NVJPG
// records nothing in it, and VIC's job waits for NVJPG's syncpoint instead.
#include <IOKit/IOLib.h>
#include "TegraNVJPGRepack.h"
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../TegraVIC/SwitchVICDecodeOutput.h"
#include "../../kits/SwitchDMA/SwitchDMAPages.h"

namespace {
// Grown to the largest decode yet and kept: decodes run one at a time under the job lock, and each returns only once
// VIC has read the buffer, so the next may write it.
switch_dma_pages *gPages;
switch_gpu_backing *gBacking;
// Only what IOLockSleep needs to sleep on VIC's completion: the engine thread and that completion's thread call are its
// only takers, so nothing else contends for it.
IOLock *gWaitLock;

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

struct Wait {
    bool queued, done;
    int status;
    const char *why;
};

struct Queue {
    const TegraNVJPGPlan *plan;
    const TegraNVJPGDecodeRequest *request;
    const TegraVICSurface *destination;
    Wait *wait;
};

void repacked(void *context, int status)
{
    auto *wait = static_cast<Wait *>(context);
    IOLockLock(gWaitLock);
    wait->status = status;
    wait->done = true;
    IOLockWakeup(gWaitLock, wait, false);
    IOLockUnlock(gWaitLock);
}

void queueRepack(void *context, uint32_t syncpt, uint32_t fence)
{
    const auto *q = static_cast<const Queue *>(context);
    const TegraNVJPGPlan *p = q->plan;
    const TegraVICAcquire after = {syncpt, fence};
    SwitchVICDecodeOutput output = {};
    output.source = gBacking;
    for (uint32_t i = 0; i < 3; ++i) {
        output.planeOffset[i] = p->planeStart[i];
        output.pitch[i] = p->target.pitch[i];
        output.rows[i] = p->extent.rows[i];
    }
    output.chroma444 = p->target.format == TEGRA_NVJPG_OUT_NV24 ? 1 : 0;
    output.width = q->request->out_width;
    output.height = q->request->out_height;
    output.x = q->request->x_offset;
    output.y = q->request->y_offset;
    output.after = &after;
    q->wait->queued = switch_vic_decode_output(q->destination, &output, repacked, q->wait, &q->wait->why) == 0;
}
}  // namespace

int tegra_nvjpg_repack(TegraNVJPGJob *engine, const TegraNVJPGPlan *plan, const TegraNVJPGDecodeRequest *request,
                       const TegraVICSurface *destination, const uint64_t *bits, uint32_t bitPages,
                       nvjpg_dec_status *status, uint64_t *busyUs, const char **why)
{
    *why = nullptr;
    if (!gWaitLock && !(gWaitLock = IOLockAlloc()))
        return -12;
    if (const int error = grow(plan->repackBytes)) {
        *why = "no memory for the planes VIC re-packs";
        return error;
    }
    tegra_nvjpg_lock();
    const uint64_t out = tegra_nvjpg_window_map(TEGRA_NVJPG_WINDOW_OUT, gPages->pages, gPages->count, true);
    const uint64_t in = tegra_nvjpg_window_map(TEGRA_NVJPG_WINDOW_IN, bits, bitPages, false);
    Wait wait = {};
    int ret = out && in ? 0 : -12;
    if (!ret) {
        engine->bitstream = in;
        for (uint32_t i = 0; i < plan->planes; ++i)
            engine->plane[i] = out + plan->planeStart[i];
        Queue q = {plan, request, destination, &wait};
        const TegraNVJPGFollow follow = {queueRepack, &q};
        ret = tegra_nvjpg_run(engine, status, busyUs, &follow);
    }
    tegra_nvjpg_window_clear(TEGRA_NVJPG_WINDOW_OUT);
    tegra_nvjpg_window_clear(TEGRA_NVJPG_WINDOW_IN);
    tegra_nvjpg_unlock();
    if (wait.queued) {
        IOLockLock(gWaitLock);
        while (!wait.done)
            IOLockSleep(gWaitLock, &wait, THREAD_UNINT);
        IOLockUnlock(gWaitLock);
        if (wait.status)
            IOLog("TegraNVJPG: VIC's re-pack into the destination failed (%d); this decode fails\n", wait.status);
        return ret ? ret : wait.status;
    }
    if (wait.why) {
        IOLog("TegraNVJPG: VIC refused the re-pack into the destination (%s); this decode fails\n", wait.why);
        return ret ? ret : -5;
    }
    return ret;
}
