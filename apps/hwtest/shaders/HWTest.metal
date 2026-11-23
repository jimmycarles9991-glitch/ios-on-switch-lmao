#include <metal_stdlib>
using namespace metal;

kernel void multiply_add(device const float *a [[buffer(0)]], device const float *b [[buffer(1)]],
                         device float *out [[buffer(2)]], device atomic_uint *counter [[buffer(3)]],
                         uint id [[thread_position_in_grid]])
{
    out[id] = fma(a[id], b[id], float(id & 1023));
    atomic_fetch_add_explicit(counter, 1, memory_order_relaxed);
}

// Each element folds `value` in, so two dispatches over the same elements leave a result that shows their order
kernel void icb_fold(device uint *data [[buffer(0)]], device const uint &value [[buffer(1)]],
                     uint id [[thread_position_in_grid]])
{
    data[id] = data[id] * 3 + value;
}

kernel void fma_loop(device float4 *data [[buffer(0)]], constant uint &iterations [[buffer(1)]],
                     uint id [[thread_position_in_grid]])
{
    float4 x = data[id];
    float4 y = x * 0.5f + 1.0f;
    for (uint i = 0; i < iterations; ++i) {
        x = fma(x, y, 0.75f);
        y = fma(y, x, -0.25f);
    }
    data[id] = x + y;
}

struct Vertex {
    float4 position [[position]];
};

/// Four corners per quad in clip space, z included, drawn as a triangle strip.
vertex Vertex quad_vertex(uint vid [[vertex_id]], constant float4 *corners [[buffer(0)]])
{
    return Vertex{corners[vid]};
}

fragment float4 solid_fragment(constant float4 &color [[buffer(0)]])
{
    return color;
}

/// A half4 output: Metal's extended-range blend takes one under every factor (m2v fragment/fetch.rs)
fragment half4 solid_half_fragment(constant float4 &color [[buffer(0)]])
{
    return half4(color);
}

/// gpu.compile.shared: a state and a kernel no pack or store holds before the run that salts them (each run's own value)
constant uint compile_salt [[function_constant(0)]];

fragment float4 salted_fragment()
{
    return float4(float(compile_salt & 0xffu) / 255.0, float((compile_salt >> 8) & 0xffu) / 255.0, 0.25, 1.0);
}

kernel void salted_kernel(device uint *out [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    out[id] = id ^ compile_salt;
}

/// Each threadgroup writes its index in the grid, plus one, at that index: a grid past the 65535 threadgroups down or deep
/// one QMD holds (gpu.gm20b.grid_slices)
kernel void grid_positions(device uint *out [[buffer(0)]], uint3 group [[threadgroup_position_in_grid]],
                           uint3 groups [[threadgroups_per_grid]])
{
    const uint at = (group.z * groups.y + group.y) * groups.x + group.x;
    out[at] = at + 1;
}

/// Each element of a buffer bound off 16 bytes, doubled plus one, into another bound off 16 (gpu.gm20b.kernel_window)
kernel void window_float(device const float *src [[buffer(0)]], device float *dst [[buffer(1)]],
                         uint id [[thread_position_in_grid]])
{
    dst[id] = src[id] * 2.0f + 1.0f;
}

kernel void window_float2(device const float2 *src [[buffer(0)]], device float2 *dst [[buffer(1)]],
                          uint id [[thread_position_in_grid]])
{
    dst[id] = src[id] * 2.0f + 1.0f;
}

kernel void window_packed3(device const packed_float3 *src [[buffer(0)]], device packed_float3 *dst [[buffer(1)]],
                           uint id [[thread_position_in_grid]])
{
    dst[id] = float3(src[id]) * 2.0f + 1.0f;
}

/// The texel under each pixel of texture 0 (gpu.gm20b.buffer_texture_rows16)
fragment float4 texel_fragment(float4 position [[position]], texture2d<float> source [[texture(0)]])
{
    return source.read(uint2(position.xy));
}

/// Writes exactly the floats it is given, so the render target's own float-to-format conversion is what is tested.
fragment float4 exact_fragment(constant float4 &value [[buffer(0)]])
{
    return value;
}

kernel void read_uint_view(texture2d<uint, access::read> view [[texture(0)]], device uint *out [[buffer(0)]],
                           uint2 id [[thread_position_in_grid]])
{
    out[id.y * view.get_width() + id.x] = view.read(id).r;
}

kernel void read_float_texture(texture2d<float, access::read> texture [[texture(0)]],
                               device float4 *out [[buffer(0)]], uint2 id [[thread_position_in_grid]])
{
    if (id.x < texture.get_width() && id.y < texture.get_height())
        out[id.y * texture.get_width() + id.x] = texture.read(id);
}

kernel void read_texture_buffer(texture_buffer<float> texels [[texture(0)]], device float4 *out [[buffer(0)]],
                                uint id [[thread_position_in_grid]])
{
    if (id < texels.get_width())
        out[id] = texels.read(id);
}

/// Samples at texel centres with nearest filtering: the only way to read a block-compressed texture back.
kernel void sample_texture(texture2d<float> texture [[texture(0)]], device float4 *out [[buffer(0)]],
                           uint2 id [[thread_position_in_grid]])
{
    constexpr sampler nearest(filter::nearest);
    const float2 size = float2(texture.get_width(), texture.get_height());
    if (id.x < texture.get_width() && id.y < texture.get_height())
        out[id.y * texture.get_width() + id.x] = texture.sample(nearest, (float2(id) + 0.5f) / size, level(0));
}

/// mediaanalysisd's Core Image 4:2:0 conversion on boot 21: luma (slot 0) and chroma (slot 2) written from an RGBA
/// texture sampled at slot 1, every texture's rows 16 bytes off 32 (gpu.gm20b.kernel_shadow)
kernel void shadow_planes(texture2d<float, access::write> luma [[texture(0)]], texture2d<float> rgba [[texture(1)]],
                          texture2d<float, access::write> chroma [[texture(2)]], sampler linear [[sampler(0)]],
                          uint2 id [[thread_position_in_grid]])
{
    if (id.x >= luma.get_width() || id.y >= luma.get_height())
        return;
    const float4 p = rgba.sample(linear, (float2(id) + 0.5f) / float2(rgba.get_width(), rgba.get_height()));
    luma.write(float4(p.g), id);
    if (!(id.x & 1) && !(id.y & 1))
        chroma.write(float4(p.b, p.r, 0, 0), id / 2);
}

/// A two-plane 4:2:0 YCbCr texture sampled through a constexpr clamp-to-edge sampler beside an RGBA texture sampled through
/// a bound one, each at the texel centres of a grid twice the textures' width (gpu.gm20b.kernel_ycbcr_sampler)
kernel void ycbcr_sampler_pairs(texture2d<float> ycbcr [[texture(0)]], texture2d<float> rgba [[texture(1)]],
                                sampler bound [[sampler(0)]], device float4 *out [[buffer(0)]],
                                uint2 id [[thread_position_in_grid]])
{
    constexpr sampler edge(address::clamp_to_edge, filter::nearest);
    const uint width = ycbcr.get_width(), height = ycbcr.get_height();
    if (id.x >= 2 * width || id.y >= height)
        return;
    const float2 uv = (float2(id) + 0.5f) / float2(width, height);
    const uint at = (id.y * 2 * width + id.x) * 2;
    out[at] = ycbcr.sample(edge, uv);
    out[at + 1] = rgba.sample(bound, uv);
}

/// The same texture through constexpr repeating and clamp-to-zero samplers, which Metal reads as clamp to edge on it
/// (gpu.gm20b.kernel_ycbcr_sampler_mapped): three samples a thread
kernel void ycbcr_sampler_mapped(texture2d<float> ycbcr [[texture(0)]], texture2d<float> rgba [[texture(1)]],
                                 sampler bound [[sampler(0)]], device float4 *out [[buffer(0)]],
                                 uint2 id [[thread_position_in_grid]])
{
    constexpr sampler wrap(address::repeat, filter::nearest);
    constexpr sampler zero(address::clamp_to_zero, filter::nearest);
    const uint width = ycbcr.get_width(), height = ycbcr.get_height();
    if (id.x >= 2 * width || id.y >= height)
        return;
    const float2 uv = (float2(id) + 0.5f) / float2(width, height);
    const uint at = (id.y * 2 * width + id.x) * 3;
    out[at] = ycbcr.sample(wrap, uv);
    out[at + 1] = ycbcr.sample(zero, uv);
    out[at + 2] = rgba.sample(bound, uv);
}

/// Both textures through the one bound repeating sampler, which Metal reads as clamp to edge on the YCbCr one alone
/// (gpu.gm20b.kernel_ycbcr_shared_sampler)
kernel void ycbcr_sampler_shared(texture2d<float> ycbcr [[texture(0)]], texture2d<float> rgba [[texture(1)]],
                                 sampler bound [[sampler(0)]], device float4 *out [[buffer(0)]],
                                 uint2 id [[thread_position_in_grid]])
{
    const uint width = ycbcr.get_width(), height = ycbcr.get_height();
    if (id.x >= 2 * width || id.y >= height)
        return;
    const float2 uv = (float2(id) + 0.5f) / float2(width, height);
    const uint at = (id.y * 2 * width + id.x) * 2;
    out[at] = ycbcr.sample(bound, uv);
    out[at + 1] = rgba.sample(bound, uv);
}

/// A kernel's float4 and half4 stores into RGB10A8_2P_XR10 over a two-plane surface, thread i at texel i
/// (gpu.gm20b.xr_kernel_store)
kernel void xr_kernel_store(texture2d<float, access::write> t [[texture(0)]], device const float4 *v [[buffer(0)]],
                            uint i [[thread_position_in_grid]])
{
    t.write(v[i], uint2(i % t.get_width(), i / t.get_width()));
}

kernel void xr_kernel_store_half(texture2d<half, access::write> t [[texture(0)]], device const half4 *v [[buffer(0)]],
                                 uint i [[thread_position_in_grid]])
{
    t.write(v[i], uint2(i % t.get_width(), i / t.get_width()));
}

/// read() and a nearest sample in pixel coordinates of RGB10A8_2P_XR10 over a two-plane surface, the texture at both
/// slots, thread i at texel i (gpu.gm20b.xr_kernel_read)
kernel void xr_kernel_read(texture2d<float, access::read> t [[texture(0)]], texture2d<float> s [[texture(1)]],
                           device float4 *out [[buffer(0)]], uint i [[thread_position_in_grid]])
{
    constexpr sampler nearest(coord::pixel, filter::nearest);
    const uint2 at = uint2(i % t.get_width(), i / t.get_width());
    out[2 * i] = t.read(at);
    out[2 * i + 1] = s.sample(nearest, float2(at) + 0.5f);
}

/// m2v writes a half-float texture through its unsigned integer twin, converting each texel in the shader: the driver
/// binds a header of that format over the texture's memory for these (gpu.gm20b.kernel_view).
kernel void view_half_write(texture2d<half> source [[texture(0)]], texture2d<half, access::write> rgba [[texture(1)]],
                            texture2d<half, access::write> red [[texture(2)]], uint2 id [[thread_position_in_grid]])
{
    constexpr sampler nearest(filter::nearest);
    const float2 size = float2(source.get_width(), source.get_height());
    const half4 s = source.sample(nearest, (float2(id) + 0.5f) / size, level(0));
    rgba.write(s * 2.0h + half4(half(id.x), half(id.y), 0.25h, -1.0h), id);
    red.write(half4(half(id.x + id.y) * 0.5h), id);
}

kernel void view_bgra_write(texture2d<float, access::write> bgra [[texture(0)]], uint2 id [[thread_position_in_grid]])
{
    bgra.write(float4(id.x, id.y, id.x + id.y, 255 - id.x) / 255.0f, id);
}

kernel void view_half_read_write(texture2d<half, access::read_write> texture [[texture(0)]],
                                 uint2 id [[thread_position_in_grid]])
{
    texture.write(texture.read(id) * 0.5h + half4(0.25h, -0.5h, half(id.x), 1.0h), id);
}

/// A colour reached through an argument buffer's pointer (metal.native.argument_encoder).
struct ColourArguments {
    constant float4 *colour [[id(0)]];
};

fragment float4 argument_colour_fragment(constant ColourArguments &arguments [[buffer(0)]])
{
    return *arguments.colour;
}

/// Two textures and a sampler reached through an argument buffer (metal.native.argument_encoder): the target's left half
/// samples the first texture, its right half the second.
struct TextureArguments {
    texture2d<float> first [[id(0)]];
    texture2d<float> second [[id(1)]];
    sampler nearest [[id(2)]];
};

fragment float4 argument_textures_fragment(float4 position [[position]],
                                           constant TextureArguments &arguments [[buffer(0)]])
{
    const float2 centre = float2(0.5);
    return position.x < 4.0 ? arguments.first.sample(arguments.nearest, centre)
                             : arguments.second.sample(arguments.nearest, centre);
}

/// An argument buffer's array of three textures and array of two samplers (metal.native.argument_encoder): column k of the
/// target samples texture k, row 0 through a clamping sampler and row 1 through a repeating one, at u = 1.25, where the clamp
/// reads a texture's right texel and the repeat its left
struct ArrayArguments {
    array<texture2d<float>, 3> textures [[id(0)]];
    array<sampler, 2> samplers [[id(3)]];
};

fragment float4 argument_array_fragment(float4 position [[position]], constant ArrayArguments &arguments [[buffer(0)]])
{
    const uint column = min(uint(position.x), 2u), row = min(uint(position.y), 1u);
    return arguments.textures[column].sample(arguments.samplers[row], float2(1.25, 0.5));
}

/// A buffer slot and an argument buffer's pointer behind one pointer (metal.native.argument_encoder): m2v reaches the slot
/// through the draw's root table too. Pixel 0 reads the slot's colour, pixel 1 the colour behind the argument buffer's pointer
fragment float4 argument_slot_fragment(float4 position [[position]], constant ColourArguments &arguments [[buffer(0)]],
                                       constant float4 *direct [[buffer(1)]])
{
    return *(position.x < 1.0 ? direct : arguments.colour);
}

/// An argument buffer behind an argument buffer's pointer (metal.native.argument_encoder): pixel 0 samples its texture, pixel
/// 1 reads the colour behind its pointer
struct InnerArguments {
    texture2d<float> texture [[id(0)]];
    constant float4 *colour [[id(1)]];
};

struct OuterArguments {
    constant InnerArguments *inner [[id(0)]];
};

fragment float4 argument_nested_fragment(float4 position [[position]], constant OuterArguments &arguments [[buffer(0)]])
{
    constexpr sampler nearest(filter::nearest);
    return position.x < 1.0 ? arguments.inner->texture.sample(nearest, float2(0.5)) : *arguments.inner->colour;
}

/// A kernel reading through an argument buffer (metal.native.argument_encoder): word 0 of its output the colour behind the
/// argument buffer's pointer, word 1 texel (0, 0) of its texture
struct KernelArguments {
    texture2d<float> texture [[id(0)]];
    constant float4 *colour [[id(1)]];
};

kernel void argument_kernel(constant KernelArguments &arguments [[buffer(0)]], device float4 *out [[buffer(1)]])
{
    out[0] = *arguments.colour;
    out[1] = arguments.texture.read(uint2(0, 0));
}

/// A colour behind an argument buffer's pointer plus one float at each of fourteen bind offsets, in red
/// (metal.native.argument_encoder): m2v places the last offsets past the fragment push window in the stage's argument
/// table, which the driver fills
fragment float4 argument_spill_fragment(constant ColourArguments &arguments [[buffer(0)]],
                                        device const float *b1 [[buffer(1)]], device const float *b2 [[buffer(2)]],
                                        device const float *b3 [[buffer(3)]], device const float *b4 [[buffer(4)]],
                                        device const float *b5 [[buffer(5)]], device const float *b6 [[buffer(6)]],
                                        device const float *b7 [[buffer(7)]], device const float *b8 [[buffer(8)]],
                                        device const float *b9 [[buffer(9)]], device const float *b10 [[buffer(10)]],
                                        device const float *b11 [[buffer(11)]], device const float *b12 [[buffer(12)]],
                                        device const float *b13 [[buffer(13)]], device const float *b14 [[buffer(14)]])
{
    return *arguments.colour + float4(b1[0] + b2[0] + b3[0] + b4[0] + b5[0] + b6[0] + b7[0] + b8[0] + b9[0] + b10[0] +
                                          b11[0] + b12[0] + b13[0] + b14[0],
                                      0, 0, 0);
}

struct TintVertex {
    float4 position [[position]];
    float4 tint;
};

/// A colour behind an argument buffer's pointer plus one float at each of thirteen bind offsets, in green, as the quad's
/// tint (metal.native.argument_encoder): m2v places the last offsets past the vertex push window in the stage's argument
/// table
vertex TintVertex spill_vertex(uint vid [[vertex_id]], constant float4 *corners [[buffer(0)]],
                               constant ColourArguments &arguments [[buffer(1)]], device const float *b2 [[buffer(2)]],
                               device const float *b3 [[buffer(3)]], device const float *b4 [[buffer(4)]],
                               device const float *b5 [[buffer(5)]], device const float *b6 [[buffer(6)]],
                               device const float *b7 [[buffer(7)]], device const float *b8 [[buffer(8)]],
                               device const float *b9 [[buffer(9)]], device const float *b10 [[buffer(10)]],
                               device const float *b11 [[buffer(11)]], device const float *b12 [[buffer(12)]],
                               device const float *b13 [[buffer(13)]], device const float *b14 [[buffer(14)]])
{
    return TintVertex{corners[vid], *arguments.colour + float4(0,
                                                               b2[0] + b3[0] + b4[0] + b5[0] + b6[0] + b7[0] + b8[0] +
                                                                   b9[0] + b10[0] + b11[0] + b12[0] + b13[0] + b14[0],
                                                               0, 0)};
}

/// gpu.gm20b.unaligned_buffer_offsets: one float from each of twenty uniform buffers, as the quad's red tint, every one
/// bound off its 256-byte base, so each reads through its bind offset residual, the last ones from the vertex argument
/// table
vertex TintVertex residual_vertex(uint vid [[vertex_id]], constant float4 *corners [[buffer(0)]],
                                  constant float *u1 [[buffer(1)]], constant float *u2 [[buffer(2)]],
                                  constant float *u3 [[buffer(3)]], constant float *u4 [[buffer(4)]],
                                  constant float *u5 [[buffer(5)]], constant float *u6 [[buffer(6)]],
                                  constant float *u7 [[buffer(7)]], constant float *u8 [[buffer(8)]],
                                  constant float *u9 [[buffer(9)]], constant float *u10 [[buffer(10)]],
                                  constant float *u11 [[buffer(11)]], constant float *u12 [[buffer(12)]],
                                  constant float *u13 [[buffer(13)]], constant float *u14 [[buffer(14)]],
                                  constant float *u15 [[buffer(15)]], constant float *u16 [[buffer(16)]],
                                  constant float *u17 [[buffer(17)]], constant float *u18 [[buffer(18)]],
                                  constant float *u19 [[buffer(19)]], constant float *u20 [[buffer(20)]])
{
    const float sum = u1[0] + u2[0] + u3[0] + u4[0] + u5[0] + u6[0] + u7[0] + u8[0] + u9[0] + u10[0] + u11[0] +
                      u12[0] + u13[0] + u14[0] + u15[0] + u16[0] + u17[0] + u18[0] + u19[0] + u20[0];
    return TintVertex{corners[vid], float4(sum, 0, 0, 1)};
}

/// The vertex stage's red beside one float from each of eight storage buffers, in green, each bound off its 16-byte
/// base
fragment float4 residual_fragment(TintVertex in [[stage_in]], device const float *s0 [[buffer(0)]],
                                  device const float *s1 [[buffer(1)]], device const float *s2 [[buffer(2)]],
                                  device const float *s3 [[buffer(3)]], device const float *s4 [[buffer(4)]],
                                  device const float *s5 [[buffer(5)]], device const float *s6 [[buffer(6)]],
                                  device const float *s7 [[buffer(7)]])
{
    const float sum = s0[0] + s1[0] + s2[0] + s3[0] + s4[0] + s5[0] + s6[0] + s7[0];
    return float4(in.tint.x, sum, 0, 1);
}

/// gpu.gm20b.vertex_cubic_sampler: one point per sample, each the texel a vertex function reads through the bound sampler at
/// its own coordinate
struct CubicPoint {
    float4 position [[position]];
    float size [[point_size]];
    float4 colour;
};

vertex CubicPoint cubic_vertex(uint vid [[vertex_id]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]],
                               constant float *u [[buffer(0)]], constant uint &count [[buffer(1)]])
{
    const float x = (float(vid) + 0.5) / float(count) * 2 - 1;
    return CubicPoint{float4(x, 0, 0, 1), 1, t.sample(s, float2(u[vid], 0.5), level(0))};
}

fragment float4 cubic_point_fragment(CubicPoint in [[stage_in]])
{
    return in.colour;
}

/// The same samples read by a fragment function, pixel x reading coordinate x
fragment float4 cubic_row_fragment(float4 position [[position]], texture2d<float> t [[texture(0)]],
                                   sampler s [[sampler(0)]], constant float *u [[buffer(0)]])
{
    return t.sample(s, float2(u[uint(position.x)], 0.5), level(0));
}

fragment float4 tint_fragment(TintVertex in [[stage_in]])
{
    return in.tint;
}

/// spill_vertex's green beside a colour behind an argument buffer's pointer, in blue, and one float at each of fourteen bind
/// offsets, in red (metal.native.argument_encoder): both stages of one draw place bind offsets past their push windows in
/// their argument tables
fragment float4 spill_tint_fragment(TintVertex in [[stage_in]], constant ColourArguments &arguments [[buffer(0)]],
                                    device const float *b1 [[buffer(1)]], device const float *b2 [[buffer(2)]],
                                    device const float *b3 [[buffer(3)]], device const float *b4 [[buffer(4)]],
                                    device const float *b5 [[buffer(5)]], device const float *b6 [[buffer(6)]],
                                    device const float *b7 [[buffer(7)]], device const float *b8 [[buffer(8)]],
                                    device const float *b9 [[buffer(9)]], device const float *b10 [[buffer(10)]],
                                    device const float *b11 [[buffer(11)]], device const float *b12 [[buffer(12)]],
                                    device const float *b13 [[buffer(13)]], device const float *b14 [[buffer(14)]])
{
    return float4(b1[0] + b2[0] + b3[0] + b4[0] + b5[0] + b6[0] + b7[0] + b8[0] + b9[0] + b10[0] + b11[0] + b12[0] +
                      b13[0] + b14[0],
                  in.tint.y, arguments.colour->z, 1);
}

/// A colour behind an argument buffer's pointer plus one float from each of twenty-eight buffers, in red: m2v places the
/// last bind offsets past the kernel's push window in its argument table
kernel void argument_spill_kernel(constant ColourArguments &arguments [[buffer(0)]], device float4 *out [[buffer(1)]],
                                  device const float *b2 [[buffer(2)]], device const float *b3 [[buffer(3)]],
                                  device const float *b4 [[buffer(4)]], device const float *b5 [[buffer(5)]],
                                  device const float *b6 [[buffer(6)]], device const float *b7 [[buffer(7)]],
                                  device const float *b8 [[buffer(8)]], device const float *b9 [[buffer(9)]],
                                  device const float *b10 [[buffer(10)]], device const float *b11 [[buffer(11)]],
                                  device const float *b12 [[buffer(12)]], device const float *b13 [[buffer(13)]],
                                  device const float *b14 [[buffer(14)]], device const float *b15 [[buffer(15)]],
                                  device const float *b16 [[buffer(16)]], device const float *b17 [[buffer(17)]],
                                  device const float *b18 [[buffer(18)]], device const float *b19 [[buffer(19)]],
                                  device const float *b20 [[buffer(20)]], device const float *b21 [[buffer(21)]],
                                  device const float *b22 [[buffer(22)]], device const float *b23 [[buffer(23)]],
                                  device const float *b24 [[buffer(24)]], device const float *b25 [[buffer(25)]],
                                  device const float *b26 [[buffer(26)]], device const float *b27 [[buffer(27)]],
                                  device const float *b28 [[buffer(28)]], device const float *b29 [[buffer(29)]])
{
    out[0] =
        *arguments.colour + float4(b2[0] + b3[0] + b4[0] + b5[0] + b6[0] + b7[0] + b8[0] + b9[0] + b10[0] + b11[0] +
                                       b12[0] + b13[0] + b14[0] + b15[0] + b16[0] + b17[0] + b18[0] + b19[0] + b20[0] +
                                       b21[0] + b22[0] + b23[0] + b24[0] + b25[0] + b26[0] + b27[0] + b28[0] + b29[0],
                                   0, 0, 0);
}

/// tests/KernelYcbcrOnePlaneTest.swift: a linear sample a quarter texel left of each texel's centre, so chroma pairs
/// mix
kernel void sample_texture_linear(texture2d<float> texture [[texture(0)]], device float4 *out [[buffer(0)]],
                                  uint2 id [[thread_position_in_grid]])
{
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    const float2 size = float2(texture.get_width(), texture.get_height());
    const float2 at = (float2(id) + float2(0.25f, 0.5f)) / size;
    if (id.x < texture.get_width() && id.y < texture.get_height())
        out[id.y * texture.get_width() + id.x] = texture.sample(linear, at, level(0));
}

/// tests/KernelConvertedWriteTest.swift: a store into every texel of a converted YCbCr view, which Metal drops
kernel void write_converted_texture(texture2d<float, access::write> texture [[texture(0)]],
                                    uint2 id [[thread_position_in_grid]])
{
    if (id.x < texture.get_width() && id.y < texture.get_height())
        texture.write(float4(0.25f, 0.5f, 0.75f, 1), id);
}

/// tests/KernelConvertedWriteTest.swift: reads of whatever texture is bound, its size unchecked (none bound reads 0)
kernel void read_bound_texture(texture2d<float, access::read> texture [[texture(0)]], device float4 *out [[buffer(0)]],
                               uint id [[thread_position_in_grid]])
{
    out[id] = texture.read(uint2(id % 4, id / 4));
}
