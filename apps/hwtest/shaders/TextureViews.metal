#include <metal_stdlib>
using namespace metal;

// TextureViewsTest: each pixel reads one texel of a view (gpu.gm20b.texture_views)

struct ViewVertex {
    float4 position [[position]];
};

fragment float4 view_read_2d(ViewVertex in [[stage_in]], texture2d<float, access::read> view [[texture(0)]])
{
    return view.read(uint2(in.position.xy));
}

// Pixel x reads face x at texel (0, 0)
fragment float4 view_read_cube_face(ViewVertex in [[stage_in]], texturecube<float, access::read> view [[texture(0)]])
{
    return view.read(uint2(0, 0), uint(in.position.x));
}

// Pixels 0 and 1 read planes 0 and 1 of the view's first level, pixel 2 plane 0 of its second
fragment float4 view_read_volume(ViewVertex in [[stage_in]], texture3d<float, access::read> view [[texture(0)]])
{
    const uint x = uint(in.position.x);
    return x < 2 ? view.read(uint3(0, 0, x), 0) : view.read(uint3(0, 0, 0), 1);
}

fragment float4 view_read_depth(ViewVertex in [[stage_in]], depth2d<float, access::read> view [[texture(0)]])
{
    return float4(view.read(uint2(in.position.xy)), 0, 0, 1);
}

// Pixel x reads layer x of a depth array view at texel (0, 0)
fragment float4 view_read_depth_layers(ViewVertex in [[stage_in]],
                                       depth2d_array<float, access::read> view [[texture(0)]])
{
    return float4(view.read(uint2(0, 0), uint(in.position.x)), 0, 0, 1);
}

fragment float4 view_read_stencil(ViewVertex in [[stage_in]], texture2d<uint, access::read> view [[texture(0)]])
{
    return float4(float(view.read(uint2(in.position.xy)).r), 0, 0, 1);
}

fragment float4 view_read_array_layer(ViewVertex in [[stage_in]],
                                      texture2d_array<float, access::read> view [[texture(0)]])
{
    return view.read(uint2(in.position.xy), 0);
}

fragment float4 view_read_multisample_layer(ViewVertex in [[stage_in]],
                                            texture2d_ms_array<float, access::read> view [[texture(0)]])
{
    return view.read(uint2(in.position.xy), 0, 0);
}
