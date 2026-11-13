// SPDX-License-Identifier: MIT
// The states and kernels direct_exec_test.py executes the raw path's records of (directrun.mm makes them).
#include <metal_stdlib>
using namespace metal;

struct ControlPoint {
    float4 position [[attribute(0)]];
};

struct Patch {
    patch_control_point<ControlPoint> points;
};

struct Tessellated {
    float4 position [[position]];
    float4 colour;
};

[[patch(triangle, 3)]] vertex Tessellated tess_vertex(Patch patch [[stage_in]], float3 uvw [[position_in_patch]],
                                                      uint id [[patch_id]])
{
    const float4 p =
        patch.points[0].position * uvw.x + patch.points[1].position * uvw.y + patch.points[2].position * uvw.z;
    return {p, float4(uvw, float(id))};
}

fragment float4 tess_fragment(Tessellated in [[stage_in]])
{
    return in.colour;
}

struct Amplified {
    float4 position [[position]];
    float4 colour;
};

vertex Amplified amp_vertex(uint vid [[vertex_id]], ushort aid [[amplification_id]],
                            const device float4 *points [[buffer(0)]])
{
    return {points[vid] + float4(0.5f * float(aid), 0, 0, 0), float4(float(aid), float(vid), 0, 1)};
}

fragment float4 amp_fragment(Amplified in [[stage_in]])
{
    return in.colour;
}

struct Wide {
    float2 position [[attribute(0)]];
    float4 colour [[attribute(1)]];
    float4 tint [[attribute(2)]];
};

struct Shaded {
    float4 position [[position]];
    float4 colour;
};

vertex Shaded wide_vertex(Wide in [[stage_in]])
{
    return {float4(in.position, 0, 1), in.colour * in.tint};
}

struct Dynamic {
    float2 position [[attribute(0)]];
};

vertex Shaded dynamic_vertex(Dynamic in [[stage_in]], const device float4 *colour [[buffer(2)]])
{
    return {float4(in.position, 0, 1), colour[0]};
}

fragment float4 wide_fragment(Shaded in [[stage_in]])
{
    return in.colour;
}

struct LayeredFetch {
    float4 position [[position]];
    uint layer [[render_target_array_index]];
};

vertex LayeredFetch layered_fetch_vertex(uint vid [[vertex_id]], uint iid [[instance_id]])
{
    return {float4(float(vid & 1) - 0.5f, float(vid >> 1) - 0.5f, 0, 1), iid};
}

fragment float4 layered_fetch_fragment(float4 c [[color(0)]])
{
    return float4(c.g, c.b, c.r, c.a);
}

struct StageIn {
    float2 value [[attribute(0)]];
};

kernel void stage_in_kernel(StageIn in [[stage_in]], uint gid [[thread_position_in_grid]],
                            device float *out [[buffer(1)]])
{
    out[gid] = in.value.x * 2.0f + in.value.y;
}
