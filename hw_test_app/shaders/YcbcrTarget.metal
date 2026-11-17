#include <metal_stdlib>
using namespace metal;

/// Each pixel's own value from a buffer of `width` a row: the floats a two-plane YCbCr attachment holds, exactly as given
/// (gpu.ycbcr_target).
fragment float4 texel_values(float4 position [[position]], device const float4 *values [[buffer(0)]],
                             constant uint &width [[buffer(1)]])
{
    return values[uint(position.y) * width + uint(position.x)];
}

/// The same values as halves, which the attachment keeps as they are.
fragment half4 texel_halves(float4 position [[position]], device const float4 *values [[buffer(0)]],
                            constant uint &width [[buffer(1)]])
{
    return half4(values[uint(position.y) * width + uint(position.x)]);
}

/// A texture's texels as a draw reads them (gpu.ycbcr_packed_target: a packed YCbCr texture against its unpacked twin).
fragment float4 texel_read(float4 position [[position]], texture2d<float, access::read> t [[texture(0)]])
{
    return t.read(uint2(position.xy));
}

struct TwoTexels {
    float4 first [[color(0)]];
    float4 second [[color(1)]];
};

/// Each pixel's value at two attachments, the second's channels the first's turned (gpu.ycbcr_corner: cameracaptured's
/// passes into two YCbCr targets at once).
fragment TwoTexels texel_values_two(float4 position [[position]], device const float4 *values [[buffer(0)]],
                                    constant uint &width [[buffer(1)]])
{
    const float4 v = values[uint(position.y) * width + uint(position.x)];
    return {v, v.zxyw};
}
