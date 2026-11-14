// SPDX-License-Identifier: BSD-2-Clause-Patent
// A decoder's native picture read by NVENC in place: no VIC job. The source IOSurface's NVDEC picture (16Bx2 block
// linear, 2-GOB blocks: NVENC's code 0, NVIDIA's driver's own input) is mapped read-only into NVENC's address space,
// and the job's input relocations point at its planes, input_cfg rewritten to the picture's pitch and block height
// (TegraNVENCDirectPlan.h). The read is a use in SwitchGpuBacking under engine SWITCH_GPU_ENGINE_NVENC: the job waits on
// the hardware for the picture's producers (NVDEC and VIC by their syncpoints; a producer's failure is the job's,
// TegraNVENCInputs.h), and later writers of the picture wait for the job's fence (VIC, NVDEC and the GPU by host1x
// waits, the CPU by tegra_nvenc_wait). A picture the GPU still writes (a GM20B job, or a writer promise) is ordered
// as VIC and NVDEC readers of it are: prepare submits the GPU-to-host1x bridge for those writers outside every queue
// lock (SwitchGpuBridge.h), and begin has the job wait on the bridge's syncpoint, with a writer recorded as its
// inputs' producer so a failed GPU job fails the frame. A writer the bridge does not cover (one that came after
// prepare), or a bridge that cannot be submitted, sends the picture through VIC.
#include <IOKit/IOLib.h>
#include "TegraNVENC.h"
#include "TegraNVENCDirect.h"
#include "TegraNVENCDirectPlan.h"
#include "../TegraHost1x/SwitchHost1xDependencies.h"
#include "../TegraHost1x/SwitchHost1xFence.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace {
constexpr uint32_t kProducers = 8;

struct Retire {
    switch_host1x_fence fence;
    void *map;
    switch_gpu_bridge *bridge;
};

void retired(switch_host1x_fence *fence, int)
{
    auto *job = reinterpret_cast<Retire *>(fence);
    tegra_nvenc_native_unmap(job->map);
    switch_gpu_bridge_release(&job->bridge);
    IODelete(job, Retire, 1);
    switch_gpu_backing_completed();
}

// The picture's uses opened under `completion`, and the producers its read waits for: its writers, then its writers'
// promises.
int openUses(TegraNVENCDirect *d, const switch_gpu_backing_token &completion, switch_gpu_backing_token *producers,
             uint32_t *needed)
{
    d->uses[0] = {d->view.native, SWITCH_GPU_BACKING_READ, 0};
    d->uses[1] = {d->view.logical, SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, 0};
    const switch_gpu_backing_guard guards[2] = {d->view.guard, d->view.representation};
    int error = switch_gpu_backing_begin_checked(d->uses, 2, guards, 2, &completion, &d->transaction);
    if (error)
        return error == SWITCH_GPU_BACKING_STALE ? -11 : error;
    uint32_t writers = 0, promised = 0;
    error = switch_gpu_backing_dependencies(&d->transaction, producers, kProducers, &writers);
    if (!error)
        error = switch_gpu_backing_promise_dependencies(&d->transaction, producers + writers, kProducers - writers,
                                                        &promised, 0, 0);
    *needed = writers + promised;
    if (error)
        switch_gpu_backing_abort(&d->transaction);
    return error == -28 ? -11 : error;
}

// Prepare's pass over the producers: the GPU writers still writing go to the bridge, submitted here because the bridge
// is submitted outside every queue lock and begin runs under NVENC's engine lock.
int bridgeGpuWriters(TegraNVENCDirect *d)
{
    SwitchHost1xDependencies writers;
    uint32_t needed = 0;
    if (openUses(d, tegra_nvenc_token(0), writers.tokens, &needed))
        return -11;
    uint32_t pending = 0;
    for (uint32_t i = 0; i < needed; ++i) {
        const int status = switch_gpu_backing_token_external_status(&writers.tokens[i]);
        pending += tegra_nvenc_direct_producer(&writers.tokens[i], status) == TEGRA_NVENC_DIRECT_BRIDGE;
    }
    const int held = pending ? writers.holdGpu(needed) : 0;
    switch_gpu_backing_abort(&d->transaction);
    if (!pending)
        return 0;
    if (held || writers.submitBridge())
        return -11;
    d->bridge = writers.takeBridge();
    return 0;
}

int begin(void *context, uint32_t fence, TegraNVENCAfter *waits, uint32_t *count)
{
    auto *d = static_cast<TegraNVENCDirect *>(context);
    d->token = tegra_nvenc_token(fence);
    switch_gpu_backing_token producers[kProducers];
    uint32_t needed = 0;
    *count = 0;
    if (const int error = openUses(d, d->token, producers, &needed))
        return error;
    int error = 0;
    bool bridged = false;
    for (uint32_t i = 0; !error && i < needed; ++i) {
        const auto &producer = producers[i];
        const int wait = tegra_nvenc_direct_producer(&producer, switch_gpu_backing_token_external_status(&producer));
        if (wait == TEGRA_NVENC_DIRECT_BRIDGE) {
            if (!d->bridge || switch_gpu_bridge_covers(d->bridge, &producer) != 1) {
                error = -11;
            } else if (!bridged && *count < TEGRA_NVENC_ORDER_WAITS) {
                uint32_t syncpt = 0, threshold = 0;
                switch_gpu_bridge_syncpoint(d->bridge, &syncpt, &threshold);
                waits[(*count)++] = {syncpt, threshold, producer};
                bridged = true;
            } else if (!bridged) {
                error = -11;
            }
        } else if (wait == 1 && *count < TEGRA_NVENC_ORDER_WAITS) {
            waits[(*count)++] = {static_cast<uint32_t>(producer.queue), static_cast<uint32_t>(producer.value),
                                 producer};
        } else if (wait) {
            error = wait == 1 ? -11 : wait;
        }
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
        job->bridge = d->bridge;
        switch_host1x_fence_init(&job->fence, tegra_nvenc_syncpt(), fence, retired);
        if (switch_host1x_fence_add(&job->fence, 0) == TEGRA_HOST1X_OK) {
            d->map = nullptr;
            d->bridge = nullptr;
            return;
        }
        IODelete(job, Retire, 1);
    }
    // No fence: the mapping and the bridge stay held for good rather than go while the job may read the picture.
    d->map = nullptr;
    d->bridge = nullptr;
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
    if (const int error = bridgeGpuWriters(d))
        return error;
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
    switch_gpu_bridge_release(&d->bridge);
    tegra_nvenc_native_unmap(d->map);
    switch_native_surface_release(&d->view);
    tegra_vic_surface_release(&d->surface);
    *d = {};
}
