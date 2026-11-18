#include <metal_stdlib>
using namespace metal;

/// gpu.raw.mip_twod's 3D route: a level from the one above (texture 0) by one bilinear tap at the pixel's centre mapped onto
/// it, as Metal's generateMipmaps samples; buffer 0 holds the reciprocal of the level's size
fragment float4 mip_step_fragment(float4 position [[position]], texture2d<float> above [[texture(0)]],
                                  constant float2 &inverseSize [[buffer(0)]])
{
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    return above.sample(linear, position.xy * inverseSize, level(0));
}
