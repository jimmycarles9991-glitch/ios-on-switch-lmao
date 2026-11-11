// SPDX-License-Identifier: MIT
// cubiccheck's state: a quad sampling texture 0 through the runtime sampler at index 0, as RenderBox's image draws do
#include <metal_stdlib>
using namespace metal;

struct Out {
    float4 position [[position]];
    float2 uv;
};

vertex Out cubic_vert(uint vid [[vertex_id]])
{
    Out o;
    o.uv = float2(vid & 1, (vid >> 1) & 1);
    o.position = float4(o.uv * 2.0 - 1.0, 0.0, 1.0);
    return o;
}

fragment float4 cubic_frag(Out in [[stage_in]], texture2d<float> image [[texture(0)]], sampler s [[sampler(0)]])
{
    return image.sample(s, in.uv);
}
