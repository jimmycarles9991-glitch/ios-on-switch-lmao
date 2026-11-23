#include <metal_stdlib>
using namespace metal;

struct ScratchColours {
    float4 c0 [[color(0)]];
    float4 c1 [[color(1)]];
};

// A seed's pattern into colour 1 (and colour 0 alike)
fragment ScratchColours scratch_write_fragment(float4 position [[position]], constant uint &seed [[buffer(0)]])
{
    const uint2 at = uint2(position.xy);
    const uint h = (at.x * 73856093u) ^ (at.y * 19349663u) ^ (seed * 83492791u);
    const float4 v = float4(float(h & 255u), float((h >> 8) & 255u), float((h >> 16) & 255u), 255.0f) / 255.0f;
    return {v, v};
}

// Colour 1 as the pass's earlier work left it, stored into colour 0
fragment ScratchColours scratch_fetch_fragment(float4 fetched [[color(1)]])
{
    return {fetched, fetched};
}
