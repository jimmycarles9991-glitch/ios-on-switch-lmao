// Fragment functions that fetch the color they write while the pipeline blends it: their storage form computes the
// blend. The source comes from one texture sample and the second source from another, so a host run sets both.
#include <metal_stdlib>
using namespace metal;

struct Raster {
    float4 position [[position]];
};

vertex Raster rop_vert(uint vid [[vertex_id]])
{
    const float2 at = float2((vid << 1) & 2, vid & 2);
    return {float4(at * 2.0f - 1.0f, 0.0f, 1.0f)};
}

fragment float4 rop_frag(float4 d [[color(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
    const float4 source = t.sample(s, float2(0.25f, 0.5f));
    return d.x == 12345.0f ? d : source;
}

fragment float4 rop_frag_const(float4 d [[color(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
    const float4 read = d.x == 12345.0f ? d : t.sample(s, float2(0.25f, 0.5f));
    return float4(read.x, 0.0f, read.z, read.w);
}

fragment float rop_frag_r(float d [[color(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
    const float source = t.sample(s, float2(0.25f, 0.5f)).x;
    return d == 12345.0f ? d : source;
}

struct Dual {
    float4 color [[color(0), index(0)]];
    float4 second [[color(0), index(1)]];
};

fragment Dual rop_frag_dual(float4 d [[color(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
    const float4 source = t.sample(s, float2(0.25f, 0.5f));
    return {d.x == 12345.0f ? d : source, t.sample(s, float2(0.75f, 0.5f))};
}

fragment float4 rop_frag_unread(float4 d [[color(0)]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]])
{
    return t.sample(s, float2(0.25f, 0.5f));
}
