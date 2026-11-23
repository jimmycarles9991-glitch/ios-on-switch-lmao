#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.planes_copies_open: texel (x, y)'s codes red 128 + 3x + 40y, green 300 + 7x + 11y, blue 600 + x + 50y
/// (each (code - 384) / 510) and alpha ((5 + 7x + 29y) & 255) / 255
kernel void forms_open_xr_store(texture2d<float, access::write> t [[texture(0)]], uint2 id [[thread_position_in_grid]])
{
    if (id.x >= t.get_width() || id.y >= t.get_height())
        return;
    const float2 p = float2(id);
    const float3 codes = float3(128.0f + 3.0f * p.x + 40.0f * p.y, 300.0f + 7.0f * p.x + 11.0f * p.y,
                                600.0f + p.x + 50.0f * p.y);
    t.write(float4((codes - 384.0f) / 510.0f, float((5 + 7 * id.x + 29 * id.y) & 255) / 255.0f), id);
}

/// gpu.gm20b.xr_texel_buffer_tiled: a colour of each pixel's own in BGR10_XR's range
fragment float4 forms_open_pixel_colours(float4 position [[position]])
{
    const float2 p = floor(position.xy);
    return float4(-0.5f + p.x / 40.0f, 1.0f - p.y / 50.0f, 0.25f + (p.x + p.y) / 160.0f, 1.0f);
}

/// gpu.gm20b.packed_mips: the width, height and level count a shader sees of each level of a texture, then 0
kernel void forms_open_levels(texture2d<float, access::read> t [[texture(0)]], device uint *out [[buffer(0)]],
                              uint i [[thread_position_in_grid]])
{
    if (i > 0)
        return;
    out[0] = t.get_num_mip_levels();
    for (uint level = 0; level < 3; level++) {
        out[1 + 2 * level] = level < t.get_num_mip_levels() ? t.get_width(level) : 0;
        out[2 + 2 * level] = level < t.get_num_mip_levels() ? t.get_height(level) : 0;
    }
}
