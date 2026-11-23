// SPDX-License-Identifier: MIT
// The range a pass of Metal's draws bounds each depth it stores to, as an M4 Max stores Depth32Float (macOS 27.2.1, the
// pixel's depth after the bias): in clip mode 0 to 1, whatever the viewport's depth range, the bias or a fragment
// function's own depth; in clamp mode from the lower of the viewport's near and far, but at least 0, to the higher, which
// a fragment function's own depth meets at 1 at most. UNORM zetas hold no more than 0 to 1 either way. Before the bias an
// M4 Max also clamps each vertex's depth at 0 (in clamp mode to 0..1), which no GM20B register does (its clamps act on the
// biased pixel depth): in clamp mode the deepest depth it stores is then 1 plus the most the draw's bias adds there, which
// the range's top meets where the bias's most is known, so a triangle beyond 1 stores Metal's depth; a triangle whose
// depth leaves 0..1 across it, or a vertex below 0 under a bias, still differs.
#pragma once
#include <algorithm>
#include <cmath>

namespace gm20b {

struct DepthBounds {
    float lo = 0, hi = 1;
};

// The most a draw's depth bias adds at depth 1: its constant times the unit there (`unit`: 2^-23 of a float zeta's depth,
// 2^-24 of FORCE_UNORM's), within the bias clamp; 0 unbiased (points and lines, or no factors); NaN where a slope factor or
// a negative constant makes it a function of the triangle
inline float topDepthBias(bool biased, float constant, float slope, float clamp, float unit)
{
    if (!biased || (constant == 0 && slope == 0))
        return 0;
    if (slope != 0 || !(constant > 0))
        return NAN;
    const float most = constant * unit;
    return clamp > 0 ? std::min(most, clamp) : most;
}

inline DepthBounds metalDepthBounds(bool clamp, const float (&range)[2], bool fragmentDepth, float topBias = NAN)
{
    if (!clamp)
        return {};
    const float lo = std::max(std::min(range[0], range[1]), 0.0f), hi = std::max(range[0], range[1]);
    if (fragmentDepth)
        return {lo, std::max(lo, std::min(hi, 1.0f))};
    return {lo, topBias >= 0 ? std::max(lo, std::min(hi, 1.0f + topBias)) : hi};
}

}  // namespace gm20b
