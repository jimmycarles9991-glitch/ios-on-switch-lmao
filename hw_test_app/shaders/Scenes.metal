#include <metal_stdlib>
using namespace metal;

struct ColoredVertex {
    float4 position [[position]];
    float4 color;
};

/// Instance iid of shape corners[vid]: both ids already include the draw's base vertex and base instance.
vertex ColoredVertex instanced_vertex(uint vid [[vertex_id]], uint iid [[instance_id]],
                                      const device float2 *corners [[buffer(0)]],
                                      const device float2 *offsets [[buffer(1)]],
                                      const device float4 *colors [[buffer(2)]], constant float2 &target [[buffer(3)]])
{
    const float2 p = corners[vid] + offsets[iid];
    return ColoredVertex{float4(p.x / target.x * 2 - 1, 1 - p.y / target.y * 2, 0.5, 1), colors[iid]};
}

fragment float4 color_fragment(ColoredVertex in [[stage_in]])
{
    return in.color;
}

struct DualSource {
    float4 color [[color(0), index(0)]];
    float4 second [[color(0), index(1)]];
};

fragment DualSource dual_fragment(constant float4 *colors [[buffer(0)]])
{
    return DualSource{colors[0], colors[1]};
}

struct TexturedVertex {
    float4 position [[position]];
    float2 uv;
};

vertex TexturedVertex textured_vertex(uint vid [[vertex_id]], constant float4 *corners [[buffer(0)]],
                                      constant float2 *uvs [[buffer(1)]])
{
    return TexturedVertex{corners[vid], uvs[vid]};
}

fragment float4 nearest_fragment(TexturedVertex in [[stage_in]], texture2d<float> texture [[texture(0)]])
{
    constexpr sampler nearest(filter::nearest, address::clamp_to_edge);
    return texture.sample(nearest, in.uv, level(0));
}

fragment float4 linear_fragment(TexturedVertex in [[stage_in]], texture2d<float> texture [[texture(0)]])
{
    constexpr sampler bilinear(filter::linear, address::clamp_to_edge);
    return texture.sample(bilinear, in.uv, level(0));
}

/// A8's coverage as grey, so an R8 or RGBA8 target shows it where R8's .r would be.
fragment float4 alpha_fragment(TexturedVertex in [[stage_in]], texture2d<float> texture [[texture(0)]])
{
    constexpr sampler nearest(filter::nearest, address::clamp_to_edge);
    const float a = texture.sample(nearest, in.uv, level(0)).a;
    return float4(a, a, a, 1);
}

/// Each pair as loaded, then again after a multiply by a one the compiler cannot see: a half that is already the result
/// of half arithmetic reaches the next operation as a half, which is the form the GPU's packed half ALU takes.
kernel void half_arithmetic(device const half4 *a [[buffer(0)]], device const half4 *b [[buffer(1)]],
                            constant half &one [[buffer(2)]], device half4 *out [[buffer(3)]],
                            uint id [[thread_position_in_grid]])
{
    const half4 x = a[id], y = b[id];
    const half4 rx = x * one, ry = y * one;
    out[4 * id] = x + y;
    out[4 * id + 1] = x * y;
    out[4 * id + 2] = rx + ry;
    out[4 * id + 3] = rx * ry;
}
