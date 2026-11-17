// SPDX-License-Identifier: MIT
// setDepthBias:slopeScale:clamp: of infinite or NaN factors as finite polygon-offset factors. What an M4 Max stores
// (macOS 27.2.1, Depth32Float and Depth16Unorm, clip and clamp modes): a triangle's bias is constant x r + slope x m in
// IEEE arithmetic, a NaN constant counting as +inf; a clamp above 0 bounds it as fmin and one below 0 as fmax, infinite
// clamps included, a NaN clamp bounds nothing; an unbounded +inf or NaN bias puts the depth at the top of its clamp and
// -inf at the bottom. So each draw's depth is its own plus the clamp or saturated high or low, the same for every flat
// triangle (m 0) and for every sloped one (m above 0), which a saturating constant, and a slope term that outweighs it
// on any slope, give through SET_DEPTH_BIAS_CLAMP and the pixel clamp: the viewport range in clamp mode, 0 to 1 in clip
// mode (RenderEncoder::writeClipControl).
#pragma once
#include <cmath>

namespace raw {

struct DepthBiasFactors {
    float constant = 0, slope = 0, clamp = 0;
    // the constant counts SET_DEPTH_BIAS_CONTROL's FORCE_UNORM units (2^-24 of depth) on every zeta
    bool unorm24 = false;
};

// 2^16 of depth in 2^-24 units: past any depth clamp a viewport sets
constexpr float kSaturatingBias = 1099511627776.0f;
// x m outweighs 2^17 of depth on every slope from 2^-83 to 2^27 without overflowing
constexpr float kSlopeOutweighs = 1267650600228229401496703205376.0f;

inline DepthBiasFactors depthBiasFactors(float constant, float slope, float clamp)
{
    const float bounded = std::isfinite(clamp) ? clamp : 0.0f;
    if (std::isfinite(constant) && std::isfinite(slope))
        return {constant, slope, bounded, false};
    const float bound = std::isnan(clamp) ? 0.0f : clamp;
    const float c = std::isnan(constant) ? INFINITY : constant;
    auto high = [bound](float bias) {
        return std::isnan(bias) ? bound >= 0 : bias > 0;
    };
    const bool flat = high(c + slope * 0.0f), sloped = high(c + slope);
    DepthBiasFactors f;
    f.constant = flat ? kSaturatingBias : -kSaturatingBias;
    f.slope = sloped == flat ? 0.0f : sloped ? kSlopeOutweighs : -kSlopeOutweighs;
    f.clamp = bounded;
    f.unorm24 = true;
    return f;
}

}  // namespace raw
