#include <metal_stdlib>
using namespace metal;

// tests/ZcViewFormatsTest.swift: a word per pixel, drawn through an R32Uint view or as RGBA codes through a colour view
static uint zc_view_hash(uint2 p, uint seed)
{
    uint h = (p.x * 73856093u) ^ (p.y * 19349663u) ^ (seed * 83492791u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return h ^ (h >> 15);
}

fragment uint zc_view_uint_fragment(float4 position [[position]], constant uint &seed [[buffer(0)]])
{
    return zc_view_hash(uint2(position.xy), seed);
}

fragment float4 zc_view_float_fragment(float4 position [[position]], constant uint &seed [[buffer(0)]])
{
    const uint h = zc_view_hash(uint2(position.xy), seed);
    return float4(uint4(h, h >> 8, h >> 16, h >> 24) & 255u) / 255.0f;
}

kernel void zc_view_read_uint(texture2d<uint, access::read> view [[texture(0)]], device uint *out [[buffer(0)]],
                              uint2 id [[thread_position_in_grid]])
{
    out[id.y * view.get_width() + id.x] = view.read(id).r;
}

// A colour view's codes, red in the low byte
kernel void zc_view_read_codes(texture2d<float, access::read> view [[texture(0)]], device uint *out [[buffer(0)]],
                               uint2 id [[thread_position_in_grid]])
{
    const uint4 c = uint4(rint(view.read(id) * 255.0f));
    out[id.y * view.get_width() + id.x] = c.x | c.y << 8 | c.z << 16 | c.w << 24;
}
