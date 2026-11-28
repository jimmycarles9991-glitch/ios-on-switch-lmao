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

// resolve_twod_fragment's sample `sample` at every pixel of a one-sample target: the codes a format stores for it
fragment float4 resolve_twod_sample_fragment(float4 position [[position]], constant uint &sample [[buffer(0)]])
{
    uint h = (uint(position.x) * 73856093u) ^ (uint(position.y) * 19349663u) ^ ((sample + 1u) * 83492791u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return float4(float3(uint3(h, h >> 8, h >> 16) & 255u), float((h >> 24) & 255u)) * (1.0f / 255.0f);
}

// The sample this invocation shades of a multisampled texture, as linear light where the texture is sRGB: the samples
// of an sRGB grid carried into a float grid of the same count (ResolveEngineArms.swift's linear-intermediate arm)
fragment float4 resolve_twod_linear_sample(float4 position [[position]], uint sample [[sample_id]],
                                           texture2d_ms<float> grid [[texture(0)]])
{
    return grid.read(uint2(position.xy), sample);
}
