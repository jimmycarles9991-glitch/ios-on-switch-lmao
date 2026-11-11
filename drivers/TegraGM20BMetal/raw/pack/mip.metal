// SPDX-License-Identifier: MIT
// raw GM20B path: generateMipmapsForTexture: of a format the 2D engine is not known to filter as Metal does (runtime/raw_mip.h),
// one pass a level drawn into that level from the one above, one pixel a fragment: the level above sampled at the pixel's centre
// mapped onto it (vkCmdBlitImage's linear blit, m2v's chain), so a 2:1 step reads the four texels it halves with weights of one
// half. The texture unit decodes sRGB texels before it filters and the target encodes what the fragment writes, so an sRGB
// level is the mean of the texels' linear values. Buffer 0 holds the reciprocal of the level's size.
// One function a target format: the pack keys an own state by its function's name.
#include <metal_stdlib>
using namespace metal;

struct Out {
    float4 position [[position]];
};

vertex Out mip_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

constexpr sampler kAbove(filter::linear, mip_filter::none, address::clamp_to_edge, coord::normalized);

static float4 above(Out in, texture2d<float> image, float2 inverseSize)
{
    return image.sample(kAbove, in.position.xy * inverseSize, level(0));
}

#define MIP(name)                                                                                                             \
    fragment float4 mip_##name##_frag(Out in [[stage_in]], texture2d<float> image [[texture(0)]],                         \
                                      constant float2 &inverseSize [[buffer(0)]])                                            \
    {                                                                                                                         \
        return above(in, image, inverseSize);                                                                                 \
    }

MIP(a8)
MIP(r16)
MIP(r16f)
MIP(b5g6r5)
MIP(bgr5a1)
MIP(r32f)
MIP(rg16)
MIP(rg16f)
MIP(rgba8_srgb)
MIP(bgra8_srgb)
MIP(rgb10a2)
MIP(rg11b10f)
MIP(bgr10a2)
MIP(rg32f)
MIP(rgba16)
MIP(rgba32f)
