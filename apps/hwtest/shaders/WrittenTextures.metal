#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.written_textures: the pixel's words written to a ushort texture at index 1, as RenderBox's
/// uber_resolve_fragment writes its 565 code to packed_tex. ushort4() keeps each word's low 16 bits.
fragment float4 written_ushort_fragment(float4 at [[position]], device const uint4 *words [[buffer(0)]],
                                        constant uint &width [[buffer(1)]],
                                        texture2d<ushort, access::write> packed [[texture(1)]])
{
    const uint2 p = uint2(at.xy);
    packed.write(ushort4(words[p.y * width + p.x]), p);
    return float4(0);
}

/// The same words whole to a uint texture at index 1
fragment float4 written_uint_fragment(float4 at [[position]], device const uint4 *words [[buffer(0)]],
                                      constant uint &width [[buffer(1)]],
                                      texture2d<uint, access::write> packed [[texture(1)]])
{
    const uint2 p = uint2(at.xy);
    packed.write(words[p.y * width + p.x], p);
    return float4(0);
}

/// gpu.gm20b.written_xr_texture and gpu.gm20b.written_shadowed_surface: the pixel's value written to a texture at index 1 as
/// half, the pass's own colour untouched
fragment float4 written_half_fragment(float4 at [[position]], device const float4 *values [[buffer(0)]],
                                      constant uint &width [[buffer(1)]],
                                      texture2d<half, access::write> written [[texture(1)]])
{
    const uint2 p = uint2(at.xy);
    written.write(half4(values[p.y * width + p.x]), p);
    return float4(0);
}

/// A texture's texel at the pixel, read (no filter)
fragment float4 read_texel_fragment(float4 at [[position]], texture2d<float, access::read> t [[texture(0)]])
{
    return t.read(uint2(at.xy));
}
