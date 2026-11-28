#include <metal_stdlib>
using namespace metal;

/// One texture and one sampler in an argument buffer, no array (metal.native.argument_encoder one_sampler_draw_path): the
/// pixel samples the texture at u = 1.25, where a repeating sampler reads its left texel and a clamping one its right
struct OneSamplerArguments {
    texture2d<float> texture [[id(0)]];
    sampler sampler [[id(1)]];
};

fragment float4 argument_one_sampler_fragment(constant OneSamplerArguments &arguments [[buffer(0)]])
{
    return arguments.texture.sample(arguments.sampler, float2(1.25, 0.5));
}
