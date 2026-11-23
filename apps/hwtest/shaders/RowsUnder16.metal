#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.xr_rows_under_16: texel i = 2y + x's codes red 300 + 11i, green 520 + 7i, blue 100 + 13i (each (code - 384)
/// / 510) and alpha (16 + 15i) / 255
static float4 rows16_xr_value(uint2 at)
{
    const float i = float(2 * at.y + at.x);
    return float4((float3(300.0f, 520.0f, 100.0f) + float3(11.0f, 7.0f, 13.0f) * i - 384.0f) / 510.0f,
                  (16.0f + 15.0f * i) / 255.0f);
}

fragment float4 rows16_xr_codes(float4 position [[position]])
{
    return rows16_xr_value(uint2(position.xy));
}

kernel void rows16_xr_store(texture2d<float, access::write> t [[texture(0)]], uint2 id [[thread_position_in_grid]])
{
    if (id.x < t.get_width() && id.y < t.get_height())
        t.write(rows16_xr_value(id), id);
}

/// gpu.gm20b.ycbcr_rows_under_16_stores: a colour of each pixel's own, which a YCbCr target stores converted
fragment float4 rows16_ycbcr_colours(float4 position [[position]])
{
    const float2 x = floor(position.xy);
    return float4(0.1f + 0.1f * x.x, 0.9f - 0.1f * x.y, 0.2f + 0.05f * (x.x + x.y), 1.0f);
}

/// gpu.gm20b.ycbcr_rows_under_16_stores: a draw that writes nothing
fragment float4 rows16_discard()
{
    discard_fragment();
    return float4(0);
}
