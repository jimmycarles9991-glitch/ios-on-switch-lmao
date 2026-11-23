// SPDX-License-Identifier: MIT
// test/linkcheck's caller: a fragment function that calls a visible function the pipeline links (MTLLinkedFunctions), and the
// same function with the callee's body inline.
#include <metal_stdlib>
using namespace metal;

struct Varyings {
    float4 position [[position]];
    float4 color;
};

vertex Varyings linked_vert(uint vid [[vertex_id]], constant float4 *colors [[buffer(0)]])
{
    Varyings v;
    v.position = float4(float(vid & 1) * 2 - 1, float(vid >> 1) * 2 - 1, 0, 1);
    v.color = colors[vid & 3];
    return v;
}

[[visible]] float4 custom_fn(float4 color, float scale);

fragment float4 linked_frag(Varyings in [[stage_in]], constant float &scale [[buffer(1)]])
{
    return custom_fn(in.color, scale);
}

fragment float4 inline_frag(Varyings in [[stage_in]], constant float &scale [[buffer(1)]])
{
    return float4(in.color.bgr * scale, in.color.a);
}
