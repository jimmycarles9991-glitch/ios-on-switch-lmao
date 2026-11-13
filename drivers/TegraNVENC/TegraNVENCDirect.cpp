// SPDX-License-Identifier: BSD-2-Clause-Patent
// A decoder's native picture read by NVENC in place: no VIC job. The source IOSurface's NVDEC picture (16Bx2 block
// linear, 2-GOB blocks: NVENC's code 0, NVIDIA's driver's own input) is mapped read-only into NVENC's address space,
// and the job's input relocations point at its planes, input_cfg rewritten to the picture's pitch and block height
// (TegraNVENCDirectPlan.h). The read is a use in SwitchGpuBacking under engine SWITCH_GPU_ENGINE_NVENC: the job waits on
// the hardware for the picture's producers (NVDEC and VIC by their syncpoints; a producer's failure is the job's,
// TegraNVENCInputs.h), and later writers of the picture wait for the job's fence (VIC, NVDEC and the GPU by host1x
// waits, the CPU by tegra_nvenc_wait). A producer that only the GPU's bridge orders (GM20B) sends the picture through
// VIC instead.
#include <IOKit/IOLib.h>
#include "TegraNVENC.h"
#include "TegraNVENCDirect.h"
#include "TegraNVENCDirectPlan.h"
#include "../TegraHost1x/SwitchHost1xFence.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace {
struct Retire {
    switch_host1x_fence fence;
    void *map;
};

void retired(switch_host1x_fence *fence, int)
{
    auto *job = reinterpret_cast<Retire *>(fence);
    tegra_nvenc_native_unmap(job->map);
    IODelete(job, Retire, 1);
    switch_gpu_backing_completed();
}

int begin(void *context, uint32_t fence, TegraNVENCAfter *waits, uint32_t *count)
{
    auto *d = static_cast<TegraNVENCDirect *>(context);
    d->token = tegra_nvenc_token(fence);
    d->uses[0] = {d->view.native, SWITCH_GPU_BACKING_READ, 0};
    d->uses[1] = {d->view.logical, SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, 0};
    const switch_gpu_backing_guard guards[2] = {d->view.guard, d->view.representation};
    int error = switch_gpu_backing_begin_checked(d->uses, 2, guards, 2, &d->token, &d->transaction);
    if (error)
        return error == SWITCH_GPU_BACKING_STALE ? -11 : error;
    switch_gpu_backing_token producers[8];
    uint32_t needed = 0;
    error = switch_gpu_backing_dependencies(&d->transaction, producers, 8, &needed);
    *count = 0;
    for (uint32_t i = 0; !error && i < needed; ++i) {
        const auto &producer = producers[i];
        const int wait = tegra_nvenc_direct_producer(&producer, switch_gpu_backing_token_external_status(&producer));
        if (wait == 1 && *count < TEGRA_NVENC_ORDER_WAITS)
            waits[(*count)++] = {static_cast<uint32_t>(producer.queue), static_cast<uint32_t>(producer.value),
                                 producer};
        else if (wait)
            error = wait == 1 ? -11 : wait;
    }
    if (error) {
        switch_gpu_backing_abort(&d->transaction);
        return error == -28 ? -11 : error;
    }
    return 0;
}

void end(void *context, uint32_t fence, bool queued)
{
    auto *d = static_cast<TegraNVENCDirect *>(context);
    if (!queued) {
        switch_gpu_backing_abort(&d->transaction);
        return;
    }
    switch_gpu_backing_commit(&d->transaction);
    auto *job = IONewZero(Retire, 1);
    if (job) {
        job->map = d->map;
        switch_host1x_fence_init(&job->fence, tegra_nvenc_syncpt(), fence, retired);
        if (switch_host1x_fence_add(&job->fence, 0) == TEGRA_HOST1X_OK) {
            d->map = nullptr;
            return;
        }
        IODelete(job, Retire, 1);
    }
    // No fence: the mapping stays held for good rather than go while the job may read it.
    d->map = nullptr;
}
}  // namespace

int tegra_nvenc_direct_prepare(const TegraNVENCSubmit *s, task_t task, TegraNVENCDirect *d, TegraNVENCSubmit *patched,
                               const char **why)
{
    *d = {};
    if (tegra_vic_surface_lookup(s->source.surface_id, task, &d->surface))
        return -95;
    if (switch_native_surface_acquire(&d->surface, &d->view, true) || !d->view.owner)
        return -95;
    nvenc_h264_surface_cfg_s cfg;
    memcpy(&cfg, s->setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), sizeof(cfg));
    TegraNVENCDirectPlan plan = {};
    *why = tegra_nvenc_direct_plan(&cfg, &s->source, &d->view.layout, d->view.crop.x, d->view.crop.y,
                                   d->view.crop.width, d->view.crop.height, &plan);
    if (*why)
        return -95;
    d->map = tegra_nvenc_native_map(d->view.native, &d->block.iova, &d->block.size);
    if (!d->map) {
        *why = "no room in NVENC's address space for the native picture";
        return -95;
    }
    *patched = *s;
    patched->source = {};
    tegra_nvenc_direct_config(&plan, &cfg);
    memcpy(patched->setup + offsetof(nvenc_h264_drv_pic_setup_s, input_cfg), &cfg, sizeof(cfg));
    for (uint32_t i = 0; i < patched->entry_count; i++) {
        TegraNVENCEntry &e = patched->entries[i];
        if (e.kind != TEGRA_NVENC_ENTRY_RELOC)
            continue;
        if (e.method == NVC5B7_SET_IN_CUR_PIC)
            e = {e.method, TEGRA_NVENC_ENTRY_RELOC, TEGRA_NVENC_DIRECT_HANDLE, static_cast<uint32_t>(plan.luma)};
        else if (e.method == NVC5B7_SET_IN_CUR_PIC_CHROMA_U)
            e = {e.method, TEGRA_NVENC_ENTRY_RELOC, TEGRA_NVENC_DIRECT_HANDLE, static_cast<uint32_t>(plan.chroma)};
    }
    d->order = {begin, end, d};
    return 0;
}

void tegra_nvenc_direct_release(TegraNVENCDirect *d)
{
    tegra_nvenc_native_unmap(d->map);
    switch_native_surface_release(&d->view);
    tegra_vic_surface_release(&d->surface);
    *d = {};
}
