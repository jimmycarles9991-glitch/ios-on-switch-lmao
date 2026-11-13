#include <metal_stdlib>
using namespace metal;

// gpu.msaa.resolve_cost's arms: the same 720x1280 output read four ways, to split a 4x resolve's cost between its
// multisampled fetches (four against one), the sample grid's layout (the same texels as a one-sample 1440x2560 texture,
// block-linear or pitch) and the resolve program's own instructions (these average in floats, with no conversions).

struct ArmVertex {
    float4 position [[position]];
};

vertex ArmVertex arms_vertex(uint vid [[vertex_id]])
{
    const float2 corner = float2(vid == 1 ? 3.0f : -1.0f, vid == 2 ? 3.0f : -1.0f);
    return {float4(corner, 0, 1)};
}

fragment float4 arms_ms4_frag(ArmVertex in [[stage_in]], texture2d_ms<float, access::read> s [[texture(0)]])
{
    const uint2 at = uint2(in.position.xy);
    return (s.read(at, 0) + s.read(at, 1) + s.read(at, 2) + s.read(at, 3)) * 0.25f;
}

fragment float4 arms_ms1_frag(ArmVertex in [[stage_in]], texture2d_ms<float, access::read> s [[texture(0)]])
{
    return s.read(uint2(in.position.xy), 0);
}

fragment float4 arms_grid4_frag(ArmVertex in [[stage_in]], texture2d<float, access::read> s [[texture(0)]])
{
    const uint2 at = uint2(in.position.xy) * 2;
    return (s.read(at) + s.read(at + uint2(1, 0)) + s.read(at + uint2(0, 1)) + s.read(at + uint2(1, 1))) * 0.25f;
}
