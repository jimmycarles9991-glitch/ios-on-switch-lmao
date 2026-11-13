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

/// The four texels' alpha, then red, about the texel corner nearest uv (Metal's gather)
fragment float4 gather_alpha_fragment(TexturedVertex in [[stage_in]], texture2d<float> texture [[texture(0)]])
{
    constexpr sampler nearest(filter::nearest, address::clamp_to_edge);
    return texture.gather(nearest, in.uv, int2(0), component::w);
}

fragment float4 gather_red_fragment(TexturedVertex in [[stage_in]], texture2d<float> texture [[texture(0)]])
{
    constexpr sampler nearest(filter::nearest, address::clamp_to_edge);
    return texture.gather(nearest, in.uv, int2(0), component::x);
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
                            device const half4 *c [[buffer(4)]],
                            uint id [[thread_position_in_grid]])
{
    const half4 x = a[id], y = b[id];
    const half4 rx = x * one, ry = y * one;
    out[5 * id] = x + y;
    out[5 * id + 1] = x * y;
    out[5 * id + 2] = rx + ry;
    out[5 * id + 3] = rx * ry;
    out[5 * id + 4] = fma(x, y, c[id]);
}

/// NAK lowers a shader the SM53 probe's way only if it holds this value (nak/opt_sm53_half_probe.rs, debug builds): the
/// probe kernels compare their thread index with it, which no dispatch reaches
constant uint SM53_PROBE_MARKER = 0x7fa55353u;
constant uint SM53_PROOF_MARKER = 0x7fa55354u;

kernel void sm53_probe_io(device const ushort *a [[buffer(0)]], device const ushort *b [[buffer(1)]],
                          device const ushort *c [[buffer(2)]], device uint4 *out [[buffer(3)]],
                          uint id [[thread_position_in_grid]])
{
    out[id] = uint4(a[id] | (uint(b[id]) << 16), c[id], id, 0x5353494fu);
}

/// gpu.sm53_probe: each form NAK lowers to SM53's half ALU with an f32 result. The first two hold if the op rounds to
/// half before it widens, the last three if it rounds the exact result once to f32.
kernel void sm53_probe(device const half *a [[buffer(0)]], device const half *b [[buffer(1)]],
                       device const half *c [[buffer(2)]], device float *out [[buffer(3)]],
                       device uint *proof [[buffer(4)]],
                       uint id [[thread_position_in_grid]])
{
    if (id == SM53_PROBE_MARKER)
        return;
    if (id == 0)
        proof[0] = SM53_PROOF_MARKER;
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
                            device float *roundTrip [[buffer(2)]], device uint *proof [[buffer(4)]],
                            uint id [[thread_position_in_grid]])
{
    if (id == SM53_PROBE_MARKER)
        return;
    if (id == 0)
        proof[0] = SM53_PROOF_MARKER;
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

/// NAK takes TEXS's F16 forms for a shader holding this value in debug builds, whatever the chip facts say
/// (nak_nir_lower_tex_sm50.c), and stamps TEXS_F16_PROOF_MARKER with 0x5346 << 16 | the fetches it folded
constant uint TEXS_F16_PROBE_MARKER = 0x7fa55346u;
constant uint TEXS_F16_PROOF_MARKER = 0x7fa55347u;

struct TexsF16In {
    float4 position [[position]];
};

/// The F16 form's bits as whole floats and the proof word's bits as a float: formats the raw path renders (RGBA32Float,
/// R32Float), each holding every bit
struct TexsF16Out {
    float4 f32 [[color(0)]];
    float4 f16 [[color(1)]];
    float proof [[color(2)]];
};

/// gpu.raw.texs_f16: one texel, sampled through two names of the same texture (bound textures, never one fetch), at the
/// pixel's centre plus `offset` texels: TEXS's F32 form, and its F16 form's bits. One function per filter, so each
/// fetch's only use is the rounding NAK folds.
static TexsF16Out texs_f16(TexsF16In in, texture2d<float> first, texture2d<float> second, float2 offset, sampler s)
{
    const float2 at = in.position.xy + offset;
    TexsF16Out out;
    out.f32 = first.sample(s, at);
    out.f16 = float4(as_type<ushort4>(half4(second.sample(s, at))));
    out.proof = as_type<float>(as_type<uint>(at.x) == TEXS_F16_PROBE_MARKER ? 0u : TEXS_F16_PROOF_MARKER);
    return out;
}

fragment TexsF16Out texs_f16_point(TexsF16In in [[stage_in]], texture2d<float> first [[texture(0)]],
                                   texture2d<float> second [[texture(1)]], constant float2 &offset [[buffer(0)]])
{
    constexpr sampler point(coord::pixel, filter::nearest, address::clamp_to_edge);
    return texs_f16(in, first, second, offset, point);
}

fragment TexsF16Out texs_f16_linear(TexsF16In in [[stage_in]], texture2d<float> first [[texture(0)]],
                                    texture2d<float> second [[texture(1)]], constant float2 &offset [[buffer(0)]])
{
    constexpr sampler bilinear(coord::pixel, filter::linear, address::clamp_to_edge);
    return texs_f16(in, first, second, offset, bilinear);
}

/// gpu.raw.texs_f16: the same fetches by a compute kernel, whose textures stay bindless (TEX.B)
kernel void texs_f16_bindless(texture2d<float> first [[texture(0)]], device float4 *out [[buffer(0)]],
                              constant float2 &offset [[buffer(1)]], constant uint &linear [[buffer(2)]],
                              uint2 id [[thread_position_in_grid]])
{
    constexpr sampler point(coord::pixel, filter::nearest, address::clamp_to_edge);
    constexpr sampler bilinear(coord::pixel, filter::linear, address::clamp_to_edge);
    const float2 at = float2(id) + 0.5f + offset;
    out[id.y * first.get_width() + id.x] = linear ? first.sample(bilinear, at) : first.sample(point, at);
}

/// gpu.raw.stored_msaa: sample x % count of pixel (x / count, y) of a multisampled texture, its samples side by side
fragment float4 ms_sample_fragment(float4 position [[position]], texture2d_ms<float> samples [[texture(0)]],
                                   constant uint &count [[buffer(0)]])
{
    const uint2 at = uint2(position.xy);
    return samples.read(uint2(at.x / count, at.y), at.x % count);
}

/// The same of a multisampled depth texture, in red
fragment float4 ms_depth_fragment(float4 position [[position]], depth2d_ms<float> depth [[texture(0)]],
                                  constant uint &count [[buffer(0)]])
{
    const uint2 at = uint2(position.xy);
    return float4(depth.read(uint2(at.x / count, at.y), at.x % count), 0, 0, 1);
}
