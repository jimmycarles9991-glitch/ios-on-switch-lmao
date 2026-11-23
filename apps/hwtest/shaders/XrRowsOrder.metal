#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.xr_descriptor_copies: texel (x, y)'s codes red (37x + 101y + 13s) & 1023, green (53x + 7y + 211s) & 1023,
/// blue (11x + 89y + 97s) & 1023 (each (code - 384) / 510) and alpha (29x + 7y + 5s) & 255, s the seed
kernel void xr_desc_fill(texture2d<float, access::write> t [[texture(0)]], constant uint &seed [[buffer(0)]],
                         uint2 id [[thread_position_in_grid]])
{
    if (id.x >= t.get_width() || id.y >= t.get_height())
        return;
    const uint3 codes = uint3(37 * id.x + 101 * id.y + 13 * seed, 53 * id.x + 7 * id.y + 211 * seed,
                              11 * id.x + 89 * id.y + 97 * seed) & 1023;
    t.write(float4((float3(codes) - 384.0f) / 510.0f, float((29 * id.x + 7 * id.y + 5 * seed) & 255) / 255.0f), id);
}

/// gpu.gm20b.texel_rows: pixel (x, y) of the BGRA8 target, codes red 4x, green 16y, blue 0x80, alpha 0xff
fragment float4 texel_rows_colours(float4 position [[position]])
{
    const float2 p = floor(position.xy);
    return float4(4.0f * p.x, 16.0f * p.y, 128.0f, 255.0f) / 255.0f;
}

/// gpu.gm20b.texel_rows: element k of a BGRA8 texel buffer, codes red k & 255, green 0x5a, blue (3k) & 255, alpha 0xff
static float4 texel_rows_stored(uint k)
{
    return float4(float(k & 255), 90.0f, float((3 * k) & 255), 255.0f) / 255.0f;
}

kernel void texel_rows_kernel_store(texture_buffer<float, access::write> t [[texture(0)]],
                                    uint k [[thread_position_in_grid]])
{
    if (k < t.get_width())
        t.write(texel_rows_stored(k), k);
}

/// gpu.gm20b.texel_rows: each fragment of a one-row draw stores element x, codes red (k + 1) & 255, green 0xa5, blue 7,
/// alpha 0xff, and returns the same
fragment float4 texel_rows_fragment_store(float4 position [[position]],
                                          texture_buffer<float, access::write> t [[texture(0)]])
{
    const uint k = uint(position.x);
    const float4 v = float4(float((k + 1) & 255), 165.0f, 7.0f, 255.0f) / 255.0f;
    if (k < t.get_width())
        t.write(v, k);
    return v;
}

/// gpu.gm20b.turned_overlap_tile_order, gpu.gm20b.xr_turned_clear_order: a colour of each frame pixel's own, inside
/// BGR10_XR's range
fragment float4 turned_order_colours(float4 position [[position]])
{
    const uint2 p = uint2(position.xy);
    const uint3 c = uint3(7 * p.x + 3 * p.y, 5 * p.x + 11 * p.y + 40, 13 * p.x + 2 * p.y + 90) & 255;
    return float4(float3(c) / 255.0f, float((p.x + 3 * p.y) & 255) / 255.0f);
}
