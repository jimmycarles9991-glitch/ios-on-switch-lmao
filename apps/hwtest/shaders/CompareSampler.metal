#include <metal_stdlib>
using namespace metal;

// tests/CompareSamplerTest.swift: an 8x8 depth image whose texel (x, y) holds (x + 8y + 0.5) / 64, then one state that
// samples it by comparison and one that samples it plainly, both through the same runtime sampler
struct CompareDepthOut {
    float4 color [[color(0)]];
    float depth [[depth(any)]];
};

fragment CompareDepthOut compare_fill_depth(float4 position [[position]])
{
    const float d = (floor(position.x) + 8.0f * floor(position.y) + 0.5f) / 64.0f;
    return {float4(d), d};
}

fragment float4 compare_sample_compared(float4 position [[position]], depth2d<float> depth [[texture(0)]],
                                        sampler s [[sampler(0)]], constant float &reference [[buffer(0)]])
{
    return float4(depth.sample_compare(s, position.xy / 8.0f, reference));
}

fragment float4 compare_sample_plain(float4 position [[position]], depth2d<float> depth [[texture(0)]],
                                     sampler s [[sampler(0)]])
{
    return float4(depth.sample(s, position.xy / 8.0f));
}
