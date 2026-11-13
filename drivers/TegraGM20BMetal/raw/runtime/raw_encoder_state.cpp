// SPDX-License-Identifier: MIT
// The render encoder's state that persists in the channel between draws: a state's registers by its segment or inline
// (raw_state_segment.h), and the raster state.
#include "raw_gpu.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace raw {

// hw_test_app's inline arm (CommandBuffer::inlineStates) binds every state as before segments: inline, none skipped
bool RenderEncoder::measuringInline() const
{
#if !defined(SWITCH_DEBUG) || SWITCH_DEBUG
    return cb_->inlineStates_;
#else
    return false;
#endif
}

bool RenderEncoder::bindsBySegment() const
{
    return !measuringInline() && cb_->segmentBinds_ < kSegmentBindsPerCommandBuffer;
}

void RenderEncoder::bindState(Push &p, const Pipeline &pl)
{
    HeldSegment &held = cb_->heldSegment_;
    if (held.holds(pl.segment) && !measuringInline()) {
        RAW_DIAGNOSTIC(cb_->counts_.segmentSkips++);
        return;
    }
    if (!pl.segment || !bindsBySegment()) {
        p.raw(pl.methods.data(), uint32_t(pl.methods.size()));
        if (pl.segment && !measuringInline())
            held.bound(pl.segment);
        else
            held.lose();
        return;
    }
    bindSegment(bodies_, bodyStart_, p.at, pl.segment);
    held.bound(pl.segment);
    cb_->segmentBinds_++;
    RAW_DIAGNOSTIC(cb_->counts_.segmentBinds++);
    RAW_DIAGNOSTIC(cb_->counts_.segmentWords += pl.segment.words);
}

void RenderEncoder::writeRaster(Push &p, const Raster &want)
{
    if (want.bias != rasterHeld_.bias) {
        p.m(NVB197_SET_POLY_OFFSET_POINT, want.bias);
        p.m(NVB197_SET_POLY_OFFSET_LINE, want.bias);
        p.m(NVB197_SET_POLY_OFFSET_FILL, want.bias);
        rasterHeld_.bias = want.bias;
    }
    if (want.biasConstant != rasterHeld_.biasConstant || want.biasSlope != rasterHeld_.biasSlope ||
        want.biasClamp != rasterHeld_.biasClamp) {
        p.m(NVB197_SET_DEPTH_BIAS, want.biasConstant);
        p.m(NVB197_SET_SLOPE_SCALE_DEPTH_BIAS, want.biasSlope);
        p.m(NVB197_SET_DEPTH_BIAS_CLAMP, want.biasClamp);
        rasterHeld_.biasConstant = want.biasConstant;
        rasterHeld_.biasSlope = want.biasSlope;
        rasterHeld_.biasClamp = want.biasClamp;
    }
    if (want.cull != rasterHeld_.cull) {
        p.m(NVB197_OGL_SET_CULL, want.cull != 0);
        if (want.cull)
            p.m(NVB197_OGL_SET_CULL_FACE, want.cull == 1 ? 0x404 : 0x405);  // FRONT, BACK
        rasterHeld_.cull = want.cull;
    }
    if (want.winding != rasterHeld_.winding) {
        p.m(NVB197_OGL_SET_FRONT_FACE, want.winding ? 0x901 : 0x900);  // CCW, CW
        rasterHeld_.winding = want.winding;
    }
    if (want.fill != rasterHeld_.fill) {
        const uint32_t mode = want.fill ? 0x1b01 : 0x1b02;  // LINE, FILL
        p.m(NVB197_SET_FRONT_POLYGON_MODE, mode);
        p.m(NVB197_SET_BACK_POLYGON_MODE, mode);
        rasterHeld_.fill = want.fill;
    }
}

// NVK's clip control for depth clamp without depth clip (nvk_cmd_draw.c nvk_flush_rs_state): the pixel's depth clamped, no
// [0, 1] clamp of its own, the geometry clipped in x and y only, its depth guardband wide (SET_VIEWPORT_CLIP_CONTROL
// MIN_Z_ZERO_MAX_Z_ONE 0:0, GEOMETRY_GUARDBAND_Z 2:1, PIXEL_MIN_Z/MAX_Z 3:3/4:4, GEOMETRY_CLIP 13:11)
static uint32_t floatBits(float f)
{
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

static uint32_t depthClamped(uint32_t clip)
{
    constexpr uint32_t kZeroOne = 1u, kGuardbandZ = 3u << 1, kPixelClamp = 3u << 3, kGeometryClip = 7u << 11,
                       kGuardbandZ256 = 1u << 1, kFrustumXy = 2u << 11;
    return (clip & ~(kZeroOne | kGuardbandZ | kGeometryClip)) | kGuardbandZ256 | kPixelClamp | kFrustumXy;
}

// After the state's bind, whose registers hold its own clip control: the encoder's clamp written over it, and the segment
// the channel holds no longer its registers. Before Volta the pixel clamp is against the viewport's clip depth range, which
// NVK's MME sets to the viewport's depth range while clamping and to (-inf, +inf) otherwise (nvk_mme_set_z_clamp).
bool RenderEncoder::writeClipControl(Push &p, const Pipeline &pl, const char **why)
{
    if (depthClamp_ && !pl.namesViewportClip) {
        *why = "a depth clamp on a state whose registers set no clip control";
        return false;
    }
    const uint32_t want = depthClamp_ ? depthClamped(pl.viewportClip) : pl.viewportClip;
    if (pl.namesViewportClip && want != clipHeld_) {
        p.m(NVB197_SET_VIEWPORT_CLIP_CONTROL, want);
        clipHeld_ = want;
        cb_->heldSegment_.lose();
    }
    if (depthClamp_) {
        const float lo = std::min(depthRange_[0], depthRange_[1]), hi = std::max(depthRange_[0], depthRange_[1]);
        const uint32_t range[2] = {floatBits(lo), floatBits(hi)};
        if (!clampRangeHeld_ || memcmp(range, clampRange_, sizeof range)) {
            writeClipDepthRange(p, range);
            memcpy(clampRange_, range, sizeof range);
            clampRangeHeld_ = true;
        }
    } else {
        unclampDepthRange(p);
    }
    return true;
}

void RenderEncoder::writeClipDepthRange(Push &p, const uint32_t range[2])
{
    p.m(NVB197_SET_VIEWPORT_CLIP_MIN_Z(0), range[0]);
    p.m(NVB197_SET_VIEWPORT_CLIP_MAX_Z(0), range[1]);
    cb_->heldSegment_.lose();
}

void RenderEncoder::unclampDepthRange(Push &p)
{
    if (!clampRangeHeld_)
        return;
    const uint32_t unbounded[2] = {floatBits(-INFINITY), floatBits(INFINITY)};
    writeClipDepthRange(p, unbounded);
    clampRangeHeld_ = false;
}

}  // namespace raw
