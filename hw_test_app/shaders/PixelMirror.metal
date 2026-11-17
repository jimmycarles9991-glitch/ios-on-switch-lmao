#include <metal_stdlib>
using namespace metal;

// tests/PixelMirrorSamplerTest.swift: a 4x4 texture drawn into the middle of a 12x12 target through the bound sampler at
// pixel coordinates, inside the texture only (Metal defines pixel coordinates outside it under ClampToEdge alone)
fragment float4 pixel_mirror_fragment(float4 position [[position]], texture2d<float> t [[texture(0)]],
                                      sampler s [[sampler(0)]])
{
    return t.sample(s, clamp(position.xy - 4.0f, float2(0.5f), float2(3.5f)));
}
