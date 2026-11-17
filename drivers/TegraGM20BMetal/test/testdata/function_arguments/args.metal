#include <metal_stdlib>
using namespace metal;

struct Inner {
    texture2d<float> texture [[id(0)]];
    constant float4 *colour [[id(1)]];
};

struct Mixed {
    float tint [[id(0)]];
    array<texture2d<float>, 2> textures [[id(1)]];
    sampler nearest [[id(3)]];
    constant float4 *colour [[id(4)]];
    uint3 counts [[id(5)]];
    half2 pair [[id(6)]];
    float4x4 matrix [[id(7)]];
    constant Inner *inner [[id(8)]];
    char flag [[id(9)]];
    bool on [[id(10)]];
};

struct Leaf {
    texture2d<float> texture [[id(0)]];
    float2 scale [[id(1)]];
};

struct Arrays {
    array<constant float *, 3> pointers [[id(0)]];
    array<float2, 2> pairs [[id(3)]];
    array<sampler, 2> samplers [[id(5)]];
    Leaf one [[id(7)]];
    array<Leaf, 2> leaves [[id(10)]];
    texture2d<float> last [[id(20)]];
};

struct Gaps {
    texture2d<float> a [[id(3)]];
    float b [[id(10)]];
    texture2d<float> c [[id(15)]];
};

struct Scalars {
    uchar a [[id(0)]];
    float3 b [[id(1)]];
    short c [[id(2)]];
    ushort4 d [[id(3)]];
};

struct Kinds {
    texture2d<float> t [[id(0)]];
    depth2d<float> d [[id(1)]];
    texture2d_array<float> ta [[id(2)]];
    texturecube<float> cube [[id(3)]];
    texture3d<float> t3 [[id(4)]];
    texture_buffer<float> tb [[id(5)]];
    device float4 *rw [[id(6)]];
};

fragment float4 mixed_fragment(float4 position [[position]], constant Mixed &m [[buffer(0)]],
                               constant float4 *plain [[buffer(1)]])
{
    float4 c = m.textures[0].sample(m.nearest, float2(0.5)) + m.textures[1].sample(m.nearest, float2(0.5));
    c += *m.colour * m.tint + float4(m.counts.x, m.counts.y, m.counts.z, 0) + float4(float2(m.pair), 0, 0);
    c += m.matrix[1] + m.inner->texture.sample(m.nearest, float2(0.5)) + *m.inner->colour;
    c += float4(m.flag, m.on ? 1 : 0, 0, 0);
    return c + plain[0];
}

fragment float4 arrays_fragment(float4 position [[position]], constant Arrays &a [[buffer(2)]],
                                constant Gaps &g [[buffer(5)]])
{
    float4 c = float4(*a.pointers[0] + *a.pointers[1] + *a.pointers[2]);
    c.xy += a.pairs[0] + a.pairs[1];
    c += a.one.texture.sample(a.samplers[0], a.one.scale);
    c += a.leaves[0].texture.sample(a.samplers[1], a.leaves[0].scale) +
         a.leaves[1].texture.sample(a.samplers[1], a.leaves[1].scale);
    c += a.last.read(uint2(0)) + g.a.read(uint2(0)) + g.c.read(uint2(0)) + g.b;
    return c;
}

kernel void kinds_kernel(constant Kinds &k [[buffer(0)]], constant Scalars &s [[buffer(3)]],
                         device float4 *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{
    out[id] = k.t.read(uint2(0)) + float4(k.d.read(uint2(0))) + k.ta.read(uint2(0), 0) + k.t3.read(uint3(0)) +
              k.tb.read(0u) + k.rw[0] + float4(s.a, s.b.x, s.c, float(s.d.w));
    k.rw[1] = out[id];
}

vertex float4 inner_vertex(uint v [[vertex_id]], constant Inner &inner [[buffer(4)]])
{
    return *inner.colour + float4(v);
}

struct Plain {
    float4 a;
    float b;
};

fragment float4 plain_fragment(constant Inner &inner [[buffer(0)]], constant Plain &p [[buffer(1)]],
                               device float *d [[buffer(2)]])
{
    return *inner.colour + p.a + p.b + d[0];
}

constant bool use_mixed [[function_constant(0)]];
constant bool use_inner = !use_mixed;

fragment float4 gated_fragment(constant Mixed &m [[buffer(0), function_constant(use_mixed)]],
                               constant Inner &inner [[buffer(0), function_constant(use_inner)]])
{
    if (use_mixed)
        return *m.colour;
    return *inner.colour;
}

struct Grid {
    array<array<texture2d<float>, 2>, 2> t [[id(0)]];
};

fragment float4 grid_fragment(constant Grid &g [[buffer(0)]])
{
    return g.t[1][1].read(uint2(0));
}
