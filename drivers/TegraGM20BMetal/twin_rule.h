// SPDX-License-Identifier: MIT
// Rule fetch.twin: a storage form's draw taken by its barrier twin (fetch_rungs.h), one triangle a barrier, where
// gpu.gm20b.blend_twin measured the twin cheaper than the pixel interlock. Boot 21, Core Animation's colour matrix state
// (its barrier twin), us per draw, storage against twin: 2 triangles over 64x64 12.9 / 9.0, 8 over 64x64 47.1 / 33.9,
// 32 over 64x64 182.8 / 132.4, 2 over 1280x720 2295.4 / 1305.5, every code the storage form's. Those quads covered the
// whole target; what a draw covers only its positions say, so the render area stands for it. That holds for a quad,
// which covers its layer, and over a small pass, but not for a mesh over a large one: there the interlock pays for few
// pixels while the twin pays a barrier and a texture invalidate a triangle. Boot 22's ui.glass_sidebar drew 8-10
// triangles a draw over 1280x720 as twins, 296.5 barriers a submit where boot 21 had 28.2 interlocked draws, and the
// compositor's GPU time a frame went from 4,285 to 6,343 us. So more than one quad takes the twin only over a pass no
// larger than 4 x 64x64, the largest area every multi-quad measurement covered. A whole draw behind one barrier is
// never taken: its own triangles are not ordered and only their positions say whether they overlap.
#pragma once
#include <cstdint>
#include "fetch_order.h"

namespace gm20b {

constexpr uint32_t kTwinTriangles = 32, kTwinQuadTriangles = 2, kTwinAreaMin = 64 * 64, kTwinMeshAreaMax = 4 * 64 * 64,
                   kTwinAreaMax = 1280 * 720;

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
    if (n < 1 || n > kTwinTriangles || a.areaPixels < kTwinAreaMin || a.areaPixels > kTwinAreaMax)
        return false;
    return n <= kTwinQuadTriangles || a.areaPixels <= kTwinMeshAreaMax;
}

}  // namespace gm20b
