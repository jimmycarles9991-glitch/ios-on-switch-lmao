#include <metal_stdlib>
using namespace metal;

struct MaskedTexel {
    float4 colour [[color(0)]];
    uint mask [[sample_mask]];
};

/// gpu.gm20b.resolve_codes, gpu.gm20b.separate_stencil_resolve: each pixel's value from a buffer of `at.x` a row, written
/// to sample `at.y` alone
fragment MaskedTexel masked_texel_values(float4 position [[position]], device const float4 *values [[buffer(0)]],
                                         constant uint2 &at [[buffer(1)]])
{
    return {values[uint(position.y) * at.x + uint(position.x)], 1u << at.y};
}
