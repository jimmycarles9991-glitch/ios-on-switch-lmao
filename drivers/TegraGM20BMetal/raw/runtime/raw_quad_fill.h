// SPDX-License-Identifier: MIT
// Rule quad.bbox: an axis-aligned quad drawn as one triangle under SET_FILL_VIA_TRIANGLE FILL_BBOX (the triangle covers
// its screen-space bounding box, kits/SwitchGM20BEmit/SwitchGM20BEmitRaster.h), half the primitives of Core Animation's
// two-triangle quads. A draw qualifies when its index stream is the quad pattern with each quad's first triangle spanning
// the quad, and every quad is axis-aligned after its transform. Undocumented on GM20B (NVK's context init sets
// DISABLED): false until HW Test gpu.x1.fill_bbox's bbox arm fills exactly the quad its triangle spans.
#pragma once
#include <cstdint>

namespace raw {

constexpr bool fillViaTriangleBox()
{
    return false;
}

// One triangle of the quad at `corners` (x, y pairs in the order a triangle strip draws them: 0 1 2 3) spans the quad's
// bounding box when the quad is axis-aligned: corners 0 and 3 opposite, 1 and 2 on the other diagonal
constexpr bool axisAlignedQuad(const float corners[8])
{
    const bool rows = corners[1] == corners[3] && corners[5] == corners[7];
    const bool columns = corners[0] == corners[4] && corners[2] == corners[6];
    return rows && columns && corners[0] != corners[2] && corners[1] != corners[5];
}

}  // namespace raw
