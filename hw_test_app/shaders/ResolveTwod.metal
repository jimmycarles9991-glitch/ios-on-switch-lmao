#include <metal_stdlib>
using namespace metal;

// tests/ResolveTwodTest.swift: each sample its own hash of the pixel and the sample index (per-sample shading), so a
// resolve averages different codes everywhere
fragment float4 resolve_twod_fragment(float4 position [[position]], uint sample [[sample_id]])
{
    uint h = (uint(position.x) * 73856093u) ^ (uint(position.y) * 19349663u) ^ ((sample + 1u) * 83492791u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return float4(float3(uint3(h, h >> 8, h >> 16) & 255u), float((h >> 24) & 255u)) * (1.0f / 255.0f);
}
