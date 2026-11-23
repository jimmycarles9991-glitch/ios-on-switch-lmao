#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.ycbcr_texel_buffers: the width a kernel sees of a texture buffer
kernel void pbuf_width(texture_buffer<float> t [[texture(0)]], device uint *out [[buffer(0)]],
                       uint i [[thread_position_in_grid]])
{
    if (i == 0)
        out[0] = t.get_width();
}

/// gpu.gm20b.packed_levels: read() of every texel of level `lod`
kernel void plevels_read(texture2d<float, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]],
                         constant uint &lod [[buffer(1)]], uint2 g [[thread_position_in_grid]])
{
    const uint w = t.get_width(lod), h = t.get_height(lod);
    if (g.x < w && g.y < h)
        out[g.y * w + g.x] = t.read(g, lod);
}

/// gpu.gm20b.packed_levels: a nearest sample at each texel centre of level `lod`, nearest between levels, clamped
kernel void plevels_sample(texture2d<float> t [[texture(0)]], device float4 *out [[buffer(0)]],
                           constant uint &lod [[buffer(1)]], uint2 g [[thread_position_in_grid]])
{
    constexpr sampler s(coord::normalized, filter::nearest, mip_filter::nearest, address::clamp_to_edge);
    const uint w = t.get_width(lod), h = t.get_height(lod);
    if (g.x < w && g.y < h)
        out[g.y * w + g.x] = t.sample(s, (float2(g) + 0.5f) / float2(w, h), level(float(lod)));
}

/// gpu.gm20b.packed_levels: minification linear, magnification nearest, at level 1 (recorded, not judged)
kernel void plevels_minmag(texture2d<float> t [[texture(0)]], device float4 *out [[buffer(0)]],
                           uint2 g [[thread_position_in_grid]])
{
    constexpr sampler s(coord::normalized, min_filter::linear, mag_filter::nearest, mip_filter::nearest,
                        address::clamp_to_edge);
    const uint w = t.get_width(1), h = t.get_height(1);
    if (g.x < w && g.y < h)
        out[g.y * w + g.x] = t.sample(s, (float2(g) + 0.5f) / float2(w, h), level(1.0f));
}

/// gpu.gm20b.packed_levels_open: one constexpr sampler over a packed texture of several levels and a plain one
kernel void plevels_shared(texture2d<float> packed [[texture(0)]], texture2d<float> plain [[texture(1)]],
                           device float4 *out [[buffer(0)]], device float4 *packedOut [[buffer(1)]],
                           uint2 g [[thread_position_in_grid]])
{
    constexpr sampler s(coord::pixel, filter::nearest, address::clamp_to_edge);
    const uint w = plain.get_width();
    if (g.x < w && g.y < plain.get_height()) {
        out[g.y * w + g.x] = plain.sample(s, float2(g) + 0.5f);
        packedOut[g.y * w + g.x] = packed.sample(s, float2(g) + 0.5f);
    }
}
