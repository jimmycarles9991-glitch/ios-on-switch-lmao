#include <metal_stdlib>
using namespace metal;

kernel void neutral_read(texture2d<float, access::read> image [[texture(0)]],
                         device float4 *output [[buffer(0)]])
{
    output[0] = image.read(uint2(0));
}

kernel void neutral_sample(texture2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]],
                           device float4 *output [[buffer(0)]])
{
    output[0] = image.sample(filtering, float2(0.5));
}

kernel void neutral_store(texture2d<float, access::write> image [[texture(0)]])
{
    image.write(float4(1), uint2(0));
}

kernel void neutral_compare(depth2d<float> image [[texture(0)]], sampler filtering [[sampler(0)]],
                            device float *output [[buffer(0)]])
{
    output[0] = image.sample_compare(filtering, float2(0.5), 0.5);
}
