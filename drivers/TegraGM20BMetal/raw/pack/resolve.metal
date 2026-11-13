// SPDX-License-Identifier: MIT
// raw GM20B path: a multisampled color attachment's resolve (MTLStoreActionMultisampleResolve), drawn one pixel a fragment over
// the pass's render area into the resolve texture, by Metal's measured rules (metal2vulkan bind/color_resolve.rs; RG11B10Float
// and four halves measured on an M1, raw/test/resolveoracle.mm):
//  - an 8-bit unorm channel: its codes summed with half the count, over the count;
//  - an 8-bit sRGB channel: its code (the nearest to its decoded value) as a 12-bit linear value (resolve_srgb.h), those averaged
//    so and encoded by Metal's thresholds, written as the code through the target's UNORM twin;
//  - a half channel: the mean rounded to a half with a tie toward +infinity (halfMean);
//  - an RG11B10Float channel: that half mean rounded to the channel's float, ties to even.
// A depth attachment's resolve writes its fragment's depth, MTLMultisampleDepthResolveFilter's sample 0, the samples' minimum or
// their maximum, as read: each the stored code of the target's format, written back unchanged. One function a target format
// and filter: the pack keys an own state by its function's name. The sample count comes in buffer 0, as the
// attachment's (the shader does not query the texture). Built without fast math (rawpack.py), so the sums keep their order and
// the bit tests their NaNs.
#include <metal_stdlib>
#include "resolve_srgb.h"
using namespace metal;

struct Out {
    float4 position [[position]];
};

vertex Out resolve_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

typedef texture2d_ms<float, access::read> Samples;

// The code of a decoded 8-bit sRGB value (the midpoints at or below it), and the code Metal encodes a 12-bit linear value as (the
// thresholds it reaches): each the count of a table's 255 ascending entries x reaches, by eight steps of a binary search. Each
// reads its table by name, so m2v keeps the tables as the program's own constants.
static uint codeOfDecoded(float x)
{
    uint lo = 0;
    for (uint step = 128; step; step >>= 1)
        lo = x >= kSrgbMidpoints[lo + step - 1] ? lo + step : lo;
    return lo;
}

static uint codeOfLinear(uint x)
{
    uint lo = 0;
    for (uint step = 128; step; step >>= 1)
        lo = x >= kSrgbThresholds[lo + step - 1] ? lo + step : lo;
    return lo;
}

static float4 unorm8(Samples s, uint n, uint2 at)
{
    uint4 sum = uint4(n / 2);
    for (uint k = 0; k < n; k++)
        sum += uint4(s.read(at, k) * 255.0 + 0.5);
    return float4(sum / n) * as_type<float>(0x3b808081u);  // 1 / 255
}

// The same with the sample count built in, in floats: an 8-bit unorm sample reads as the float nearest code / 255, so
// rint(x * 255) is its code, the codes and half the count sum exactly, and dividing by a power of two and flooring is the
// integer quotient. With the count from a buffer the loop runs to eight guarded fetches and each conversion to an integer
// carries its range and NaN guards, the 642 instructions of resolve_*8_frag.
template <uint N> static float4 unorm8Of(Samples s, uint2 at)
{
    float4 sum = float4(N / 2);
    for (uint k = 0; k < N; k++)
        sum += rint(s.read(at, k) * 255.0);
    return floor(sum * (1.0 / N)) * as_type<float>(0x3b808081u);  // 1 / 255
}

static float4 srgb8(Samples s, uint n, uint2 at)
{
    uint4 sum = uint4(n / 2);
    for (uint k = 0; k < n; k++) {
        const float4 x = s.read(at, k);
        for (uint c = 0; c < 3; c++)
            sum[c] += kSrgbLinear[codeOfDecoded(x[c])];
        sum.w += uint(x.w * 255.0 + 0.5);
    }
    sum /= n;
    return float4(codeOfLinear(sum.x), codeOfLinear(sum.y), codeOfLinear(sum.z), sum.w) * as_type<float>(0x3b808081u);
}

// A half resolve: each sample's value (a NaN counts 0, an infinity 65536 of its sign) summed, times one over the count, rounded
// to the half grid with a tie toward +infinity as floor(x / u + 1/2) u, u the grid's step at x (each step exact); 65536 or more
// an infinity, a zero +0. Two samples sum exactly (Metal's rule). Four sum as Metal's adder comes nearest of the forms measured
// on an M1: each sample floored to a sixteenth of the half step at the largest one's exponent, which leaves about one random
// half texel in 26 a step off Metal's (resolveoracle), so that rule is not closed.
static float4 counted(float4 x)
{
    const uint4 bits = as_type<uint4>(x), m = bits & 0x7fffffffu;
    return as_type<float4>(select(select(bits, (bits & 0x80000000u) | 0x47800000u, m == 0x7f800000u), uint4(0), m > 0x7f800000u));
}

static float halfGrid(float x)
{
    const uint field = max((as_type<uint>(x) & 0x7fffffffu) >> 23, 127u - 14u) - 10u;
    const float units = floor(x * as_type<float>((254u - field) << 23) + 0.5);
    const uint rounded = as_type<uint>(units * as_type<float>(field << 23));
    const uint rm = rounded & 0x7fffffffu;
    return as_type<float>(rm == 0 ? 0u : rm >= 0x47800000u ? (rounded & 0x80000000u) | 0x7f800000u : rounded);
}

static uint4 magnitude(float4 x)
{
    return as_type<uint4>(x) & 0x7fffffffu;
}

static float4 halfMean(Samples s, uint n, uint2 at)
{
    const float4 a = counted(s.read(at, 0)), b = counted(s.read(at, 1));
    float4 sum = a + b;
    if (n == 4) {
        const float4 c = counted(s.read(at, 2)), d = counted(s.read(at, 3));
        const uint4 top = max(max(magnitude(a), magnitude(b)), max(magnitude(c), magnitude(d)));
        const uint4 field = max(top >> 23, uint4(127u - 14u)) - 14u;
        const float4 g = as_type<float4>(field << 23), inv = as_type<float4>((254u - field) << 23);
        sum = floor(a * inv) * g + floor(b * inv) * g + floor(c * inv) * g + floor(d * inv) * g;
    }
    const float4 x = sum * (1.0 / float(n));
    return float4(halfGrid(x.x), halfGrid(x.y), halfGrid(x.z), halfGrid(x.w));
}

// An unsigned float of `m` fraction bits and a five-bit exponent (RG11B10Float's channels) nearest a half, ties to even, as
// the exact float the target stores: Metal's resolve rounds the half mean so
static float smallFloat(float x, uint m)
{
    if (!(x > 0.0))
        return 0.0;
    const uint field = max(as_type<uint>(x) >> 23, 127u - 14u) - m;
    const float rounded = rint(x * as_type<float>((254u - field) << 23)) * as_type<float>(field << 23);
    return rounded >= 65536.0 ? INFINITY : rounded;
}

static float4 smallFloats(Samples s, uint n, uint2 at)
{
    const float4 h = halfMean(s, n, at);
    return float4(smallFloat(h.x, 6), smallFloat(h.y, 6), smallFloat(h.z, 5), 1.0);
}

fragment float4 resolve_rgba8_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return unorm8(s, n, uint2(in.position.xy));
}

fragment float4 resolve_rgba8_srgb_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return srgb8(s, n, uint2(in.position.xy));
}

fragment float4 resolve_bgra8_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return unorm8(s, n, uint2(in.position.xy));
}

fragment float4 resolve_rgba8_2x_frag(Out in [[stage_in]], Samples s [[texture(0)]])
{
    return unorm8Of<2>(s, uint2(in.position.xy));
}

fragment float4 resolve_rgba8_4x_frag(Out in [[stage_in]], Samples s [[texture(0)]])
{
    return unorm8Of<4>(s, uint2(in.position.xy));
}

fragment float4 resolve_bgra8_2x_frag(Out in [[stage_in]], Samples s [[texture(0)]])
{
    return unorm8Of<2>(s, uint2(in.position.xy));
}

fragment float4 resolve_bgra8_4x_frag(Out in [[stage_in]], Samples s [[texture(0)]])
{
    return unorm8Of<4>(s, uint2(in.position.xy));
}

fragment float4 resolve_bgra8_srgb_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return srgb8(s, n, uint2(in.position.xy));
}

fragment float4 resolve_rg11b10f_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return smallFloats(s, n, uint2(in.position.xy));
}

fragment float4 resolve_rgba16f_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant uint &n [[buffer(0)]])
{
    return halfMean(s, n, uint2(in.position.xy));
}

typedef depth2d_ms<float, access::read> DepthSamples;

struct Depth {
    float depth [[depth(any)]];
};

static Depth depthOf(DepthSamples s, uint n, uint2 at, uint filter)
{
    float d = s.read(at, 0);
    for (uint k = 1; filter && k < n; k++)
        d = filter == 1 ? min(d, s.read(at, k)) : max(d, s.read(at, k));
    return {d};
}

#define DEPTH_RESOLVE(format, filter, index)                                                                               \
    fragment Depth resolve_##format##_##filter##_frag(Out in [[stage_in]], DepthSamples s [[texture(0)]],                \
                                                      constant uint &n [[buffer(0)]])                                     \
    {                                                                                                                      \
        return depthOf(s, n, uint2(in.position.xy), index);                                                               \
    }

DEPTH_RESOLVE(depth16, sample0, 0)
DEPTH_RESOLVE(depth16, min, 1)
DEPTH_RESOLVE(depth16, max, 2)
DEPTH_RESOLVE(depth32f, sample0, 0)
DEPTH_RESOLVE(depth32f, min, 1)
DEPTH_RESOLVE(depth32f, max, 2)
DEPTH_RESOLVE(depth32fs8, sample0, 0)
DEPTH_RESOLVE(depth32fs8, min, 1)
DEPTH_RESOLVE(depth32fs8, max, 2)
