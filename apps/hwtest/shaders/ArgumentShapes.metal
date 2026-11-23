#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.argument_array_of_arrays and gpu.gm20b.plain_struct_encoder: buffers whose layout each function's own encoder
/// gives (newArgumentEncoderWithBufferIndex:). The cases' expected lengths, alignments and offsets are Metal's for
/// these structs on an M4 Max under macOS 27.2, and their pixels and words what the same draw and dispatch gave there.
struct GridInner {
    texture2d<float> texture [[id(0)]];
    constant float4 *colour [[id(1)]];
};

struct GridArguments {
    float lead [[id(0)]];
    array<array<texture2d<float>, 2>, 2> tex [[id(3)]];
    array<array<sampler, 3>, 2> samp [[id(9)]];
    array<array<constant float4 *, 2>, 3> ptrs [[id(20)]];
    array<array<array<texture2d<float>, 2>, 2>, 2> cube [[id(30)]];
    array<array<float2, 2>, 3> consts [[id(40)]];
    array<array<constant GridInner *, 2>, 2> inners [[id(70)]];
};

/// One element a pixel: 0 tex[0][1] through samp[1][2], 1 the colour behind ptrs[2][1], 2 cube[1][0][1], 3
/// consts[2][1], consts[0][1].x and lead, 4 the texture behind inners[1][0], 5 the colour behind inners[0][1]
fragment float4 grid_fragment(float4 position [[position]], constant GridArguments &g [[buffer(0)]])
{
    switch (uint(position.x)) {
    case 0:
        return g.tex[0][1].sample(g.samp[1][2], float2(0.5));
    case 1:
        return *g.ptrs[2][1];
    case 2:
        return g.cube[1][0][1].read(uint2(0));
    case 3:
        return float4(g.consts[2][1], g.consts[0][1].x, g.lead);
    case 4:
        return g.inners[1][0]->texture.read(uint2(0));
    default:
        return *g.inners[0][1]->colour;
    }
}

struct PlainK {
    float m[3];
    int n[2];
    float tail;
};

struct PlainS {
    float4 a;
    float b;
    uint c;
    bool d;
    float2 e;
    short f;
    uchar3 g;
    float3 h;
};

/// One-component vectors, which Metal reflects with no data type
struct PlainV {
    float x;
    packed_vec<int, 1> a;
};

/// Reads back what the plain buffers' own encoders wrote: k's ids 0, 1 and 2 (tail, n[1], m[2]), s's id 0 (h) and v's
/// id 0 (a)
kernel void plain_struct_kernel(constant PlainK &k [[buffer(0)]], device atomic_uint *counter [[buffer(1)]],
                                constant PlainS &s [[buffer(2)]], device float4 *out [[buffer(3)]],
                                constant PlainV &v [[buffer(4)]])
{
    out[0] = float4(k.tail, float(k.n[1]), k.m[2], 0);
    out[1] = float4(s.h, 0);
    out[2] = float4(float(v.a[0]), 0, 0, 0);
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}
