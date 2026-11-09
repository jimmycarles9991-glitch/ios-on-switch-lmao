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

/// gpu.xr_blend: row y's S in red, green and blue and its source1 alpha (XRBlendScene.row), colour alpha one
fragment DualSource xr_blend_fragment(float4 position [[position]], constant float4 *rows [[buffer(0)]])
{
    const float4 row = rows[uint(position.y)];
    return DualSource{float4(row.rgb, 1), float4(0, 0, 0, row.a)};
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

/// NAK lowers a shader the SM53 probe's way only if it holds this value (nak/opt_sm53_half_probe.rs, debug builds): the
/// probe kernels compare their thread index with it, which no dispatch reaches
constant uint SM53_PROBE_MARKER = 0x7fa55353u;

/// gpu.sm53_probe: each form NAK lowers to SM53's half ALU with an f32 result. The first two hold if the op rounds to
/// half before it widens, the last three if it rounds the exact result once to f32.
kernel void sm53_probe(device const half *a [[buffer(0)]], device const half *b [[buffer(1)]],
                       device const half *c [[buffer(2)]], device float *out [[buffer(3)]],
                       uint id [[thread_position_in_grid]])
{
    if (id == SM53_PROBE_MARKER)
        return;
    const half x = a[id], y = b[id], z = c[id];
    const half sum = x + y, product = x * y;
    out[5 * id] = float(sum);
    out[5 * id + 1] = float(product);
    out[5 * id + 2] = float(x) + float(y);
    out[5 * id + 3] = float(x) * float(y);
    out[5 * id + 4] = fma(float(x), float(y), float(z));
}

/// gpu.sm53_probe: an f32 narrowed to half and its round trip through half, which NAK emits as one HADD2 each, reading
/// the f32 through HADD2's F32 swizzle
kernel void sm53_f32_source(device const float *x [[buffer(0)]], device half *narrowed [[buffer(1)]],
                            device float *roundTrip [[buffer(2)]], uint id [[thread_position_in_grid]])
{
    if (id == SM53_PROBE_MARKER)
        return;
    const half h = half(x[id]);
    narrowed[id] = h;
    roundTrip[id] = float(h);
}

/// gpu.sm53_probe: the half of bits row * 256 + column (a NaN at the last pixel) written as computed, which a blend
/// of other factors than one and zero keeps from metal2vulkan's unorm rounding: the device's own conversion
fragment half4 sm53_probe_unorm(float4 position [[position]])
{
    const uint i = uint(position.y) * 256 + uint(position.x);
    const ushort bits = i == 32767 ? ushort(0x7e00) : i < 16384 ? ushort(i) : ushort(0x8000 | (i - 16384));
    return half4(as_type<half>(bits));
}

/// gpu.draw.linked_function: the colour comes from custom_fn, which the pipeline links (RenderBox's icon effects call their
/// effect so)
[[visible]] float4 custom_fn(float4 color);

fragment float4 linked_fragment()
{
    return custom_fn(float4(1, 0, 0, 1));
}

constant uint linked_variant [[function_constant(1)]];

/// What the scene links as custom_fn, specialized with linked_variant: its colour, or that colour's blue and red swapped
[[visible]] float4 linked_effect_v1(float4 color)
{
    return linked_variant == 2 ? color.zyxw : color;
}

/// Corners and coordinates through device pointers: storage buffers, which a caller may bind at any offset its element
/// alignment allows (RenderBox's at 8 past a multiple of 16).
vertex TexturedVertex device_textured_vertex(uint vid [[vertex_id]], device const float2 *corners [[buffer(0)]],
                                             device const float *uvs [[buffer(1)]])
{
    return TexturedVertex{float4(corners[vid], 0.5, 1), float2(uvs[2 * vid], uvs[2 * vid + 1])};
}

/// The bound sampler's reading, whatever its coordinates.
fragment float4 sampled_fragment(TexturedVertex in [[stage_in]], texture2d<float> texture [[texture(0)]],
                                 sampler bound [[sampler(0)]])
{
    return texture.sample(bound, in.uv, level(0));
}
