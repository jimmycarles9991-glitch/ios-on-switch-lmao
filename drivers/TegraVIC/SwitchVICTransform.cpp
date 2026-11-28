// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchVICTransform.h"
#include "SwitchVICMapping.h"
#include "SwitchVICJobs.h"
#include "SwitchVICSurfaceAccess.h"
#include "SwitchVICTiledDst.h"
#include "SwitchNativeVICImage.h"
#include "TegraVICBounce.h"
#include "../TegraGM20B/TegraGM20BLayout.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

namespace {
struct SwitchVICTransform {
    uint32_t refs = 1;
    SwitchVICSurfaceAccess access[2];
    SwitchNativeSurfaceView native = {};
    SwitchVICMapping *mappings[2] = {};
    void (*completion)(void *, int) = nullptr;
    void *context = nullptr;
};

void release(SwitchVICTransform *work)
{
    if (__atomic_sub_fetch(&work->refs, 1, __ATOMIC_ACQ_REL))
        return;
    for (uint32_t i = 0; i < 2; ++i) {
        switch_vic_mapping_release(work->mappings[i]);
        switch_vic_surface_release_access(&work->access[i]);
    }
    switch_native_surface_release(&work->native);
    IODelete(work, SwitchVICTransform, 1);
}

void completed(void *context, int status)
{
    auto *work = static_cast<SwitchVICTransform *>(context);
    const auto completion = work->completion;
    void *owner = work->context;
    completion(owner, status);
    release(work);
}
}  // namespace

bool switch_vic_pitch_backing(switch_gpu_backing *backing, uint64_t extent)
{
    switch_gpu_backing_info info = {};
    switch_gpu_backing_describe(backing, &info);
    if (extent > static_cast<uint64_t>(info.page_count) * 4096)
        return false;
    tgm_layout legacy = {};
    if (!info.layout.plane_count && info.page_count && tegra_gm20b_surface_layout(info.pages[0], &legacy) == 1)
        return false;
    for (uint32_t p = 0; p < info.layout.plane_count; ++p) {
        if (info.layout.planes[p].layout != SWITCH_GPU_BACKING_PITCH)
            return false;
    }
    return true;
}

int switch_vic_transform(const TegraVICSurface *source, const TegraVICSurface *destination, const TegraVICPlan *plan,
                         uint32_t chromaSite, void (*completion)(void *, int), void *context, SwitchVICEvents *events,
                         const TegraVICAcquire *acquire)
{
    return switch_vic_transform(source, destination, plan, chromaSite, completion, context, events, acquire, nullptr);
}

int switch_vic_transform(const TegraVICSurface *source, const TegraVICSurface *destination, const TegraVICPlan *plan,
                         uint32_t chromaSite, void (*completion)(void *, int), void *context, SwitchVICEvents *events,
                         const TegraVICAcquire *acquire, SwitchVICWriteReservation *dstReserve)
{
    if (!completion || !destination || !plan || !plan->count || plan->count > 3 || (plan->readsSource && !source))
        return -22;
    if (plan->readsSource && source->memory == destination->memory)
        return 0;
    auto *work = IONewZero(SwitchVICTransform, 1);
    if (!work)
        return -12;
    work->refs = 1;
    work->completion = completion;
    work->context = context;
    const TegraVICTraits *traits = tegra_vic_traits();
    int error = switch_vic_surface_access_destination(destination, &work->access[1], dstReserve);
    SwitchVICTiledDst tiledDst = {};
    bool directTiledDst = false;
    if (!error) {
        auto *output = work->access[1].backing;
        const uint64_t destinationExtent = tegra_vic_surface_extent(&destination->desc);
        directTiledDst = switch_vic_tiled_dst(destination, output, work->access[1].offset, plan, traits, &tiledDst);
        if (!directTiledDst &&
            (destinationExtent > UINT64_MAX - work->access[1].offset ||
             !switch_vic_pitch_backing(output, work->access[1].offset + destinationExtent) ||
             tegra_vic_bounce_needed(&destination->desc, tegra_vic_pitch_align(traits, false)) != 1)) {
            release(work);
            return 0;
        }
    }
    if (!error && plan->readsSource) {
        const int native = switch_native_surface_acquire(source, &work->native, false);
        if (native == -2 || native == -95 || native == SWITCH_GPU_BACKING_STALE) {
            if (tegra_vic_bounce_needed(&source->desc, tegra_vic_pitch_align(traits, true)) != 1) {
                release(work);
                return 0;
            }
            error = switch_vic_surface_access(source, false, &work->access[0], work->access[1].owned != nullptr);
        } else {
            error = native;
        }
    }
    if (error) {
        release(work);
        return error == -16 ? 0 : error;
    }
    auto *input = work->native.owner ? work->native.native : work->access[0].backing;
    auto *output = work->access[1].backing;
    const uint64_t sourceExtent = plan->readsSource ? tegra_vic_surface_extent(&source->desc) : 0;
    if ((plan->readsSource && !work->native.owner &&
         (sourceExtent > UINT64_MAX - work->access[0].offset ||
          !switch_vic_pitch_backing(input, work->access[0].offset + sourceExtent))) ||
        (plan->readsSource && (input == output || work->native.logical == output))) {
        release(work);
        return 0;
    }
    work->mappings[1] = switch_vic_mapping_acquire(output, true);
    if (plan->readsSource)
        work->mappings[0] = switch_vic_mapping_acquire(input, false);
    if (!work->mappings[1] || (plan->readsSource && !work->mappings[0])) {
        release(work);
        return -12;
    }
    TegraVICImage src = {}, dst = {};
    const uint64_t dstBase = switch_vic_mapping_address(work->mappings[1]);
    const char *why = directTiledDst ? switch_vic_tiled_image(destination, tiledDst, dstBase, &dst)
                                     : tegra_vic_image_from(&destination->desc, dstBase + work->access[1].offset, &dst);
    if (chromaSite)
        dst.chromaSite = chromaSite;
    if (!why && plan->readsSource) {
        const uint64_t address = switch_vic_mapping_address(work->mappings[0]);
        why = work->native.owner ? switch_native_vic_image(work->native, address, &src)
                                 : tegra_vic_image_from(&source->desc, address + work->access[0].offset, &src);
    }
    SwitchVICStagedJob staged[3] = {};
    uint8_t stagedSlots[3] = {};
    uint8_t *stagedVas[3] = {};
    bool useStaged = false;
    TegraVICJob *fallbackJobs = nullptr;
    if (!why) {
        useStaged = switch_vic_config_reserve(plan->count, stagedSlots, stagedVas);
        if (!useStaged) {
            fallbackJobs = IONew(TegraVICJob, plan->count);
            if (!fallbackJobs) {
                release(work);
                return -12;
            }
        }
        for (uint32_t i = 0; !why && i < plan->count; ++i) {
            auto operation = plan->op[i];
            if (!operation.fill && work->native.owner && !switch_native_vic_crop(work->native, &operation)) {
                why = "native source crop";
                break;
            }
            if (useStaged) {
                staged[i].slot = stagedSlots[i];
                why = tegra_vic_build_into(operation.fill ? nullptr : &src, &dst, &operation, tegra_vic_traits(),
                                           tegra_vic_config_address(), tegra_vic_filter_address(), stagedVas[i],
                                           &staged[i].methods);
                if (!why)
                    switch_dcache_clean_lines(stagedVas[i], TEGRA_VIC_CONFIG_BYTES);
            } else {
                why = tegra_vic_build(operation.fill ? nullptr : &src, &dst, &operation, tegra_vic_traits(),
                                      tegra_vic_config_address(), tegra_vic_filter_address(), &fallbackJobs[i]);
            }
        }
        if (!why && useStaged)
            switch_dcache_sync();
    }
    if (why) {
        if (useStaged)
            switch_vic_config_release(stagedSlots, plan->count);
        if (fallbackJobs)
            IODelete(fallbackJobs, TegraVICJob, plan->count);
        release(work);
        return -22;
    }
    // A native source is read from the decoder's picture; the surface's own backing is listed only to order against
    // it. A CPU reader materializing that backing's pitch rows from the same picture holds it as a writer until VIC
    // has copied them, which must not refuse this read (the guards still refuse a representation the CPU replaced).
    switch_gpu_backing_use uses[3] = {
        {output, SWITCH_GPU_BACKING_WRITE, 0},
        {input, SWITCH_GPU_BACKING_READ, 0},
        {work->native.logical, SWITCH_GPU_BACKING_READ | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, 0}};
    switch_gpu_backing_guard guards[3] = {work->access[1].guard};
    uint32_t guardCount = 1;
    if (work->native.owner) {
        guards[guardCount++] = work->native.guard;
        guards[guardCount++] = work->native.representation;
    } else if (plan->readsSource) {
        guards[guardCount++] = work->access[0].guard;
    }
    switch_gpu_cpu_lease *leases[3] = {};
    uint32_t leaseCount = 0;
    for (auto &access : work->access)
        if (access.owned)
            leases[leaseCount++] = access.owned;
    if (dstReserve && dstReserve->lease && dstReserve->backing == output && !work->access[1].owned)
        leases[leaseCount++] = dstReserve->lease;
    const SwitchVICOwnership ownership = {
        uses, work->native.owner ? 3u : plan->readsSource ? 2u : 1u, guards, guardCount, leases, leaseCount};
    __atomic_add_fetch(&work->refs, 1, __ATOMIC_RELAXED);
    if (useStaged) {
        error = switch_vic_submit_staged(staged, plan->count, acquire, ownership, completed, work, nullptr, events);
        if (error)
            switch_vic_config_release(stagedSlots, plan->count);
    } else {
        error =
            switch_vic_submit_checked(fallbackJobs, plan->count, acquire, ownership, completed, work, nullptr, events);
        IODelete(fallbackJobs, TegraVICJob, plan->count);
    }
    for (auto &access : work->access)
        switch_vic_surface_unreserve(&access);
    if (dstReserve)
        switch_vic_surface_release_write(dstReserve);
    if (error)
        release(work);
    release(work);
    return error ? error : 1;
}
