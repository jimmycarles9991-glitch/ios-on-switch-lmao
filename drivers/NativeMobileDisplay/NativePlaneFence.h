// SPDX-License-Identifier: BSD-2-Clause-Patent
// When a window B change may reach the registers. Window B scans a surface in place, so a surface the GPU renders
// (block-linear, NativePlanes.h blockReads) is written only once the work its writers had submitted is done, as window A's
// direct slots are: a GPU present with no copy (TegraScanoutCopy.h) waits for it on the GPU and its completion interrupt
// writes the registers. Window B's change rides on the present of the window A frame it goes out with (the present also
// waits for the plane surface's writers), or on a present of its own when the swap changes window B alone. A frame no
// present carries (the CPU copy) has nothing that orders it after the GPU's writes, so such a change is not shown; the
// next swap brings it again. Surfaces nothing on the GPU writes (VIC's video frames, CPU-drawn images) go out as before.
#pragma once

namespace native_planes {

enum class PlaneRoute {
    kNow,        // written by this kick (or with a window A frame still held for a flip)
    kWithFrame,  // with the swap's window A frame, when that frame goes out
    kFence,      // at the completion of a present of its own
    kNotShown,   // dropped: counted, window B keeps what it shows
};

// fenced: the new set's surface is the GPU's; uiNew: the swap has a window A frame; framePresent: that frame goes through a
// GPU present.
constexpr PlaneRoute planeRoute(bool fenced, bool uiNew, bool framePresent)
{
    if (!uiNew)
        return fenced ? PlaneRoute::kFence : PlaneRoute::kNow;
    return !fenced || framePresent ? PlaneRoute::kWithFrame : PlaneRoute::kNotShown;
}

}  // namespace native_planes
