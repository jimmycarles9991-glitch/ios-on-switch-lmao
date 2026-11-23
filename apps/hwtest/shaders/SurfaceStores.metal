#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.xr_texel_buffer_written: one value into every element of a texture buffer, by a kernel
kernel void xr_tbuf_written_kernel(texture_buffer<float, access::write> texels [[texture(0)]],
                                   constant float4 &value [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    if (id < texels.get_width())
        texels.write(value, id);
}

struct SurfaceStoresVertex {
    float4 position [[position]];
};

/// A rectangle given in clip space as (x0, y0, x1, y1), drawn as a four-vertex strip
vertex SurfaceStoresVertex surface_stores_rect(uint vid [[vertex_id]], constant float4 &rect [[buffer(0)]])
{
    return SurfaceStoresVertex{float4(vid & 1 ? rect.z : rect.x, vid >> 1 ? rect.w : rect.y, 0, 1)};
}

/// gpu.gm20b.xr_texel_buffer_written: each fragment writes the value at its column of the texture buffer
fragment float4 xr_tbuf_written_fragment(SurfaceStoresVertex in [[stage_in]],
                                         texture_buffer<float, access::write> texels [[texture(0)]],
                                         constant float4 &value [[buffer(0)]])
{
    texels.write(value, uint(in.position.x));
    return 0;
}

/// gpu.gm20b.ycbcr_turned_load: a draw whose every fragment is discarded, so a loaded pass stores back only what it loaded
fragment float4 ycbcr_turned_load_discard(SurfaceStoresVertex in [[stage_in]])
{
    discard_fragment();
    return 0;
}
