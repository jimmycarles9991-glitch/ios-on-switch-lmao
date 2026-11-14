#include <metal_stdlib>
using namespace metal;

/// gpu.raw.readback_routes: each target texel is the source's texel at the same position, drawn by the 3D engine the way a
/// widget snapshot's draws write their target
fragment float4 readback_copy_fragment(float4 position [[position]], texture2d<float, access::read> source [[texture(0)]])
{
    return source.read(uint2(position.xy));
}
