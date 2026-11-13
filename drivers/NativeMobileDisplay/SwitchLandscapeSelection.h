// SPDX-License-Identifier: BSD-2-Clause-Patent
// planeDecide's landscape composition (SwitchLandscape.h), in the class body after SwitchNativePlaneDisplay.h: a lone
// detached layer whose destination TegraVIC recorded as the decoder's picture turned (SwitchVICTurn.h) is scanned by
// window A turned, behind the UI on B, once V3 has passed (SwitchDisplayTraits columnScanYuv). Window A reads through
// the display's SMMU domain, so direct scanout's attach is required; landscape stays off once it failed this boot.

bool planeLandscapeCandidate(const native_planes::Roles &roles) const
{
    return switch_display_traits().columnScanYuv && roles.plane >= 0 && roles.second < 0 &&
           native_planes::isYuv(planeLayers[roles.plane].layer.kind) && landscapeReady() && !planeRecovering &&
           !scanoutStopped && !hwSuspended;
}

// Window A could scan a turned picture now: planes on, window A attached to the display's SMMU domain, landscape not
// turned off this boot.
bool landscapeReady() const
{
    return planesReady() && directAttached && !landscapeState.retired;
}

// The slot window A scans the picture from (image filled), -95 when the layer is not a turned picture window A can
// take, or planeNativeSlotFor's errors.
int planeLandscapeSlotFor(OSObject *surface, const native_planes::Layer &layer, uint32_t width, uint32_t height,
                          native_planes::Image &image)
{
    SwitchNativeScanout scanout = {};
    int error = switch_native_scanout_acquire(surface, &scanout, true);
    const auto &view = scanout.view;
    if (!error &&
        (!view.turn.transform ||
         !switch_landscape::turnedVideoImage(
             layer, view.layout, view.crop.x, view.crop.y, view.crop.width, view.crop.height, view.turn.transform,
             {int32_t(view.turn.x), int32_t(view.turn.y), int32_t(view.turn.width), int32_t(view.turn.height)}, width,
             height, image)))
        error = -95;
    // Window A's CSC is live while it scans a video: another matrix waits for the next entry.
    if (!error && landscapeOn() &&
        (landscapeState.csc[0] != int(image.matrix) || landscapeState.csc[1] != (image.fullRange ? 1 : 0)))
        error = -95;
    if (error) {
        switch_native_scanout_release(&scanout);
        return error == -2 ? -95 : error;
    }
    const int slot = planeMapSnapshot(scanout, image);
    if (slot >= 0 && stats)
        ++stPlaneNative;
    return slot;
}

// planeDecide, before the overlays are placed: true when the swap's composition is the landscape one (or must wait).
bool planeLandscapeDecide(const native_planes::Roles &roles, uint32_t width, uint32_t height, PlaneDecision &d)
{
    if (!planeLandscapeCandidate(roles))
        return false;
    const auto &layer = planeLayers[roles.plane];
    switch_display::Overlay video = {};
    const int slot = planeLandscapeSlotFor(layer.surface, layer.layer, width, height, video.image);
    if (planeRetry(slot)) {
        d.waitSlot = true;
        return true;
    }
    if (slot < 0)
        return false;
    video.on = true;
    video.slot = slot;
    video.depth = switch_landscape::kVideoOnA.layer & 0xff;
    PlaneSet want = {};
    want.landscape = true;
    want.window[0] = video;
    d.planeChange = !planeSame(want, planeTarget);
    d.set = want;
    return true;
}
