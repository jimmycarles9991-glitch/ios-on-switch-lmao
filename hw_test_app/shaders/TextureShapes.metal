#include <metal_stdlib>
using namespace metal;

// TextureShapesTest: each pixel reads one texel as half, the type Core Animation reads an extended-range texture as
// (gpu.raw.texture_shapes)

struct ShapesVertex {
    float4 position [[position]];
};

// Pixel x reads face x at texel (0, 0)
fragment float4 shapes_cube_face_half(ShapesVertex in [[stage_in]], texturecube<half, access::read> t [[texture(0)]])
{
    return float4(t.read(uint2(0, 0), uint(in.position.x)));
}

// Pixels 0 and 1 read planes 0 and 1 of the first level, pixel 2 plane 0 of the second
fragment float4 shapes_volume_half(ShapesVertex in [[stage_in]], texture3d<half, access::read> t [[texture(0)]])
{
    const uint x = uint(in.position.x);
    return float4(x < 2 ? t.read(uint3(0, 0, x), 0) : t.read(uint3(0, 0, 0), 1));
}
