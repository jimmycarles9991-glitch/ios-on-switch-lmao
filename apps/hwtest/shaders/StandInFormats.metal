#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.xr_texel_buffer_stores: one value into every texel of an extended-range texture buffer
kernel void xr_texel_buffer_store(texture_buffer<float, access::write> texels [[texture(0)]],
                                  constant float4 &value [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    if (id < texels.get_width())
        texels.write(value, id);
}

/// gpu.gm20b.turned_standin_targets: the frame pixel each fragment is, (x/64, y/64)
fragment float4 turned_where_fragment(float4 position [[position]])
{
    return float4(floor(position.x) / 64.0f, floor(position.y) / 64.0f, 0.25f, 1.0f);
}

/// The frame pixel as one code, (y * width + x) / 255 in red (`frame.x` is the frame's width)
fragment float4 turned_index_fragment(float4 position [[position]], constant float4 &frame [[buffer(0)]])
{
    return float4((floor(position.y) * frame.x + floor(position.x)) / 255.0f, 0.0f, 0.0f, 1.0f);
}
