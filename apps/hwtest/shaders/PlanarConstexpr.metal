#include <metal_stdlib>
using namespace metal;

// tests/PlanarConstexprTest.swift: a two-plane 4:4:4 texture sampled through constexpr samplers, target pixel (x, y)
// reading texel (x - 1, y - 1), so the ring of pixels around the texture reads what each address mode gives outside it
static float2 ringTexel(float4 position, texture2d<float> t)
{
    return (position.xy - 1.0f) / float2(t.get_width(), t.get_height());
}

fragment float4 planar_constexpr_zero(float4 position [[position]], texture2d<float> t [[texture(0)]])
{
    constexpr sampler s(filter::nearest, address::clamp_to_zero);
    return t.sample(s, ringTexel(position, t));
}

fragment float4 planar_constexpr_border(float4 position [[position]], texture2d<float> t [[texture(0)]])
{
    constexpr sampler s(filter::nearest, address::clamp_to_border, border_color::opaque_white);
    return t.sample(s, ringTexel(position, t));
}

fragment float4 planar_constexpr_edge(float4 position [[position]], texture2d<float> t [[texture(0)]])
{
    constexpr sampler s(filter::nearest, address::clamp_to_edge);
    return t.sample(s, ringTexel(position, t));
}

fragment float4 planar_constexpr_repeat(float4 position [[position]], texture2d<float> t [[texture(0)]])
{
    constexpr sampler s(filter::nearest, address::repeat);
    return t.sample(s, ringTexel(position, t));
}

fragment float4 planar_constexpr_mirror(float4 position [[position]], texture2d<float> t [[texture(0)]])
{
    constexpr sampler s(filter::nearest, address::mirrored_repeat);
    return t.sample(s, ringTexel(position, t));
}

fragment float4 planar_constexpr_linear(float4 position [[position]], texture2d<float> t [[texture(0)]])
{
    constexpr sampler s(filter::linear, address::clamp_to_zero);
    return t.sample(s, ringTexel(position, t));
}
