// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOLib.h>
#include "SwitchVICTurn.h"
#include "TegraVIC.h"
#include "SwitchVICJobs.h"
#include "SwitchVICMapping.h"
#include "SwitchNativeVICImage.h"
#include "../NativeMobileDisplay/SwitchLandscapeRunABI.h"

namespace {
#if TEGRA_VIC_STATS
uint32_t gForcedTraits;
const void *gForcedBy;
#endif

struct Turn {
    uint32_t refs = 1;
    switch_gpu_backing *source = nullptr, *destination = nullptr;
    SwitchVICMapping *maps[2] = {};
    TegraVICJob job = {};
};

void releaseTurn(Turn *turn)
{
    if (__atomic_sub_fetch(&turn->refs, 1, __ATOMIC_ACQ_REL))
        return;
    for (auto *map : turn->maps)
        switch_vic_mapping_release(map);
    if (turn->source)
        switch_gpu_backing_release(turn->source);
    if (turn->destination)
        switch_gpu_backing_release(turn->destination);
    IODelete(turn, Turn, 1);
}

void turnCompleted(void *context, int)
{
    releaseTurn(static_cast<Turn *>(context));
}

const char *buildTurn(const SwitchNativeSurfaceView &source, const TegraVICOp &op, uint32_t chromaSite,
                      const TegraVICSurface *destination, uint64_t sourceAddress, uint64_t destinationAddress,
                      TegraVICJob *job)
{
    TegraVICImage src = {}, dst = {};
    const char *why = switch_native_vic_image(source, sourceAddress, &src);
    if (!why)
        why = tegra_vic_image_from(&destination->desc, destinationAddress, &dst);
    TegraVICOp operation = op;
    if (!why && !switch_native_vic_crop(source, &operation))
        why = "native source crop";
    if (why)
        return why;
    if (chromaSite)
        dst.chromaSite = chromaSite;
    return tegra_vic_build(&src, &dst, &operation, tegra_vic_traits(), tegra_vic_config_address(),
                           tegra_vic_filter_address(), job);
}
}  // namespace

int switch_vic_turn_surface(const SwitchNativeSurfaceView &source, const TegraVICOp &op, uint32_t chromaSite,
                            const TegraVICSurface *destination, switch_gpu_cpu_lease *owned, bool queue,
                            const char **why, SwitchVICEvents *events)
{
    *why = "a turn without its picture, destination or write lease";
    if (!source.native || !destination || destination->baseOffset || !owned)
        return -22;
    auto *turn = IONewZero(Turn, 1);
    if (!turn)
        return -12;
    turn->refs = 1;
    turn->source = source.native;
    switch_gpu_backing_retain(turn->source);
    turn->destination = switch_vic_surface_backing(destination);
    if (turn->destination) {
        turn->maps[0] = switch_vic_mapping_acquire(turn->source, false);
        turn->maps[1] = switch_vic_mapping_acquire(turn->destination, true);
    }
    *why = "the turn's mappings could not be retained";
    if (!turn->maps[0] || !turn->maps[1]) {
        releaseTurn(turn);
        return -12;
    }
    if ((*why = buildTurn(source, op, chromaSite, destination, switch_vic_mapping_address(turn->maps[0]),
                          switch_vic_mapping_address(turn->maps[1]), &turn->job))) {
        releaseTurn(turn);
        return -95;
    }
    switch_gpu_backing_use uses[] = {{turn->source, SWITCH_GPU_BACKING_READ, 0},
                                     {turn->destination, SWITCH_GPU_BACKING_WRITE, 0}};
    const SwitchVICOwnership ownership = {uses, 2, nullptr, 0, &owned, 1};
    uint32_t fence = 0;
    __atomic_add_fetch(&turn->refs, 1, __ATOMIC_RELAXED);
    int error = switch_vic_submit_checked(&turn->job, 1, nullptr, ownership, turnCompleted, turn, &fence, events);
    if (error)
        releaseTurn(turn);
    releaseTurn(turn);
    if (!error && !queue)
        error = switch_vic_wait(fence);
    *why = error ? "the turn's VIC job failed" : nullptr;
    return error;
}

int switch_vic_turn_defer(const TegraVICSurface *source, const TegraVICSurface *destination, const TegraVICPlan *plan,
                          uint32_t chromaSite, SwitchVICEvents **events, const SwitchDisplayTraits &traits)
{
    if (!traits.columnScanYuv || !source || !destination || !plan || plan->count != 1 || !plan->readsSource ||
        source->memory == destination->memory)
        return 0;
    SwitchNativeSurfaceView picture = {};
    if (switch_native_surface_acquire(source, &picture, false))
        return 0;
    // The job a reader's rows will take is built now, against the windows the transform path checks requests in, so
    // a turn VIC would refuse is copied now instead of failing a later reader.
    SwitchNativeSurfaceTurn turn = {};
    TegraVICJob check = {};
    const char *why = switch_vic_turn_of(plan->op[0], destination->desc, picture.crop, turn);
    if (!why)
        why = buildTurn(picture, plan->op[0], chromaSite, destination, kTegraVICWindowBase,
                        kTegraVICWindowBase + TEGRA_VIC_WINDOW_DST * kTegraVICWindowBytes, &check);
    SwitchVICEvents *taken = events ? *events : nullptr;
    const int error =
        why ? -95 : switch_native_surface_publish_turned(destination, picture, plan->op[0], chromaSite, nullptr, taken);
    switch_native_surface_release(&picture);
    if (error)
        return 0;
    if (events)
        *events = nullptr;
    return 1;
}

#if TEGRA_VIC_STATS
extern "C" SwitchDisplayTraits switch_display_traits(void)
{
    SwitchDisplayTraits traits = kSwitchDisplayTraits;
    const uint32_t forced = __atomic_load_n(&gForcedTraits, __ATOMIC_ACQUIRE);
    traits.overlayBlockYuv = traits.overlayBlockYuv || (forced & SWITCH_DISPLAY_TRAIT_OVERLAY_BLOCK_YUV);
    traits.columnScanYuv = traits.columnScanYuv || (forced & SWITCH_DISPLAY_TRAIT_COLUMN_SCAN_YUV);
    traits.offsetCrop = traits.offsetCrop || (forced & SWITCH_DISPLAY_TRAIT_OFFSET_CROP);
    return traits;
}

void switch_display_traits_force(uint32_t traits, const void *client)
{
    __atomic_store_n(&gForcedBy, client, __ATOMIC_RELEASE);
    __atomic_store_n(&gForcedTraits, traits, __ATOMIC_RELEASE);
}

void switch_display_traits_release(const void *client)
{
    const void *expected = client;
    if (__atomic_compare_exchange_n(&gForcedBy, &expected, nullptr, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        __atomic_store_n(&gForcedTraits, 0u, __ATOMIC_RELEASE);
}
#endif
