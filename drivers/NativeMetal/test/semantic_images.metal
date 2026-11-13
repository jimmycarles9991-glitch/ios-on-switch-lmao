#include <metal_stdlib>
using namespace metal;

kernel void semantic_image_read(texture2d<uint, access::read> input [[texture(0)]],
                                device uint *output [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    uint4 v = input.read(uint2(id, 0));
    output[id] = v.x + v.y * 3 + v.z * 5 + v.w * 7 + input.get_width() + input.get_height();
}

kernel void semantic_image_store(texture2d<uint, access::write> output [[texture(0)]],
                                 uint id [[thread_position_in_grid]])
{
    output.write(uint4(id, id + 3, id * 5, 99), uint2(id, 0));
}

kernel void semantic_image_rw(texture2d<uint, access::read_write> image [[texture(0)]],
                              uint id [[thread_position_in_grid]])
{
    uint4 value = image.read(uint2(id, 0));
    image.write(value ^ uint4(3, 5, 7, 9), uint2(id, 0));
}

kernel void semantic_image_sample(texture2d<float> input [[texture(0)]], sampler sample [[sampler(0)]],
                                  device float *output [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    output[id] = input.sample(sample, float2(id, 0) / 8.0f, level(1)).x;
}

kernel void semantic_image_compare(depth2d<float> input [[texture(0)]], sampler sample [[sampler(0)]],
                                   device float *output [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    output[id] = input.sample_compare(sample, float2(id, 0) / 8.0f, 0.25f, level(0));
}

kernel void semantic_image_ms(texture2d_ms<float, access::read> input [[texture(0)]],
                              device float *output [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    output[id] = input.read(uint2(id, 0), 1).x + input.get_num_samples();
}

kernel void semantic_image_array(texture2d_array<uint, access::read> input [[texture(0)]],
                                 device uint *output [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    output[id] = input.read(uint2(id, 0), 1).x + input.get_array_size();
}

kernel void semantic_image_3d(texture3d<uint, access::read> input [[texture(0)]],
                              device uint *output [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    output[id] = input.read(uint3(id, 1, 1)).x + input.get_depth();
}

kernel void semantic_image_gradient(texture2d<float> input [[texture(0)]], sampler sample [[sampler(0)]],
                                    device float *output [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    output[id] = input.sample(sample, float2(id, 0) / 8.0f,
                             gradient2d(float2(0.125f, 0), float2(0, 0.125f))).x;
}

kernel void semantic_image_gather(texture2d<float> input [[texture(0)]], sampler sample [[sampler(0)]],
                                  device float *output [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    output[id] = input.gather(sample, float2(id, 0) / 8.0f).x;
}
