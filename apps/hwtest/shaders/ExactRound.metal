#include <metal_stdlib>
using namespace metal;

/// gpu.compute.exact_round: each value through every float pack: unorm4x8, snorm4x8, unorm2x16, snorm2x16, unorm10a2
kernel void exact_round_packs(device const float *values [[buffer(0)]], device uint *out [[buffer(1)]],
                              uint i [[thread_position_in_grid]])
{
    const float v = values[i];
    out[i * 5] = pack_float_to_unorm4x8(float4(v));
    out[i * 5 + 1] = pack_float_to_snorm4x8(float4(v));
    out[i * 5 + 2] = pack_float_to_unorm2x16(float2(v));
    out[i * 5 + 3] = pack_float_to_snorm2x16(float2(v));
    out[i * 5 + 4] = pack_float_to_unorm10a2(float4(v));
}

/// Texel id written from `values` (a row of the texture's width) as floats
kernel void exact_round_write_float(texture2d<float, access::write> t [[texture(0)]],
                                    device const float4 *values [[buffer(0)]], uint2 id [[thread_position_in_grid]])
{
    t.write(values[id.y * t.get_width() + id.x], id);
}

/// The same as halves
kernel void exact_round_write_half(texture2d<half, access::write> t [[texture(0)]],
                                   device const float4 *values [[buffer(0)]], uint2 id [[thread_position_in_grid]])
{
    t.write(half4(values[id.y * t.get_width() + id.x]), id);
}
