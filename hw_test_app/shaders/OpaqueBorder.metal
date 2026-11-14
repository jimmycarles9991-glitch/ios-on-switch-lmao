#include <metal_stdlib>
using namespace metal;

// tests/OpaqueBorderTest.swift: a 4x4 texture drawn into the middle of a 12x12 target through the bound sampler, the
// ring of four pixels around it sampled past the texture's edge
fragment float4 opaque_border_fragment(float4 position [[position]], texture2d<float> t [[texture(0)]],
                                       sampler s [[sampler(0)]])
{
    return t.sample(s, (position.xy - 4.0f) / 4.0f);
}
