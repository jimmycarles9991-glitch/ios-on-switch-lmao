#include <metal_stdlib>
using namespace metal;

// tests/AlphaToCoverageTest.swift: white with alpha (x + 0.5) / width, so each column of a 256-wide target carries one
// alpha step of 1/256
fragment float4 a2c_ramp_fragment(float4 position [[position]], constant float &width [[buffer(0)]])
{
    return float4(1.0, 1.0, 1.0, position.x / width);
}
