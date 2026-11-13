#include <metal_stdlib>
using namespace metal;

// tests/PrivateCompressionTest.swift's chain: a hashed pattern, then each level the bilinear 2:1 reduction of the one
// above (the glass pyramid's shape)
fragment float4 comp_pattern_fragment(float4 position [[position]])
{
    uint h = (uint(position.x) * 73856093u) ^ (uint(position.y) * 19349663u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    const bool flat = ((uint(position.x) >> 5) + (uint(position.y) >> 5)) & 1;
    return flat ? float4(0.25f, 0.5f, 0.75f, 1.0f) : float4(float3(uint3(h, h >> 8, h >> 16) & 255u), 255.0f) / 255.0f;
}

fragment float4 comp_reduce_fragment(float4 position [[position]], texture2d<float> above [[texture(0)]])
{
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    return above.sample(linear, position.xy / float2(above.get_width() / 2, above.get_height() / 2));
}
