// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include <libkern/c++/OSObject.h>
#include "TegraVIC.h"
#include "TegraVICPresent.h"
#include "TegraVICSurface.h"
#include "SwitchVICPicturePlan.h"
#include "SwitchVICMapping.h"
#include "SwitchVICJobs.h"
#include "SwitchVICSurfaceAccess.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"

namespace {
bool gPresentReady;

struct Present {
    uint32_t references;
    bool publishSeed;  // a queued transfer into a client's surface: its seed moves when VIC has written it
    TegraVICSurface surface = {};
    switch_gpu_backing *destination = nullptr;
    SwitchVICSurfaceAccess access;
    SwitchVICMapping *sourceMap = nullptr, *destinationMaps[2] = {};
    TegraVICJob job = {};
};

void release(Present *work)
{
    if (__atomic_sub_fetch(&work->references, 1, __ATOMIC_ACQ_REL))
        return;
    switch_vic_mapping_release(work->sourceMap);
    for (auto *mapping : work->destinationMaps)
        switch_vic_mapping_release(mapping);
    if (work->destination)
        switch_gpu_backing_release(work->destination);
    switch_vic_surface_release_access(&work->access);
    tegra_vic_surface_release(&work->surface);
    IODelete(work, Present, 1);
}

void completed(void *context, int status)
{
    auto *work = static_cast<Present *>(context);
    if (!status && work->publishSeed)
        tegra_vic_surface_written(&work->surface);
    release(work);
}

int place(const TegraVICPicture *picture, task_t task, Present *work, switch_gpu_cpu_lease *owned,
          const TegraVICSurface *surface, bool asynchronous, const char **why)
{
    if (!picture || !picture->backing ||
        (surface && (!surface->surface || !surface->memory || surface->id != picture->surfaceID))) {
        *why = "picture backing or retained surface is invalid";
        return -22;
    }
    if (surface) {
        work->surface = *surface;
        work->surface.prepared = false;
        work->surface.surface->retain();
    } else if (tegra_vic_surface_lookup(picture->surfaceID, task, &work->surface)) {
        *why = "picture backing or surface ID not found";
        return -22;
    }
    TegraVICPicture bytes = {};
    TegraVICSurfaceDesc desc = {};
    SwitchVICPlacement placement = {};
    *why = switch_vic_picture_bytes(picture, &work->surface.desc, &bytes, &desc);
    if (!*why)
        *why = switch_vic_picture_place(&bytes, &desc, &placement);
    if (*why)
        return -95;
    if (!owned) {
        const int error = switch_vic_surface_access(&work->surface, true, &work->access);
        if (error) {
            *why = "destination representation could not be acquired";
            return error;
        }
        work->destination = work->access.backing;
        switch_gpu_backing_retain(work->destination);
    } else {
        work->destination = switch_vic_surface_backing(&work->surface);
    }
    if (!work->destination) {
        *why = "the destination could not be retained";
        return -12;
    }
    if (work->access.offset) {
        switch_gpu_backing_info info = {};
        switch_gpu_backing_describe(work->destination, &info);
        desc.length = uint64_t(info.page_count) << 12;
        for (uint32_t plane = 0; plane < 2; ++plane) {
            if (desc.planeOffset[plane] > UINT64_MAX - work->access.offset) {
                *why = "destination plane address overflow";
                return -22;
            }
            desc.planeOffset[plane] += work->access.offset;
        }
        *why = switch_vic_picture_place(&bytes, &desc, &placement);
        if (*why)
            return -95;
    }
    const uint32_t planes = placement.whole.format->planes;
    work->sourceMap = switch_vic_mapping_acquire(picture->backing, false);
    for (uint32_t plane = 0; plane < planes; ++plane)
        work->destinationMaps[plane] = switch_vic_mapping_acquire_range(
            work->destination, true, static_cast<uint32_t>(placement.band[plane].start >> 12), placement.pages[plane]);
    if (!work->sourceMap || !work->destinationMaps[0] || (planes > 1 && !work->destinationMaps[1])) {
        *why = "the picture mappings could not be retained";
        return -12;
    }
    TegraVICImage source = {}, destination = {};
    TegraVICOp operation = {};
    switch_vic_picture_describe(&bytes, placement, switch_vic_mapping_address(work->sourceMap), 0, &source,
                                &destination, &operation);
    for (uint32_t plane = 0; plane < planes; ++plane)
        destination.base[plane] =
            switch_vic_mapping_address(work->destinationMaps[plane]) + (placement.band[plane].start & 4095);
    *why = tegra_vic_build(&source, &destination, &operation, tegra_vic_traits(), tegra_vic_config_address(),
                           tegra_vic_filter_address(), &work->job);
    if (*why)
        return -95;
    switch_gpu_backing_use uses[] = {{picture->backing, SWITCH_GPU_BACKING_READ, 0},
                                     {work->destination, SWITCH_GPU_BACKING_WRITE, 0}};
    uint32_t fence = 0;
    auto *lease = owned ? owned : work->access.owned;
    const SwitchVICOwnership ownership = {
        uses, 2, owned ? nullptr : &work->access.guard, owned ? 0u : 1u, lease ? &lease : nullptr, lease ? 1u : 0u};
    if (asynchronous)
        __atomic_add_fetch(&work->references, 1, __ATOMIC_RELAXED);
    work->publishSeed = asynchronous && !owned;
    int error = switch_vic_submit_checked(&work->job, 1, nullptr, ownership, asynchronous ? completed : nullptr,
                                          asynchronous ? work : nullptr, &fence);
    if (error && asynchronous)
        release(work);
    switch_vic_surface_unreserve(&work->access);
#if TEGRA_VIC_STATS
    if (!error)
        switch_vic_stats_present(!asynchronous);
#endif
    if (!error && !asynchronous)
        error = switch_vic_wait(fence);
    if (!error && !owned && !asynchronous)
        tegra_vic_surface_written(&work->surface);
    if (error)
        *why = "the queued picture transfer failed";
    return error;
}
}  // namespace

extern "C" bool tegra_vic_present_ready(void)
{
    const auto *traits = tegra_vic_traits();
    return __atomic_load_n(&gPresentReady, __ATOMIC_ACQUIRE) && tegra_vic_ready() && traits && traits->blockLinearIn;
}

extern "C" void tegra_vic_present_start(void)
{
    if (tegra_vic_traits()->blockLinearIn)
        __atomic_store_n(&gPresentReady, true, __ATOMIC_RELEASE);
}

static int present(const TegraVICPicture *picture, task_t task, switch_gpu_cpu_lease *owned,
                   const TegraVICSurface *surface, bool asynchronous, const char **why)
{
    if (!why)
        return -22;
    *why = nullptr;
    if (!tegra_vic_present_ready()) {
        *why = "VIC is not published";
        return -19;
    }
    auto *work = IONewZero(Present, 1);
    if (!work)
        return -12;
    work->references = 1;
    const int error = place(picture, task, work, owned, surface, asynchronous, why);
    release(work);
    if (error && *why && !owned)
        switch_refusal("TegraVIC", *why, "decoded picture not placed by VIC, the decoder copies it on the CPU");
    return error;
}

extern "C" int tegra_vic_present(const TegraVICPicture *picture, task_t task, const char **why)
{
    return present(picture, task, nullptr, nullptr, true, why);
}

extern "C" int switch_vic_present_owned(const TegraVICPicture *picture, task_t task, switch_gpu_cpu_lease *destination,
                                        const char **why)
{
    if (!destination) {
        if (why)
            *why = "materialization requires destination ownership";
        return -22;
    }
    return present(picture, task, destination, nullptr, false, why);
}

extern "C" int switch_vic_present_surface_owned(const TegraVICPicture *picture, const TegraVICSurface *surface,
                                                switch_gpu_cpu_lease *destination, const char **why)
{
    if (!destination || !surface) {
        if (why)
            *why = "materialization requires retained surface and destination ownership";
        return -22;
    }
    return present(picture, nullptr, destination, surface, false, why);
}

extern "C" int switch_vic_present_surface_queue(const TegraVICPicture *picture, const TegraVICSurface *surface,
                                                switch_gpu_cpu_lease *destination, const char **why)
{
    if (!destination || !surface) {
        if (why)
            *why = "materialization requires retained surface and destination ownership";
        return -22;
    }
    return present(picture, nullptr, destination, surface, true, why);
}
