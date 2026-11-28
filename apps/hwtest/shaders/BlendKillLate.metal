#include <metal_stdlib>
using namespace metal;

struct BlendKillDepth {
    float4 colour [[color(0)]];
    float depth [[depth(any)]];
};

/// gpu.gm20b.blend_kill late_depth: `value`'s colour at the depth `depth` the program writes, so the depth test runs late
fragment BlendKillDepth blend_kill_depth_fragment(constant float4 &value [[buffer(0)]],
                                                  constant float &depth [[buffer(1)]])
{
    return {value, depth};
}

/// gpu.gm20b.blend_kill late_discard: `value`, from a program that can discard (never for a colour above -1 in alpha),
/// so the depth test runs late
fragment float4 blend_kill_discard_fragment(constant float4 &value [[buffer(0)]])
{
    if (value.w < -1.0f)
        discard_fragment();
    return value;
}
