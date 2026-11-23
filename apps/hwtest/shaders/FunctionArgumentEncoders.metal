#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.function_argument_encoder: argument buffers each function's own encoder lays out
/// (newArgumentEncoderWithBufferIndex:). The case's expected lengths, alignments and offsets are Metal's for these
/// structs on an M4 Max under macOS 27.2.
struct ArgEncInner {
    texture2d<float> texture [[id(0)]];
    constant float4 *colour [[id(1)]];
};

struct ArgEncMixed {
    float tint [[id(0)]];
    array<texture2d<float>, 2> textures [[id(1)]];
    sampler nearest [[id(3)]];
    constant float4 *colour [[id(4)]];
    uint3 counts [[id(5)]];
    half2 pair [[id(6)]];
    float4x4 matrix [[id(7)]];
    constant ArgEncInner *inner [[id(8)]];
    char flag [[id(9)]];
    bool on [[id(10)]];
};

struct ArgEncLeaf {
    texture2d<float> texture [[id(0)]];
    float2 scale [[id(1)]];
};

struct ArgEncArrays {
    array<constant float *, 3> pointers [[id(0)]];
    array<float2, 2> pairs [[id(3)]];
    array<sampler, 2> samplers [[id(5)]];
    ArgEncLeaf one [[id(7)]];
    array<ArgEncLeaf, 2> leaves [[id(10)]];
    texture2d<float> last [[id(20)]];
};

struct ArgEncGaps {
    texture2d<float> a [[id(3)]];
    float b [[id(10)]];
    texture2d<float> c [[id(15)]];
};

struct ArgEncKinds {
    texture2d<float> t [[id(0)]];
    depth2d<float> d [[id(1)]];
    texture2d_array<float> ta [[id(2)]];
    texturecube<float> cube [[id(3)]];
    texture3d<float> t3 [[id(4)]];
    texture_buffer<float> tb [[id(5)]];
    device float4 *rw [[id(6)]];
};

struct ArgEncScalars {
    uchar a [[id(0)]];
    float3 b [[id(1)]];
    short c [[id(2)]];
    ushort4 d [[id(3)]];
};

/// One member a pixel: 0 the second texture of the array, 1 the colour behind the pointer times the tint, 2 and 3 the
/// texture and colour of the argument buffer behind the pointer at id 8, 4 the constants (counts.y, pair.x,
/// matrix[1].z, on and flag), 5 the plain buffer at index 1
fragment float4 argenc_mixed_fragment(float4 position [[position]], constant ArgEncMixed &m [[buffer(0)]],
                                      constant float4 *plain [[buffer(1)]])
{
    switch (uint(position.x)) {
    case 0:
        return m.textures[1].sample(m.nearest, float2(0.5));
    case 1:
        return *m.colour * m.tint;
    case 2:
        return m.inner->texture.sample(m.nearest, float2(0.5));
    case 3:
        return *m.inner->colour;
    case 4:
        return float4(float(m.counts.y) / 255.0, float(m.pair.x), m.matrix[1].z, m.on && m.flag == 3 ? 1.0 : 0.0);
    default:
        return plain[0];
    }
}

fragment float4 argenc_arrays_fragment(constant ArgEncArrays &a [[buffer(2)]], constant ArgEncGaps &g [[buffer(5)]])
{
    float4 c = float4(*a.pointers[0] + *a.pointers[1] + *a.pointers[2]);
    c.xy += a.pairs[0] + a.pairs[1];
    c += a.one.texture.sample(a.samplers[0], a.one.scale);
    c += a.leaves[0].texture.sample(a.samplers[1], a.leaves[0].scale) +
         a.leaves[1].texture.sample(a.samplers[1], a.leaves[1].scale);
    return c + a.last.read(uint2(0)) + g.a.read(uint2(0)) + g.c.read(uint2(0)) + g.b;
}

kernel void argenc_kinds_kernel(constant ArgEncKinds &k [[buffer(0)]], constant ArgEncScalars &s [[buffer(3)]],
                                device float4 *out [[buffer(1)]], uint id [[thread_position_in_grid]])
{
    out[id] = k.t.read(uint2(0)) + float4(k.d.read(uint2(0))) + k.ta.read(uint2(0), 0) + k.t3.read(uint3(0)) +
              k.tb.read(0u) + k.rw[0] + float4(s.a, s.b.x, s.c, float(s.d.w));
    k.rw[1] = out[id];
}
