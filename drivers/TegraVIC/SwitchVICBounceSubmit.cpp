// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchVICBounceSubmit.h"
#include "SwitchVICBouncePool.h"
#include "SwitchVICOverwrite.h"
#include "SwitchVICSurfaceAccess.h"
#include "SwitchNativeVICImage.h"
#include "SwitchVICEvents.h"
#include "SwitchVICJobs.h"
#include "../TegraGM20B/TegraGM20BCopy.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

namespace {
struct Work {
    uint32_t refs;
    bool accepted;
    int status;
    SwitchVICSurfaceAccess access[2];
    SwitchNativeSurfaceView native;
    SwitchVICBounceSlot *slot;
    SwitchVICMapping *maps[2];
    TegraVICJob jobs[3];
    void (*completion)(void *, int);
    void *context;
};

void release(Work *work)
{
    if (__atomic_sub_fetch(&work->refs, 1u, __ATOMIC_ACQ_REL))
        return;
    for (unsigned i = 0; i < 2; ++i) {
        switch_vic_mapping_release(work->maps[i]);
        switch_vic_surface_release_access(&work->access[i]);
    }
    switch_native_surface_release(&work->native);
    switch_vic_bounce_release(work->slot);
    if (work->accepted)
        work->completion(work->context, work->status);
    IODelete(work, Work, 1);
}

void completed(void *context, int status)
{
    auto *work = static_cast<Work *>(context);
    if (status) {
        int expected = 0;
        __atomic_compare_exchange_n(&work->status, &expected, status, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    release(work);
}

bool tiled(switch_gpu_backing *backing)
{
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(backing, &info);
    for (uint32_t i = 0; i < info.layout.plane_count; ++i)
        if (info.layout.planes[i].layout != SWITCH_GPU_BACKING_PITCH)
            return true;
    tgm_layout old = {};
    return !info.layout.plane_count && info.page_count && tegra_gm20b_surface_layout(info.pages[0], &old) == 1;
}

struct Side {
    IOMemoryDescriptor *memory;
    switch_gpu_backing *backing;
    const TegraVICSurfaceDesc *desc;
    uint64_t memoryOffset, backingOffset;
};

int copy(const Side &from, const Side &to, Work *work, const switch_gpu_event_submission &events)
{
    const auto *format = tegra_vic_format(from.desc->fourcc);
    const tegra_gm20b_ce_done done = {TEGRA_GM20B_CE_NO_SYNCPT, completed, work};
    switch_gpu_cpu_lease *leases[2] = {};
    switch_gpu_backing_guard guards[2] = {};
    uint32_t leaseCount = 0, guardCount = 0;
    for (const auto &access : work->access) {
        if (!access.backing || (access.backing != from.backing && access.backing != to.backing))
            continue;
        guards[guardCount++] = access.guard;
        if (access.owned)
            leases[leaseCount++] = access.owned;
    }
    const switch_gpu_copy_ownership ownership = {leases, leaseCount, guards, guardCount};
    for (uint32_t p = 0; p < format->planes; ++p) {
        const tegra_gm20b_ce_plane src = {from.memory, from.memoryOffset + from.desc->planeOffset[p],
                                          static_cast<uint32_t>(from.desc->planeBytesPerRow[p])};
        const tegra_gm20b_ce_plane dst = {to.memory, to.memoryOffset + to.desc->planeOffset[p],
                                          static_cast<uint32_t>(to.desc->planeBytesPerRow[p])};
        const bool last = p + 1 == format->planes;
        auto stage = events;
        if (!last) {
            stage.waits = nullptr;
            stage.signals = nullptr;
            stage.signal_count = 0;
        }
        __atomic_add_fetch(&work->refs, 1u, __ATOMIC_RELAXED);
        const int error =
            tegra_gm20b_ce_copy_plane_checked(&src, &dst, tegra_vic_bounce_row_bytes(format, from.desc->width, p),
                                              static_cast<uint32_t>(from.desc->planeHeight[p]), nullptr, &done,
                                              from.backing, from.backingOffset + from.desc->planeOffset[p], to.backing,
                                              to.backingOffset + to.desc->planeOffset[p], &ownership, &stage);
        if (error) {
            release(work);
            return error;
        }
        work->accepted = true;
    }
    return 0;
}

Side original(const TegraVICSurface &surface, const SwitchVICSurfaceAccess &access)
{
    return {surface.memory, access.backing, &surface.desc, surface.baseOffset, access.offset};
}

Side scratch(const SwitchVICBounceBuffer &buffer)
{
    return {buffer.memory, buffer.backing, &buffer.desc, 0, 0};
}
}

int switch_vic_bounce_submit(const TegraVICSurface *source, const TegraVICSurface *destination,
                             const TegraVICPlan *plan, uint32_t chromaSite, void (*completion)(void *, int),
                             void *context, SwitchVICEvents *events)
{
    if (!destination || !plan || !completion || !plan->count || plan->count > 3 || (plan->readsSource && !source))
        return -22;
    auto *work = IONewZero(Work, 1);
    if (!work)
        return -12;
    work->refs = 1;
    work->completion = completion;
    work->context = context;
    const bool sharedDescriptor = plan->readsSource && source->memory == destination->memory;
    const bool alias = sharedDescriptor && source->baseOffset == destination->baseOffset &&
                       source->desc.length <= destination->desc.length;
    int error = switch_vic_surface_access(destination, true, &work->access[1]);
    if (!error && plan->readsSource && !alias) {
        const int native = switch_native_surface_acquire(source, &work->native, false);
        if (native == -2 || native == -95 || native == SWITCH_GPU_BACKING_STALE)
            error = switch_vic_surface_access(source, false, &work->access[0], work->access[1].owned != nullptr);
        else
            error = native;
    }
    if (error) {
        release(work);
        return error;
    }
    auto &sourceAccess = work->access[alias ? 1 : 0];
    const TegraVICTraits *traits = tegra_vic_traits();
    const bool bounceSrc =
        plan->readsSource && !work->native.owner &&
        (sharedDescriptor || tegra_vic_bounce_needed(&source->desc, tegra_vic_pitch_align(traits, true)) == 0 ||
         tiled(sourceAccess.backing));
    const bool bounceDst = tegra_vic_bounce_needed(&destination->desc, tegra_vic_pitch_align(traits, false)) == 0 ||
                           tiled(work->access[1].backing);
    error = switch_vic_bounce_acquire(bounceSrc ? &source->desc : nullptr, bounceDst ? &destination->desc : nullptr,
                                      &work->slot);
    if (error) {
        release(work);
        return error;
    }
    auto &sourceBuffer = work->slot->buffers[0];
    auto &destinationBuffer = work->slot->buffers[1];
    auto *input = work->native.owner ? work->native.native : bounceSrc ? sourceBuffer.backing : sourceAccess.backing;
    auto *output = bounceDst ? destinationBuffer.backing : work->access[1].backing;
    work->maps[1] = bounceDst ? nullptr : switch_vic_mapping_acquire(output, true);
    if (plan->readsSource && !bounceSrc)
        work->maps[0] = switch_vic_mapping_acquire(input, false);
    if ((!bounceDst && !work->maps[1]) || (plan->readsSource && !bounceSrc && !work->maps[0])) {
        release(work);
        return -12;
    }
    const uint64_t dstAddress = switch_vic_mapping_address(bounceDst ? destinationBuffer.mapping : work->maps[1]);
    TegraVICImage src = {}, dst = {};
    const char *why = tegra_vic_image_from(bounceDst ? &destinationBuffer.desc : &destination->desc,
                                           dstAddress + (bounceDst ? 0 : work->access[1].offset), &dst);
    if (chromaSite)
        dst.chromaSite = chromaSite;
    if (!why && plan->readsSource) {
        const uint64_t address = switch_vic_mapping_address(bounceSrc ? sourceBuffer.mapping : work->maps[0]);
        why = work->native.owner ? switch_native_vic_image(work->native, address, &src)
                                 : tegra_vic_image_from(bounceSrc ? &sourceBuffer.desc : &source->desc,
                                                        address + (bounceSrc ? 0 : sourceAccess.offset), &src);
    }
    for (uint32_t i = 0; !why && i < plan->count; ++i) {
        auto operation = plan->op[i];
        if (!operation.fill && work->native.owner && !switch_native_vic_crop(work->native, &operation)) {
            why = "native source crop";
            break;
        }
        why = tegra_vic_build(operation.fill ? nullptr : &src, &dst, &operation, traits, tegra_vic_config_address(),
                              tegra_vic_filter_address(), &work->jobs[i]);
    }
    if (why) {
        release(work);
        return -22;
    }
    switch_gpu_cpu_lease *leases[2] = {};
    switch_gpu_backing_guard guards[4] = {work->access[1].guard};
    uint32_t leaseCount = 0, guardCount = 1;
    for (auto &access : work->access)
        if (access.owned)
            leases[leaseCount++] = access.owned;
    if (plan->readsSource && !alias && !work->native.owner)
        guards[guardCount++] = work->access[0].guard;
    if (work->native.owner) {
        guards[guardCount++] = work->native.guard;
        guards[guardCount++] = work->native.representation;
    }
    switch_gpu_event_submission dependencies = {};
    switch_vic_events_submission(events, false, &dependencies);
    if (bounceSrc)
        error = copy(original(*source, sourceAccess), scratch(sourceBuffer), work, dependencies);
    if (!error && bounceDst && !switch_vic_overwrites(*plan, destination->desc))
        error = copy(original(*destination, work->access[1]), scratch(destinationBuffer), work, dependencies);
    switch_gpu_backing_use uses[5] = {{output, SWITCH_GPU_BACKING_WRITE, 0}};
    uint32_t useCount = 1;
    if (plan->readsSource)
        uses[useCount++] = {input, SWITCH_GPU_BACKING_READ, 0};
    if (work->native.owner)
        uses[useCount++] = {work->native.logical, SWITCH_GPU_BACKING_READ, 0};
    // Representation guards are evaluated with the logical allocations locked, even when scratch carries the bytes.
    if (bounceDst)
        uses[useCount++] = {work->access[1].backing, SWITCH_GPU_BACKING_READ, 0};
    if (bounceSrc && !alias)
        uses[useCount++] = {sourceAccess.backing, SWITCH_GPU_BACKING_READ, 0};
    const SwitchVICOwnership ownership = {uses, useCount, guards, guardCount, leases, leaseCount};
    SwitchVICEvents *stage = nullptr;
    if (!error && bounceDst)
        error = switch_vic_events_stage(events, &stage);
    if (!error) {
        __atomic_add_fetch(&work->refs, 1u, __ATOMIC_RELAXED);
        error = switch_vic_submit_checked(work->jobs, plan->count, nullptr, ownership, completed, work, nullptr,
                                          bounceDst ? stage : events);
        if (error)
            release(work);
        else
            work->accepted = true;
    }
    switch_vic_events_release(&stage);
    if (!error && bounceDst) {
        switch_gpu_event_submission final = {};
        switch_vic_events_submission(events, true, &final);
        error = copy(scratch(destinationBuffer), original(*destination, work->access[1]), work, final);
        if (!error)
            switch_vic_events_commit(events);
    }
    if (error) {
        int expected = 0;
        __atomic_compare_exchange_n(&work->status, &expected, error, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    }
    const bool accepted = work->accepted;
    for (auto &access : work->access)
        switch_vic_surface_unreserve(&access);
    release(work);
    return accepted ? 1 : error;
}
