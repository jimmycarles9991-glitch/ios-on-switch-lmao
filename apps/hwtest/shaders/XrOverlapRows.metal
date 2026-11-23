#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.xr_overlap_rows: texel (x, y) as red code 385 + y, green code 385 + x, blue code 384 and alpha byte y
static float4 xr_overlap_rows_value(uint2 g)
{
    return float4(float(g.y + 1) / 510.0f, float(g.x + 1) / 510.0f, 0.0f, float(g.y) / 255.0f);
}

fragment float4 xr_overlap_rows_frag(float4 position [[position]])
{
    return xr_overlap_rows_value(uint2(position.xy));
}

kernel void xr_overlap_rows_kernel(texture2d<float, access::write> t [[texture(0)]],
                                   uint2 g [[thread_position_in_grid]])
{
    if (g.x < t.get_width() && g.y < t.get_height())
        t.write(xr_overlap_rows_value(g), g);
}
