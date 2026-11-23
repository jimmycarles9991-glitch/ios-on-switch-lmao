#include <metal_stdlib>
using namespace metal;

// gpu.decode.*: point i of the list as read() gives it, at level levelCount.x (of levelCount.y points)
kernel void decode_read_points(texture2d<float, access::read> t [[texture(0)]], device const uint2 *at [[buffer(0)]],
                               device float4 *out [[buffer(1)]], constant uint2 &levelCount [[buffer(2)]],
                               uint i [[thread_position_in_grid]])
{
    if (i < levelCount.y)
        out[i] = t.read(at[i], levelCount.x);
}

// The same of an array's slice sliceCount.x
kernel void decode_read_slice_points(texture2d_array<float, access::read> t [[texture(0)]],
                                     device const uint2 *at [[buffer(0)]], device float4 *out [[buffer(1)]],
                                     constant uint2 &sliceCount [[buffer(2)]], uint i [[thread_position_in_grid]])
{
    if (i < sliceCount.y)
        out[i] = t.read(at[i], sliceCount.x);
}

// gpu.decode.upload_cost: one sample of the texture at its centre
kernel void decode_sample_once(texture2d<float> t [[texture(0)]], device float4 *out [[buffer(0)]],
                               uint i [[thread_position_in_grid]])
{
    constexpr sampler s(filter::nearest);
    if (i == 0)
        out[0] = t.sample(s, float2(0.5, 0.5));
}
