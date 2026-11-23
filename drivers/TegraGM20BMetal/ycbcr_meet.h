// SPDX-License-Identifier: MIT
// A YCbCr target whose two planes' rows, as its stores land them, reach one another's bytes (YcbcrPlan::meet). An M4 Max's
// pass keeps such a byte by its frame's 2x2 quads across both planes, the OR of one quad's luma and chroma bytes
// (overlap_order.h overlapPlanesWriters). Each plane renders into its pool image (Texture::sampled), then the pack's meet
// state draws the span of both, a byte a texel, into the planes' rows.
#pragma once
#include <algorithm>

#include "gpu.h"
#include "overlap_rows.h"
#include "ycbcr_store.h"

namespace gm20b {

inline bool ycbcrPlanesMeet(const Texture &t, const YcbcrStoreShape &store)
{
    if (t.onePlane || !t.luma || !t.chroma || !t.luma->sampled || !t.chroma->sampled || store.packed || store.words ||
        store.plane)
        return false;
    Texture ys, cs;
    const Texture &y = storedRows(*t.luma, t, ys), &c = storedRows(*t.chroma, t, cs);
    const uint64_t yEnd = y.va + uint64_t(y.pitch) * (y.height - 1) + uint64_t(y.width) * y.bpp;
    const uint64_t cEnd = c.va + uint64_t(c.pitch) * (c.height - 1) + uint64_t(c.width) * c.bpp;
    return y.va < cEnd && c.va < yEnd;
}

// The meet state's buffer for `t`'s planes, their span from `lo` to `hi`: luma a code a texel, chroma a pair a block of the
// store's halving; `loaded`: a loaded pass's reach, whose unreached tiles keep their bytes
inline OverlapPlanes meetingPlanesOrder(const Texture &t, const TurnMask *loaded, uint64_t &lo, uint64_t &hi)
{
    Texture ys, cs;
    const Texture &y = storedRows(*t.luma, t, ys), &c = storedRows(*t.chroma, t, cs);
    lo = std::min(y.va, c.va);
    hi = std::max(y.va + uint64_t(y.pitch) * (y.height - 1) + uint64_t(y.width) * y.bpp,
                  c.va + uint64_t(c.pitch) * (c.height - 1) + uint64_t(c.width) * c.bpp);
    const YcbcrStoreShape &store = *t.ycbcrStore;
    OverlapPlanes s;
    s.pitch = y.pitch;
    s.plane[0] = meetPlane(uint32_t(y.va - lo), y.pitch, y.width * y.bpp, y.height, y.bpp, y.bpp);
    s.plane[1] = meetPlane(uint32_t(c.va - lo), c.pitch, c.width * c.bpp, c.height, c.bpp, c.bpp / 2u,
                           store.across ? 2 : 1, store.down ? 2 : 1);
    s.tiles = loaded ? loaded->tiles : frameTiles(t);
    s.masked = loaded ? 1 : 0;
    return s;
}

// 0, or 1 a target the driver cannot render or no meet state in the pack, 2 a draw the encoder refused (*why)
inline int storeMeetingPlanes(Device &d, CommandBuffer &rc, const Texture &t, const TurnMask *loaded, const char **why)
{
    Pipeline *state = d.ownPipeline(kOverlapMeetState);
    if (!state) {
        *why = "the pack has no state storing YCbCr planes whose rows reach one another's";
        return 1;
    }
    uint64_t lo = 0, hi = 0;
    const OverlapPlanes s = meetingPlanesOrder(t, loaded, lo, hi);
    return storePlanesSpan(rc, state, t, s, lo, hi, *t.luma->sampled, *t.chroma->sampled,
                           loaded ? *loaded->mask : *t.luma->sampled,
                           "YCbCr planes whose rows reach one another's the driver cannot render", why);
}

}  // namespace gm20b
