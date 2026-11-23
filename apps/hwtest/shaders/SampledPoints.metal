#include <metal_stdlib>
using namespace metal;

kernel void sampled_points(texture2d<float> t [[texture(0)]], constant uint2 *at [[buffer(0)]],
                           device float4 *out [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    constexpr sampler nearest(filter::nearest);
    out[i] = t.sample(nearest, (float2(at[i]) + 0.5f) / float2(t.get_width(), t.get_height()), level(0));
}
