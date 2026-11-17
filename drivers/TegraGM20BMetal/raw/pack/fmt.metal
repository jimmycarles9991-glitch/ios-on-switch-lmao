// SPDX-License-Identifier: MIT
// raw GM20B path: colour targets of formats the GM20B renders none of (runtime/raw_format_standin.h), rendered into an
// RGBA32Float stand-in. The load states fill the stand-in with the texture's texels as floats (RGB9E5Float's decoded from
// its words); each store state writes the stand-in's texels into the texture's words, as an M4 Max stores a drawn texel:
// A1BGR5Unorm and ABGR4Unorm each channel's nearest half, a tie to even, clamped to [0, 1] (a NaN 0), then its nearest code;
// RGB9E5Float each channel clamped to [0, 65408] (a NaN 0), the largest's exponent shared, every mantissa truncated. A clear
// goes into the stand-in as floats these give back (formatStandInClear).
#include <metal_stdlib>
using namespace metal;

struct Out {
    float4 position [[position]];
};

vertex Out fmt_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

fragment float4 fmt_load_frag(Out in [[stage_in]], texture2d<float, access::read> t [[texture(0)]])
{
    return t.read(uint2(in.position.xy));
}

static float3 rgb9e5Floats(uint w)
{
    return ldexp(float3(uint3(w, w >> 9, w >> 18) & 511u), int3(int(w >> 27) - 24));
}

fragment float4 fmt_load_rgb9e5_frag(Out in [[stage_in]], texture2d<uint, access::read> t [[texture(0)]])
{
    return float4(rgb9e5Floats(t.read(uint2(in.position.xy)).r), 1.0f);
}

static uint drawnCode(float v, float max)
{
    const float h = float(half(v));
    return h > 0.0f ? uint(rint(min(h, 1.0f) * max)) : 0u;
}

static float4 texel(Out in, texture2d<float, access::read> s)
{
    return s.read(uint2(in.position.xy));
}

fragment uint fmt_store_abgr4_frag(Out in [[stage_in]], texture2d<float, access::read> s [[texture(0)]])
{
    const float4 v = texel(in, s);
    return drawnCode(v.r, 15) << 12 | drawnCode(v.g, 15) << 8 | drawnCode(v.b, 15) << 4 | drawnCode(v.a, 15);
}

fragment uint fmt_store_a1bgr5_frag(Out in [[stage_in]], texture2d<float, access::read> s [[texture(0)]])
{
    const float4 v = texel(in, s);
    return drawnCode(v.r, 31) << 11 | drawnCode(v.g, 31) << 6 | drawnCode(v.b, 31) << 1 | drawnCode(v.a, 1);
}

fragment uint fmt_store_rgb9e5_frag(Out in [[stage_in]], texture2d<float, access::read> s [[texture(0)]])
{
    const float4 v = texel(in, s);
    float3 c;
    for (int k = 0; k < 3; k++)
        c[k] = v[k] > 0.0f ? min(v[k], 65408.0f) : 0.0f;
    const float m = max(c.r, max(c.g, c.b));
    const int e = m > 0.0f ? max(0, int(as_type<uint>(m) >> 23 & 255) - 127 + 16) : 0;
    const uint3 mantissa = uint3(floor(ldexp(c, int3(24 - e))));
    return mantissa.r | mantissa.g << 9 | mantissa.b << 18 | uint(e) << 27;
}

// generateMipmapsForTexture: of such a chain (formatMipPasses): each texel Metal's step (pack/mip.metal's taps: one bilinear
// tap at its centre on the level above, the weight in 1/256), the level above's words read as integers. A1BGR5Unorm and
// ABGR4Unorm weigh their codes exactly and hold the mean as its nearest float, then its nearest code, ties even (a tie of
// code k is held above it where bit k of the format's mask is set: raw_format_standin.h formatMipTiesUp); RGB9E5Float blends
// its decoded values and holds the blend as a half, mantissas truncated; R8Unorm_sRGB blends its codes in linear light.
struct Step {
    uint2 above, size;
};

struct Taps {
    uint2 first, second, weight;
};

static Taps taps(float2 position, constant Step &step)
{
    const uint2 x = uint2(position), s = step.above, d = step.size;
    const uint2 at = (2 * x + 1) * s - d;
    const uint2 whole = at / (2 * d), part = at % (2 * d);
    Taps t;
    t.first = min(whole, s - 1);
    t.second = min(whole + 1, s - 1);
    t.weight = (part * 256 + d) / (2 * d);
    return t;
}

static uint4 words(texture2d<uint, access::read> above, Taps t)
{
    return uint4(above.read(t.first).r, above.read(uint2(t.second.x, t.first.y)).r,
                 above.read(uint2(t.first.x, t.second.y)).r, above.read(t.second).r);
}

static uint mipCode(uint4 c, uint2 w, uint tiesUp)
{
    const uint n = (c.x * (256 - w.x) + c.y * w.x) * (256 - w.y) + (c.z * (256 - w.x) + c.w * w.x) * w.y;
    const uint base = n >> 16, rest = n & 0xffffu;
    return base + uint(rest > 0x8000u || (rest == 0x8000u && (tiesUp >> base & 1u)));
}

fragment uint fmt_mip_abgr4_frag(Out in [[stage_in]], texture2d<uint, access::read> above [[texture(0)]],
                                 constant Step &step [[buffer(0)]])
{
    const Taps t = taps(in.position.xy, step);
    const uint4 w = words(above, t);
    uint out = 0;
    for (uint shift = 0; shift < 16; shift += 4)
        out |= mipCode(w >> shift & 15u, t.weight, 0xffu) << shift;
    return out;
}

fragment uint fmt_mip_a1bgr5_frag(Out in [[stage_in]], texture2d<uint, access::read> above [[texture(0)]],
                                  constant Step &step [[buffer(0)]])
{
    const Taps t = taps(in.position.xy, step);
    const uint4 w = words(above, t);
    uint out = mipCode(w & 1u, t.weight, 0u);
    for (uint shift = 1; shift < 16; shift += 5)
        out |= mipCode(w >> shift & 31u, t.weight, 0x5555ff00u) << shift;
    return out;
}

static float3 blend3(float3 a, float3 b, float3 c, float3 e, float2 w)
{
    const float3 top = fma(b, float3(w.x), a * (1.0 - w.x)), bottom = fma(e, float3(w.x), c * (1.0 - w.x));
    return fma(bottom, float3(w.y), top * (1.0 - w.y));
}

fragment uint fmt_mip_rgb9e5_frag(Out in [[stage_in]], texture2d<uint, access::read> above [[texture(0)]],
                                  constant Step &step [[buffer(0)]])
{
    const Taps t = taps(in.position.xy, step);
    const uint4 w = words(above, t);
    const float3 v = float3(half3(
        blend3(rgb9e5Floats(w.x), rgb9e5Floats(w.y), rgb9e5Floats(w.z), rgb9e5Floats(w.w), float2(t.weight) / 256.0)));
    const float m = max(v.r, max(v.g, v.b));
    const int e = m > 0.0f ? max(0, int(as_type<uint>(m) >> 23 & 255) - 127 + 16) : 0;
    const uint3 mantissa = uint3(floor(ldexp(v, int3(24 - e))));
    return mantissa.r | mantissa.g << 9 | mantissa.b << 18 | uint(e) << 27;
}

static float linearLight(uint code)
{
    const float c = float(code) / 255.0;
    return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
}

fragment uint fmt_mip_srgb8_frag(Out in [[stage_in]], texture2d<uint, access::read> above [[texture(0)]],
                                 constant Step &step [[buffer(0)]])
{
    const Taps t = taps(in.position.xy, step);
    const uint4 w = words(above, t);
    const float2 f = float2(t.weight) / 256.0;
    const float top = fma(linearLight(w.y), f.x, linearLight(w.x) * (1.0 - f.x)),
                bottom = fma(linearLight(w.w), f.x, linearLight(w.z) * (1.0 - f.x));
    const float v = saturate(fma(bottom, f.y, top * (1.0 - f.y)));
    const float s = v <= 0.0031308 ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
    return uint(rint(s * 255.0));
}
