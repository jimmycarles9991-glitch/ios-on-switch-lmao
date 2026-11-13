// SPDX-License-Identifier: BSD-2-Clause-Patent
int planeNativeSlotFor(OSObject *surface, const native_planes::Layer &layer, uint32_t width, uint32_t height,
                       native_planes::Image &image, bool &resolved)
{
    SwitchNativeScanout scanout = {};
    int error = switch_native_scanout_acquire(surface, &scanout);
    resolved = true;
    // Only a surface with no native picture, one the window cannot take, or a retired one falls back to its pitch
    // representation; a busy one (-16) keeps the frame on screen rather than read the representation being replaced.
    if (error && error != -2 && error != -95 && error != SWITCH_GPU_BACKING_STALE) {
        switch_native_scanout_release(&scanout);
        return error;
    }
    const bool native = !error && switch_native_plane_image(layer, width, height, scanout.view.layout,
                                                            scanout.view.crop.x, scanout.view.crop.y, image);
    if (!native) {
        switch_native_scanout_release(&scanout);
        error = switch_native_scanout_pitch(surface, &scanout);
        tgm_layout block = {};
        if (!error && !native_planes::imageOf(layer, width, height, image, surfaceLayout(surface, block)))
            error = -95;
        if (!error && scanout.pitchOffset) {
            const uint64_t offset = scanout.pitchOffset;
            if (offset > UINT64_MAX - image.readHigh - 4095) {
                error = -22;
            } else {
                image.start[0] += offset;
                image.start[1] += offset;
                image.readLow = (image.readLow + offset) & ~4095ULL;
                image.readHigh = (image.readHigh + offset + 4095) & ~4095ULL;
            }
        }
    }
    if (error) {
        switch_native_scanout_release(&scanout);
        return error;
    }
    const int slot = planeUseNoted(planeMapSnapshot(scanout, image), surface);
    if (native && slot >= 0 && stats)
        ++stPlaneNative;
    return slot;
}

int planeMapSnapshot(SwitchNativeScanout &scanout, const native_planes::Image &image, int avoid = -1)
{
    const auto &allocation = scanout.allocation;
    if (!image.visible) {
        switch_native_scanout_release(&scanout);
        return -2;
    }
    if (!allocation.pages || image.readHigh <= image.readLow || image.readHigh - image.readLow > kPlaneSlotSpan ||
        image.readHigh > (uint64_t(allocation.page_count) << 12)) {
        switch_native_scanout_release(&scanout);
        return -95;
    }
    int victim = -1, mapped = -1;
    for (unsigned k = 0; k < kPlaneSlots; ++k) {
        if (int(k) == avoid)
            continue;
        auto &slot = planeSlots[k];
        const bool samePages = slot.native && slot.mappedAllocation == allocation.allocation_id &&
                               slot.mappedGeneration == allocation.allocation_generation && slot.low == image.readLow &&
                               slot.high == image.readHigh;
        if (samePages && slot.native->lease &&
            slot.native->allocation.content_generation == allocation.content_generation) {
            slot.used = ++planeSlotUses;
            switch_native_scanout_release(&scanout);
            return int(k);
        }
        const bool busy = planeSlotBusy(int(k));
        if (samePages && !busy && mapped < 0)
            mapped = int(k);
        if (!busy && (victim < 0 || slot.used < planeSlots[victim].used))
            victim = int(k);
    }
    if (mapped >= 0) {
        auto &slot = planeSlots[mapped];
        planeUseRelease(unsigned(mapped));
        switch_native_scanout_deliver(slot.native);
        switch_native_scanout_release(slot.native);
        *slot.native = scanout;
        scanout = {};
        slot.tiled = image.surfaceKind != 0;
        slot.used = ++planeSlotUses;
        return mapped;
    }
    if (victim < 0) {
        switch_native_scanout_release(&scanout);
        return -11;
    }
    auto &slot = planeSlots[victim];
    if (!slot.native)
        slot.native = IONewZero(SwitchNativeScanout, 1);
    if (!slot.native) {
        switch_native_scanout_release(&scanout);
        return -1;
    }
    planeDropSlot(unsigned(victim));
    const uint32_t first = uint32_t(image.readLow >> 12), count = uint32_t((image.readHigh - image.readLow) >> 12);
    if (!probeDomain.map(planeSlotIova(victim), allocation.pages + first, count, false)) {
        probeDomain.unmap(planeSlotIova(victim), count);
        cleanDomain();
        switch_native_scanout_release(&scanout);
        return -1;
    }
    slot.mappedAllocation = allocation.allocation_id;
    slot.mappedGeneration = allocation.allocation_generation;
    *slot.native = scanout;
    scanout = {};
    slot.low = image.readLow;
    slot.high = image.readHigh;
    slot.tiled = image.surfaceKind != 0;
    slot.used = ++planeSlotUses;
    cleanDomain();
    if (stats)
        ++stPlaneMaps;
    return victim;
}

// What the overlay's scanout is ordered after: its picture's (switch_native_scanout_dependencies), then its surface's
// writer promises (SwitchGpuWriterPromise.h), which the set's GPU fence acquires.
static uint32_t planeOverlayDependencies(const SwitchNativeScanout &native, switch_gpu_backing_token *tokens)
{
    const uint32_t count = switch_native_scanout_dependencies(native, tokens, kPlaneOverlayDependencies);
    const int promised = native.view.native
                             ? switch_gpu_backing_promise_tokens(native.view.native, 0, 0, tokens + count,
                                                                 kPlaneOverlayDependencies - count)
                             : 0;
    return count + (promised > 0 ? uint32_t(promised) : 0u);
}

// A picture's producer, a recorded turn's request wait or a writer promise not complete yet: the set goes out once the
// GPU has seen them complete (planeFence), never before.
static bool planeOverlayWaits(const SwitchNativeScanout &native)
{
    switch_gpu_backing_token tokens[kPlaneOverlayDependencies];
    const uint32_t count = planeOverlayDependencies(native, tokens);
    for (uint32_t i = 0; i < count; ++i)
        if (switch_gpu_backing_token_external_status(&tokens[i]) != 1)
            return true;
    return false;
}

unsigned planeDependencies(const PlaneSet &set, switch_gpu_backing_token tokens[kPlaneDependencies]) const
{
    unsigned count = 0;
    for (const auto &overlay : set.window) {
        if (!overlay.on)
            continue;
        const auto *native = planeSlots[overlay.slot].native;
        if (native && native->lease)
            count += planeOverlayDependencies(*native, tokens + count);
    }
    return count;
}

// A set's frames are on the panel (or will never be): recorded turns' request signals go.
void planeDeliver(const PlaneSet &set)
{
    for (const auto &overlay : set.window)
        if (overlay.on && planeSlots[overlay.slot].native)
            switch_native_scanout_deliver(planeSlots[overlay.slot].native);
}

void planeNextDependencies(TegraScanoutCopy &copy, switch_gpu_backing_token tokens[kPlaneDependencies]) const
{
    if (!planeNextSet)
        return;
    copy.dependencyCount = planeDependencies(planeNext, tokens);
    copy.dependencies = copy.dependencyCount ? tokens : nullptr;
}
