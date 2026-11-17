// SPDX-License-Identifier: MIT
// The states and kernels direct_exec_test.py executes the raw path's records of (directrun.mm makes them).
#include <metal_stdlib>
using namespace metal;

[[visible]] uint binary_affine_definition(uint x) { return 2 * x + 1; }
[[visible]] uint binary_affine_alias(uint x);

kernel void binary_affine_kernel(device uint *out [[buffer(0)]], uint tid [[thread_position_in_grid]])
{
    out[tid] = binary_affine_alias(tid);
}

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

vertex Shaded cubic_vertex(uint vid [[vertex_id]], const device float2 *uv [[buffer(0)]],
                           texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]])
{
    return {float4(float(vid & 1) * 2 - 1, float(vid >> 1) * 2 - 1, 0, 1),
            image.sample(filtering, uv[0], level(0))};
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

vertex float4 shadow_vertex(uint vid [[vertex_id]])
{
    return float4(float(vid & 1), float(vid >> 1), 0, 1);
}

struct StencilOut {
    float4 colour [[color(0)]];
    uint stencil [[stencil]];
};

fragment StencilOut shadow_fragment(float4 p [[position]])
{
    return {float4(p.x / 64.0f, p.y / 64.0f, 0, 1), uint(p.x) * 7u + uint(p.y) * 61u + 0x100u};
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

// Threadgroup memory a dispatch sizes (setThreadgroupMemoryLength:atIndex: 0), as MPS's kernels take theirs: each thread
// writes its words of every 1 KiB row of `words`, and reads its own back after the barrier
kernel void dynamic_threadgroup_kernel(threadgroup uint *memory [[threadgroup(0)]], device uint *out [[buffer(0)]],
                                       constant uint &rows [[buffer(1)]], uint tid [[thread_index_in_threadgroup]])
{
    for (uint k = 0; k < rows; k++)
        memory[tid + 256 * k] = (tid + 256 * k) * 2654435761u;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint x = 0;
    for (uint k = 0; k < rows; k++)
        x ^= memory[tid + 256 * k] + k;
    out[tid] = x;
}

// States of more than 32 set-0 entries: twenty storage buffers a stage, each read once (and one or two fragment uniform
// buffers after them, their descriptors past set-0 byte 496), and every argument the A10X's tables let a stage bind (31
// buffers, 31 textures, 16 samplers) and the A13's (31 buffers, 128 textures, 16 samplers)
#define SWITCH_EACH_20(X)                                                                                              \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) X(17) X(18) X(19)
#define SWITCH_EACH_31(X) SWITCH_EACH_20(X) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30)
#define SWITCH_EACH_16(X)                                                                                              \
    X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15)
#define SWITCH_PAIRS_31(X)                                                                                             \
    X(0, 0) X(1, 1) X(2, 2) X(3, 3) X(4, 4) X(5, 5) X(6, 6) X(7, 7) X(8, 8) X(9, 9) X(10, 10) X(11, 11) X(12, 12)      \
    X(13, 13) X(14, 14) X(15, 15) X(16, 0) X(17, 1) X(18, 2) X(19, 3) X(20, 4) X(21, 5) X(22, 6) X(23, 7) X(24, 8)     \
    X(25, 9) X(26, 10) X(27, 11) X(28, 12) X(29, 13) X(30, 14)
#define SWITCH_EACH_128(X) X(0) X(1) X(2) X(3) X(4) X(5) X(6) X(7) X(8) X(9) X(10) X(11) X(12) X(13) X(14) X(15) X(16) \
    X(17) X(18) X(19) X(20) X(21) X(22) X(23) X(24) X(25) X(26) X(27) X(28) X(29) X(30) X(31) X(32) X(33) X(34) X(35)  \
    X(36) X(37) X(38) X(39) X(40) X(41) X(42) X(43) X(44) X(45) X(46) X(47) X(48) X(49) X(50) X(51) X(52) X(53) X(54)  \
    X(55) X(56) X(57) X(58) X(59) X(60) X(61) X(62) X(63) X(64) X(65) X(66) X(67) X(68) X(69) X(70) X(71) X(72) X(73)  \
    X(74) X(75) X(76) X(77) X(78) X(79) X(80) X(81) X(82) X(83) X(84) X(85) X(86) X(87) X(88) X(89) X(90) X(91) X(92)  \
    X(93) X(94) X(95) X(96) X(97) X(98) X(99) X(100) X(101) X(102) X(103) X(104) X(105) X(106) X(107) X(108) X(109)    \
    X(110) X(111) X(112) X(113) X(114) X(115) X(116) X(117) X(118) X(119) X(120) X(121) X(122) X(123) X(124) X(125)    \
    X(126) X(127)
#define SWITCH_PAIRS_128(X) X(0, 0) X(1, 1) X(2, 2) X(3, 3) X(4, 4) X(5, 5) X(6, 6) X(7, 7) X(8, 8) X(9, 9) X(10, 10)  \
    X(11, 11) X(12, 12) X(13, 13) X(14, 14) X(15, 15) X(16, 0) X(17, 1) X(18, 2) X(19, 3) X(20, 4) X(21, 5) X(22, 6)   \
    X(23, 7) X(24, 8) X(25, 9) X(26, 10) X(27, 11) X(28, 12) X(29, 13) X(30, 14) X(31, 15) X(32, 0) X(33, 1) X(34, 2)  \
    X(35, 3) X(36, 4) X(37, 5) X(38, 6) X(39, 7) X(40, 8) X(41, 9) X(42, 10) X(43, 11) X(44, 12) X(45, 13) X(46, 14)   \
    X(47, 15) X(48, 0) X(49, 1) X(50, 2) X(51, 3) X(52, 4) X(53, 5) X(54, 6) X(55, 7) X(56, 8) X(57, 9) X(58, 10)      \
    X(59, 11) X(60, 12) X(61, 13) X(62, 14) X(63, 15) X(64, 0) X(65, 1) X(66, 2) X(67, 3) X(68, 4) X(69, 5) X(70, 6)   \
    X(71, 7) X(72, 8) X(73, 9) X(74, 10) X(75, 11) X(76, 12) X(77, 13) X(78, 14) X(79, 15) X(80, 0) X(81, 1) X(82, 2)  \
    X(83, 3) X(84, 4) X(85, 5) X(86, 6) X(87, 7) X(88, 8) X(89, 9) X(90, 10) X(91, 11) X(92, 12) X(93, 13) X(94, 14)   \
    X(95, 15) X(96, 0) X(97, 1) X(98, 2) X(99, 3) X(100, 4) X(101, 5) X(102, 6) X(103, 7) X(104, 8) X(105, 9)          \
    X(106, 10) X(107, 11) X(108, 12) X(109, 13) X(110, 14) X(111, 15) X(112, 0) X(113, 1) X(114, 2) X(115, 3)          \
    X(116, 4) X(117, 5) X(118, 6) X(119, 7) X(120, 8) X(121, 9) X(122, 10) X(123, 11) X(124, 12) X(125, 13) X(126, 14) \
    X(127, 15)
#define SWITCH_BUFFER(i) , device const float *b##i [[buffer(i)]]
#define SWITCH_READ(i) +b##i[0]
#define SWITCH_TEXTURE(i) , texture2d<float> t##i [[texture(i)]]
#define SWITCH_SAMPLER(i) , sampler s##i [[sampler(i)]]
#define SWITCH_SAMPLE(i, j) +t##i.sample(s##j, float2(0.5f), level(0)).x

vertex Shaded wide_set_vertex(uint vid [[vertex_id]] SWITCH_EACH_20(SWITCH_BUFFER))
{
    const float sum = 0.0f SWITCH_EACH_20(SWITCH_READ);
    return {float4(float(vid & 1) * 4 - 1, float(vid >> 1) * 4 - 1, 0, 1), float4(0, sum, 0, 1)};
}

fragment float4 wide_set_fragment(Shaded in [[stage_in]] SWITCH_EACH_20(SWITCH_BUFFER))
{
    return float4(0.0f SWITCH_EACH_20(SWITCH_READ), in.colour.y, 0, 1);
}

fragment float4 far_uniform_fragment(Shaded in [[stage_in]] SWITCH_EACH_20(SWITCH_BUFFER),
                                     constant float4 &tint [[buffer(20)]])
{
    return float4(0.0f SWITCH_EACH_20(SWITCH_READ), in.colour.y, 0, 1) * tint;
}

fragment float4 two_far_fragment(Shaded in [[stage_in]] SWITCH_EACH_20(SWITCH_BUFFER), constant float4 &tint [[buffer(20)]],
                                 constant float4 &shift [[buffer(21)]])
{
    return float4(0.0f SWITCH_EACH_20(SWITCH_READ), in.colour.y, 0, 1) * tint + shift;
}

vertex Shaded bound_vertex(uint vid [[vertex_id]] SWITCH_EACH_31(SWITCH_BUFFER) SWITCH_EACH_31(SWITCH_TEXTURE)
                               SWITCH_EACH_16(SWITCH_SAMPLER))
{
    const float buffers = 0.0f SWITCH_EACH_31(SWITCH_READ);
    const float textures = 0.0f SWITCH_PAIRS_31(SWITCH_SAMPLE);
    return {float4(float(vid & 1) * 4 - 1, float(vid >> 1) * 4 - 1, 0, 1), float4(buffers, textures, 0, 1)};
}

fragment float4 bound_fragment(Shaded in [[stage_in]] SWITCH_EACH_31(SWITCH_BUFFER) SWITCH_EACH_31(SWITCH_TEXTURE)
                                   SWITCH_EACH_16(SWITCH_SAMPLER))
{
    const float buffers = 0.0f SWITCH_EACH_31(SWITCH_READ);
    const float textures = 0.0f SWITCH_PAIRS_31(SWITCH_SAMPLE);
    return float4(in.colour.x + buffers, in.colour.y + textures, 0, 1);
}

vertex Shaded bound_wide_vertex(uint vid [[vertex_id]] SWITCH_EACH_31(SWITCH_BUFFER) SWITCH_EACH_128(SWITCH_TEXTURE)
                                    SWITCH_EACH_16(SWITCH_SAMPLER))
{
    const float buffers = 0.0f SWITCH_EACH_31(SWITCH_READ);
    const float textures = 0.0f SWITCH_PAIRS_128(SWITCH_SAMPLE);
    return {float4(float(vid & 1) * 4 - 1, float(vid >> 1) * 4 - 1, 0, 1), float4(buffers, textures, 0, 1)};
}

fragment float4 bound_wide_fragment(Shaded in [[stage_in]] SWITCH_EACH_31(SWITCH_BUFFER) SWITCH_EACH_128(SWITCH_TEXTURE)
                                        SWITCH_EACH_16(SWITCH_SAMPLER))
{
    const float buffers = 0.0f SWITCH_EACH_31(SWITCH_READ);
    const float textures = 0.0f SWITCH_PAIRS_128(SWITCH_SAMPLE);
    return float4(in.colour.x + buffers, in.colour.y + textures, 0, 1);
}
