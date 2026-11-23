#include <metal_stdlib>
using namespace metal;

fragment float4 standin_sample_values(float4 position [[position]], uint sample [[sample_id]],
                                      device const float4 *values [[buffer(0)]], constant uint2 &shape [[buffer(1)]])
{
    return values[(uint(position.y) * shape.x + uint(position.x)) * shape.y + sample];
}

struct EightTargetValues {
    float4 c0 [[color(0)]];
    float4 c1 [[color(1)]];
    float4 c2 [[color(2)]];
    float4 c3 [[color(3)]];
    float4 c4 [[color(4)]];
    float4 c5 [[color(5)]];
    float4 c6 [[color(6)]];
    float4 c7 [[color(7)]];
};

fragment EightTargetValues eight_target_values(float4 position [[position]], device const float4 *values [[buffer(0)]],
                                               constant uint &width [[buffer(1)]])
{
    const float4 v = values[uint(position.y) * width + uint(position.x)];
    return {v, v, v, v, v, v, v, v};
}

fragment void zeta_no_colour()
{
}

struct ZetaSampleDepth {
    float depth [[depth(any)]];
};

fragment ZetaSampleDepth zeta_depth_by_sample(uint sample [[sample_id]], constant float *depths [[buffer(0)]])
{
    return {depths[sample]};
}
