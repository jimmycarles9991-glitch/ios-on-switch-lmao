#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.xr_planes_rows_off32: each texel read and written back with alpha 1
kernel void xr_rewrite_alpha(texture2d<float, access::read_write> t [[texture(0)]], uint2 id [[thread_position_in_grid]])
{
    t.write(float4(t.read(id).rgb, 1.0), id);
}

/// gpu.gm20b.xr_next_plane, gpu.gm20b.xr_alpha_sink, gpu.gm20b.xr_tiled_surface: one value written to every texel
kernel void xr_fill_kernel(texture2d<float, access::write> t [[texture(0)]], constant float4 &value [[buffer(0)]],
                           uint2 id [[thread_position_in_grid]])
{
    if (id.x < t.get_width() && id.y < t.get_height())
        t.write(value, id);
}
