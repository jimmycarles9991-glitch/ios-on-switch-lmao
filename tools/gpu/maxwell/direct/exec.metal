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

// 48 KiB of threadgroup memory, GM20B's whole CTA: each thread writes its words of every 1 KiB row, the last row past
// 47 KiB, and reads its own back after the barrier
kernel void wide_threadgroup_kernel(uint tid [[thread_index_in_threadgroup]], device uint *out [[buffer(0)]])
{
    threadgroup uint memory[12288];
    for (uint k = 0; k < 48; k++)
        memory[tid + 256 * k] = (tid + 256 * k) * 2654435761u;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint x = 0;
    for (uint k = 0; k < 48; k++)
        x ^= memory[tid + 256 * k] + k;
    out[tid] = x;
}

// One word past 48 KiB
kernel void past_threadgroup_kernel(uint tid [[thread_index_in_threadgroup]], device uint *out [[buffer(0)]])
{
    threadgroup uint memory[12289];
    memory[tid * 48] = tid;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    out[tid] = memory[12288 - tid];
}

// 32 float4 varyings: GM20B's 128 interpolated components, every one read by the fragment function
struct Interpolated {
    float4 position [[position]];
    float4 v0;
    float4 v1;
    float4 v2;
    float4 v3;
    float4 v4;
    float4 v5;
    float4 v6;
    float4 v7;
    float4 v8;
    float4 v9;
    float4 v10;
    float4 v11;
    float4 v12;
    float4 v13;
    float4 v14;
    float4 v15;
    float4 v16;
    float4 v17;
    float4 v18;
    float4 v19;
    float4 v20;
    float4 v21;
    float4 v22;
    float4 v23;
    float4 v24;
    float4 v25;
    float4 v26;
    float4 v27;
    float4 v28;
    float4 v29;
    float4 v30;
    float4 v31;
};

static float4 varying(uint i, uint vid)
{
    return float4(float(i), float(vid), 0.5f * float(i), 1.0f + float(i));
}

vertex Interpolated interpolants_vertex(uint vid [[vertex_id]])
{
    Interpolated out;
    out.position = float4(float(vid), 0, 0, 1);
    out.v0 = varying(0, vid);
    out.v1 = varying(1, vid);
    out.v2 = varying(2, vid);
    out.v3 = varying(3, vid);
    out.v4 = varying(4, vid);
    out.v5 = varying(5, vid);
    out.v6 = varying(6, vid);
    out.v7 = varying(7, vid);
    out.v8 = varying(8, vid);
    out.v9 = varying(9, vid);
    out.v10 = varying(10, vid);
    out.v11 = varying(11, vid);
    out.v12 = varying(12, vid);
    out.v13 = varying(13, vid);
    out.v14 = varying(14, vid);
    out.v15 = varying(15, vid);
    out.v16 = varying(16, vid);
    out.v17 = varying(17, vid);
    out.v18 = varying(18, vid);
    out.v19 = varying(19, vid);
    out.v20 = varying(20, vid);
    out.v21 = varying(21, vid);
    out.v22 = varying(22, vid);
    out.v23 = varying(23, vid);
    out.v24 = varying(24, vid);
    out.v25 = varying(25, vid);
    out.v26 = varying(26, vid);
    out.v27 = varying(27, vid);
    out.v28 = varying(28, vid);
    out.v29 = varying(29, vid);
    out.v30 = varying(30, vid);
    out.v31 = varying(31, vid);
    return out;
}

fragment float4 interpolants_fragment(Interpolated in [[stage_in]])
{
    return in.v0 * 1.0f +
           in.v1 * 2.0f +
           in.v2 * 3.0f +
           in.v3 * 4.0f +
           in.v4 * 5.0f +
           in.v5 * 6.0f +
           in.v6 * 7.0f +
           in.v7 * 8.0f +
           in.v8 * 9.0f +
           in.v9 * 10.0f +
           in.v10 * 11.0f +
           in.v11 * 12.0f +
           in.v12 * 13.0f +
           in.v13 * 14.0f +
           in.v14 * 15.0f +
           in.v15 * 16.0f +
           in.v16 * 17.0f +
           in.v17 * 18.0f +
           in.v18 * 19.0f +
           in.v19 * 20.0f +
           in.v20 * 21.0f +
           in.v21 * 22.0f +
           in.v22 * 23.0f +
           in.v23 * 24.0f +
           in.v24 * 25.0f +
           in.v25 * 26.0f +
           in.v26 * 27.0f +
           in.v27 * 28.0f +
           in.v28 * 29.0f +
           in.v29 * 30.0f +
           in.v30 * 31.0f +
           in.v31 * 32.0f;
}

// Two textures and a sampler by ID in a tier-2 argument buffer: the left half samples the first, the right the second
struct TextureArguments {
    texture2d<float> first [[id(0)]];
    texture2d<float> second [[id(1)]];
    sampler nearest [[id(2)]];
};

vertex float4 argument_vertex(uint vid [[vertex_id]])
{
    return float4(float(vid & 1) * 4 - 1, float(vid >> 1) * 4 - 1, 0, 1);
}

fragment float4 argument_textures_fragment(float4 position [[position]],
                                           constant TextureArguments &arguments [[buffer(0)]])
{
    const float2 centre = float2(0.5);
    return position.x < 4.0 ? arguments.first.sample(arguments.nearest, centre)
                             : arguments.second.sample(arguments.nearest, centre);
}
