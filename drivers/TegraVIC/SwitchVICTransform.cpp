// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchVICTransform.h"
#include "SwitchVICMapping.h"
#include "SwitchVICJobs.h"
#include "SwitchVICSurfaceAccess.h"
#include "SwitchNativeVICImage.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

namespace {
struct SwitchVICTransform {
    uint32_t refs = 1;
    SwitchVICSurfaceAccess access[2];
    SwitchNativeSurfaceView native = {};
    SwitchVICMapping *mappings[2] = {};
    TegraVICJob jobs[3] = {};
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
                         uint32_t chromaSite, void (*completion)(void *, int), void *context, SwitchVICEvents *events)
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
    int error = switch_vic_surface_access(destination, true, &work->access[1]);
    if (!error && plan->readsSource) {
        const int native = switch_native_surface_acquire(source, &work->native, false);
        if (native == -2 || native == -95 || native == SWITCH_GPU_BACKING_STALE)
            error = switch_vic_surface_access(source, false, &work->access[0], work->access[1].owned != nullptr);
        else
            error = native;
    }
    if (error) {
        release(work);
        return error == -16 ? 0 : error;
    }
    auto *input = work->native.owner ? work->native.native : work->access[0].backing;
    auto *output = work->access[1].backing;
    const uint64_t destinationExtent = tegra_vic_surface_extent(&destination->desc);
    const uint64_t sourceExtent = plan->readsSource ? tegra_vic_surface_extent(&source->desc) : 0;
    if (destinationExtent > UINT64_MAX - work->access[1].offset ||
        !switch_vic_pitch_backing(output, work->access[1].offset + destinationExtent) ||
        (plan->readsSource && !work->native.owner &&
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
    const char *why = tegra_vic_image_from(
        &destination->desc, switch_vic_mapping_address(work->mappings[1]) + work->access[1].offset, &dst);
    if (chromaSite)
        dst.chromaSite = chromaSite;
    if (!why && plan->readsSource) {
        const uint64_t address = switch_vic_mapping_address(work->mappings[0]);
        why = work->native.owner ? switch_native_vic_image(work->native, address, &src)
                                 : tegra_vic_image_from(&source->desc, address + work->access[0].offset, &src);
    }
    for (uint32_t i = 0; !why && i < plan->count; ++i) {
        auto operation = plan->op[i];
        if (!operation.fill && work->native.owner && !switch_native_vic_crop(work->native, &operation)) {
            why = "native source crop";
            break;
        }
        why = tegra_vic_build(operation.fill ? nullptr : &src, &dst, &operation, tegra_vic_traits(),
                              tegra_vic_config_address(), tegra_vic_filter_address(), &work->jobs[i]);
    }
    if (why) {
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
    switch_gpu_cpu_lease *leases[2] = {};
    uint32_t leaseCount = 0;
    for (auto &access : work->access)
        if (access.owned)
            leases[leaseCount++] = access.owned;
    const SwitchVICOwnership ownership = {
        uses, work->native.owner ? 3u : plan->readsSource ? 2u : 1u, guards, guardCount, leases, leaseCount};
    __atomic_add_fetch(&work->refs, 1, __ATOMIC_RELAXED);
    error = switch_vic_submit_checked(work->jobs, plan->count, nullptr, ownership, completed, work, nullptr, events);
    for (auto &access : work->access)
        switch_vic_surface_unreserve(&access);
    if (error)
        release(work);
    release(work);
    return error ? error : 1;
}
