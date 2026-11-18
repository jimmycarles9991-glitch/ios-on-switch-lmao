#include <metal_stdlib>
using namespace metal;

// tests/FixedBlendRuleTest.swift's layers: each pixel's premultiplied source for layer `layer.x`, from an integer hash of
// its position the test repeats on the CPU; 8-bit codes (layer.y 0) or codes plus sixteenths, a shader-computed colour
// (layer.y 1). One multiply of an exact integer per channel, so the CPU's float is the GPU's.
static uint rule_hash(uint x, uint y, uint layer)
{
    uint h = (x * 73856093u) ^ (y * 19349663u) ^ (layer * 83492791u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

static float4 rule_source(float4 position, uint2 layer)
{
    const uint h = rule_hash(uint(position.x), uint(position.y), layer.x);
    const uint alpha = h & 255u;
    const uint3 codes = (uint3(h >> 8, h >> 16, h >> 24) & 255u) * alpha / 255u;
    if (layer.y == 0)
        return float4(float3(codes), float(alpha)) * (1.0f / 255.0f);
    const uint3 sixteenths = codes * 16u + ((uint3(h >> 4, h >> 12, h >> 20) & 15u) * min(alpha, 1u));
    return float4(float3(sixteenths) * (1.0f / 4080.0f), float(alpha) * (1.0f / 255.0f));
}

fragment float4 rule_rop_fragment(float4 position [[position]], constant uint2 &layer [[buffer(0)]])
{
    return rule_source(position, layer);
}

fragment float4 rule_fetch_fragment(float4 position [[position]], float4 destination [[color(0)]],
                                    constant uint2 &layer [[buffer(0)]])
{
    const float4 s = rule_source(position, layer);
    return s + destination * (1.0f - s.a);
}
