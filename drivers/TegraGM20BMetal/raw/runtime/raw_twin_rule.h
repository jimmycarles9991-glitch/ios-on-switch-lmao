// SPDX-License-Identifier: MIT
// Rule fetch.twin: a storage form's draw taken by its barrier twin (raw_fetch_rungs.h), one triangle a barrier, where
// gpu.raw.blend_twin measured the twin cheaper than the pixel interlock. Boot 21, Core Animation's colour matrix state
// (its barrier twin), us per draw, storage against twin: 2 triangles over 64x64 12.9 / 9.0, 8 over 64x64 47.1 / 33.9,
// 32 over 64x64 182.8 / 132.4, 2 over 1280x720 2295.4 / 1305.5, every code the storage form's. Those quads covered the
// whole target; what a draw covers only its positions say, so the render area stands for it. A whole draw behind one
// barrier is never taken: its own triangles are not ordered and only their positions say whether they overlap.
#pragma once
#include <cstdint>
#include "raw_fetch_order.h"

namespace raw {

constexpr uint32_t kTwinTriangles = 32, kTwinAreaMin = 64 * 64, kTwinAreaMax = 1280 * 720;

struct TwinAsk {
    FetchDraw shape;
    bool primitiveId = false;  // the twin's fragments read the primitive ID
    uint64_t areaPixels = 0;   // the pass's render area
    bool amplified = false;    // drawn once per vertex amplification
};

// The shapes selfRuns draws a triangle at a time, of the triangle counts and areas the device measured. An indexed
// strip is not one: whether its faces decide its pixels (FetchDraw::facing) is known only once the draw is prepared,
// and a draw that took the twin there would be refused where the storage form draws it.
constexpr bool twinTaken(const TwinAsk &a)
{
    const FetchDraw &d = a.shape;
    if (a.amplified || (d.primitive != 3 && d.primitive != 4) || (d.indexed && d.primitive == 4) ||
        fetchRefusal(true, a.primitiveId, d) != FetchRefusal::None)
        return false;
    const uint32_t n = triangles(d);
    return n >= 1 && n <= kTwinTriangles && a.areaPixels >= kTwinAreaMin && a.areaPixels <= kTwinAreaMax;
}

}  // namespace raw
