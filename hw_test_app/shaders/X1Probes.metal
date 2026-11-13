#include <metal_stdlib>
using namespace metal;

/// gpu.x1.rop_conversion: the half of bits row * 256 + column, each of the 65,536 (NaN, infinities and negatives
/// included), in every channel
fragment half4 x1_half_bits(float4 position [[position]])
{
    return half4(as_type<half>(ushort(uint(position.y) * 256 + uint(position.x))));
}

/// gpu.x1.sampler_conversion: each texel as a nearest sample at its centre returns it as half
kernel void x1_sample_half(texture2d<half> texture [[texture(0)]], device ushort4 *out [[buffer(0)]],
                           uint2 id [[thread_position_in_grid]])
{
    constexpr sampler nearest(filter::nearest);
    const float2 size = float2(texture.get_width(), texture.get_height());
    if (id.x < texture.get_width() && id.y < texture.get_height())
        out[id.y * texture.get_width() + id.x] =
            as_type<ushort4>(texture.sample(nearest, (float2(id) + 0.5f) / size, level(0)));
}

/// gpu.x1.blend_exact: pixel y * 4096 + x draws case (y * 4096 + x) / 256 of `pairs` (indices of `halves`: the
/// colour, then the alpha) as (s, s, s, sa)
fragment half4 x1_blend_source(float4 position [[position]], device const ushort2 *pairs [[buffer(0)]],
                               device const half *halves [[buffer(1)]])
{
    const uint p = uint(position.y) * 4096 + uint(position.x);
    const ushort2 pair = pairs[min(p / 256, 32895u)];
    const half s = halves[pair.x];
    return half4(s, s, s, halves[pair.y]);
}

/// gpu.x1.iterated_blend: a premultiplied ramp, red across and alpha down
fragment half4 x1_ramp_source(float4 position [[position]])
{
    const float u = float(uint(position.x)) / 255.0f, a = float(uint(position.y)) / 255.0f;
    return half4(float4(u * a, (1.0f - u) * a, 0.5f * a, a));
}

/// gpu.x1.f32_filter: one linear sample of `texture` at `uv`
kernel void x1_sample_linear(texture2d<float> texture [[texture(0)]], constant float2 &uv [[buffer(0)]],
                             device float4 *out [[buffer(1)]])
{
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    out[0] = texture.sample(linear, uv, level(0));
}

/// metal.float32_filtering: linear samples of a 32-bit float texture at (u, v, lod) triples, mip levels blended linearly
kernel void float32_filter_sample(texture2d<float> texture [[texture(0)]], constant float4 *points [[buffer(0)]],
                                  device float4 *out [[buffer(1)]], uint i [[thread_position_in_grid]])
{
    constexpr sampler trilinear(filter::linear, mip_filter::linear, address::clamp_to_edge);
    out[i] = texture.sample(trilinear, points[i].xy, level(points[i].z));
}

struct X1ColoredVertex {
    float4 position [[position]];
    float4 color;
};

/// gpu.x1.viewport_swizzle: rectangles of six vertices each, one colour a rectangle
vertex X1ColoredVertex x1_colored_vertex(uint vid [[vertex_id]], device const float4 *corners [[buffer(0)]],
                                         device const float4 *colors [[buffer(1)]])
{
    return X1ColoredVertex{corners[vid], colors[vid / 6]};
}

fragment float4 x1_colored_fragment(X1ColoredVertex in [[stage_in]])
{
    return in.color;
}

/// The barrier probes' texel hash (HWTestCore probeHash)
static uint x1_hash(uint x, uint y, uint seed)
{
    uint v = (x * 73856093u) ^ (y * 19349663u) ^ (seed * 83492791u);
    v ^= v >> 13;
    v *= 0x5bd1e995u;
    v ^= v >> 15;
    return v;
}

struct X1Seeded {
    uint seed, spin;
    float zero;
    uint pad;
};

/// `spin` rounds of work that come to a runtime zero, so a seeded draw takes as long as the probe asks
static float x1_spin(uint2 p, constant X1Seeded &s)
{
    uint h = p.x ^ (p.y << 16);
    float acc = 0;
    for (uint i = 0; i < s.spin; i++) {
        h = h * 1664525u + 1013904223u;
        acc += float(h >> 20);
    }
    return acc * s.zero;
}

/// gpu.x1 barrier probes: pass A's texel for seed `seed`, unorm8 codes of the hash
fragment float4 x1_seeded_unorm(float4 position [[position]], constant X1Seeded &s [[buffer(0)]])
{
    const uint2 p = uint2(position.xy);
    const uint v = x1_hash(p.x, p.y, s.seed);
    return float4(float(v & 255), float((v >> 8) & 255), float((v >> 16) & 255), float(v >> 24)) / 255.0f +
           x1_spin(p, s);
}

/// The same as halves in [1, 2): ten bits of the hash a component
fragment half4 x1_seeded_half(float4 position [[position]], constant X1Seeded &s [[buffer(0)]])
{
    const uint2 p = uint2(position.xy);
    const uint v = x1_hash(p.x, p.y, s.seed);
    const ushort4 bits = ushort4(0x3c00) | ushort4(v & 0x3ff, (v >> 10) & 0x3ff, (v >> 20) & 0x3ff, (v >> 22) & 0x3ff);
    return as_type<half4>(bits) + half(x1_spin(p, s));
}

struct X1Tap {
    int2 base, step, tile, size;
};

/// Pass B: texel base + (x, y) * step of the source (clamped to it) at pixel (x, y) of the atlas tile at `tile`
fragment float4 x1_tap_unorm(float4 position [[position]], texture2d<float, access::read> src [[texture(0)]],
                             constant X1Tap &t [[buffer(0)]])
{
    const int2 at = clamp(t.base + (int2(position.xy) - t.tile) * t.step, int2(0), t.size - 1);
    return src.read(uint2(at));
}

fragment half4 x1_tap_half(float4 position [[position]], texture2d<half, access::read> src [[texture(0)]],
                           constant X1Tap &t [[buffer(0)]])
{
    const int2 at = clamp(t.base + (int2(position.xy) - t.tile) * t.step, int2(0), t.size - 1);
    return src.read(uint2(at));
}

/// A pyramid level: the 2 x 2 box of the level above
fragment float4 x1_box_unorm(float4 position [[position]], texture2d<float, access::read> src [[texture(0)]])
{
    const uint2 p = uint2(position.xy) * 2, last = uint2(src.get_width(), src.get_height()) - 1;
    return (src.read(min(p, last)) + src.read(min(p + uint2(1, 0), last)) + src.read(min(p + uint2(0, 1), last)) +
            src.read(min(p + 1, last))) * 0.25f;
}

fragment half4 x1_box_half(float4 position [[position]], texture2d<half, access::read> src [[texture(0)]])
{
    const uint2 p = uint2(position.xy) * 2, last = uint2(src.get_width(), src.get_height()) - 1;
    return (src.read(min(p, last)) + src.read(min(p + uint2(1, 0), last)) + src.read(min(p + uint2(0, 1), last)) +
            src.read(min(p + 1, last))) * 0.25h;
}

/// gpu.x1.job_boundary_idle: buffer word i of seed `seed` (x1_hash(i, 0, seed)), written through the SMs' stores
kernel void x1_buffer_seed(device uint *out [[buffer(0)]], constant uint &seed [[buffer(1)]],
                           uint i [[thread_position_in_grid]])
{
    out[i] = x1_hash(i, 0, seed);
}

/// A buffer read through the SMs' data cache
kernel void x1_buffer_copy(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                           uint i [[thread_position_in_grid]])
{
    out[i] = in[i];
}

/// A buffer read through the constant cache
kernel void x1_constant_copy(constant uint *in [[buffer(0)]], device uint *out [[buffer(1)]],
                             uint i [[thread_position_in_grid]])
{
    out[i] = in[i];
}

/// gpu.x1.subchannel_switch_drain: a BGRA8 texture's codes (red lowest), read by a compute kernel
kernel void x1_texture_codes_unorm(texture2d<float, access::read> src [[texture(0)]], device uint *out [[buffer(0)]],
                                   uint2 id [[thread_position_in_grid]])
{
    if (id.x < src.get_width() && id.y < src.get_height())
        out[id.y * src.get_width() + id.x] = pack_float_to_unorm4x8(src.read(id));
}

/// An RGBA16F texture's halves' bits
kernel void x1_texture_codes_half(texture2d<half, access::read> src [[texture(0)]], device uint2 *out [[buffer(0)]],
                                  uint2 id [[thread_position_in_grid]])
{
    if (id.x < src.get_width() && id.y < src.get_height())
        out[id.y * src.get_width() + id.x] = as_type<uint2>(src.read(id));
}

/// gpu.x1.twod_scale: the 3D route's scaled copy, as Core Animation and MPS scale: a sample of the source at the
/// destination pixel's centre in normalized coordinates, through `s` (nearest or linear, clamped to the edge)
fragment float4 x1_scale_fragment(float4 position [[position]], texture2d<float> src [[texture(0)]],
                                  sampler s [[sampler(0)]], constant float2 &size [[buffer(0)]])
{
    return src.sample(s, position.xy / size, level(0));
}

/// gpu.x1.twod_resolve: sample `id`'s colour of `colors` (shaded per sample)
fragment float4 x1_sample_fragment(float4 position [[position]], uint id [[sample_id]],
                                   constant float4 *colors [[buffer(0)]])
{
    const uint2 p = uint2(position.xy);
    return colors[((p.x + p.y * 3u) % 4u) * 4u + id];
}
