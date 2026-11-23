// SPDX-License-Identifier: MIT
// raw GM20B path: an extended-range texture's stand-in (runtime/xr.h), drawn one texel a fragment: each channel's value as
// Metal reads it into a texture2d<half>, (code - 384) / 510 (Metal's MTLPixelFormat documentation) and the two-plane alpha byte
// over 255, as the half nearest that quotient. No quotient is a midpoint between two halves, so the rounding has no ties, and its
// float lies nowhere near one, so Metal's read through float rounds to the same half. BGRA10_XR's alpha code is clamped to
// [384, 894], the codes of [0, 1]; BGR10_XR's alpha is 1.
// BGR10_XR_sRGB's word and BGRA10_XR_sRGB's fields are BGR10_XR's and BGRA10_XR's, their colour codes read through Metal's
// sRGB table instead, BGRA10_XR_sRGB's alpha as BGRA10_XR's (xr_bgr10_srgb_frag, xr_bgra10_srgb_frag).
// Texture 0 is the codes through the driver's integer header (gpu.cpp kFormats): R32Uint (BGR10_XR), RGBA16Uint
// (BGRA10_XR: blue's field first) or RG32Uint (RGB10A8_2P_XR10: the color word, then the alpha byte); an RGB10A8_2P_XR10
// texture held in Metal's two planes (runtime/xr_pass.h) is plane 0's words as R32Uint and plane 1's bytes, texture 1, as
// R8Uint.
#include <metal_stdlib>
using namespace metal;

struct Out {
    float4 position [[position]];
};

vertex Out xr_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

// The half nearest n / d for |n / d| < 2 and 0 < d <= 510, from integers alone: its exponent e (2^e <= |n| / d < 2^(e + 1), at
// least -9) and its 11-bit significand rounded to nearest even
static half quotient(int n, uint d)
{
    if (n == 0)
        return 0.0h;
    const uint m = uint(abs(n));
    int e = 0;
    for (uint k = m; k < d; k <<= 1)
        e--;
    const uint scaled = m << uint(10 - e);
    uint q = scaled / d;
    const uint r = scaled - q * d;
    if (2 * r > d || (2 * r == d && (q & 1)))
        q++;
    if (q == 2048) {
        q = 1024;
        e++;
    }
    return as_type<half>(ushort((n < 0 ? 0x8000u : 0u) | uint(e + 15) << 10 | (q - 1024)));
}

static half color(uint code)
{
    return quotient(int(code) - 384, 510);
}

static half3 word(uint w)
{
    return half3(color(w >> 20 & 0x3ff), color(w >> 10 & 0x3ff), color(w & 0x3ff));
}

fragment half4 xr_bgr10_frag(Out in [[stage_in]], texture2d<uint, access::read> codes [[texture(0)]])
{
    return half4(word(codes.read(uint2(in.position.xy)).r), 1.0h);
}

constant ushort kSrgbDecode[640] = {
#include "xr_srgb_decode.h"
};

// BGR10_XR_sRGB's read of a colour code (runtime/xr_srgb.h bgr10XrSrgbValue): its decode step s / 8192, signed,
// as the half nearest it with a tie away from zero (Metal's rounding of these steps). s is below 2^14: rounded to the half
// step of its binade in integers, the quotient is exact in a half.
static half srgbColor(uint code)
{
    const uint s = kSrgbDecode[code < 384 ? 384 - code : code - 384];
    const uint k = s < 2048 ? 1 : s < 4096 ? 2 : s < 8192 ? 4 : 8;
    const half v = half(float((s + k / 2) / k * k) * (1.0f / 8192.0f));
    return code < 384 ? -v : v;
}

static half4 bgr10Srgb(uint w)
{
    return half4(srgbColor(w >> 20 & 0x3ff), srgbColor(w >> 10 & 0x3ff), srgbColor(w & 0x3ff), 1.0h);
}

static half4 bgra10Srgb(uint4 f)
{
    return half4(srgbColor(f.z), srgbColor(f.y), srgbColor(f.x), color(clamp(f.w, 384u, 894u)));
}

fragment half4 xr_bgr10_srgb_frag(Out in [[stage_in]], texture2d<uint, access::read> codes [[texture(0)]])
{
    return bgr10Srgb(codes.read(uint2(in.position.xy)).r);
}

fragment half4 xr_bgra10_frag(Out in [[stage_in]], texture2d<uint, access::read> codes [[texture(0)]])
{
    const uint4 f = codes.read(uint2(in.position.xy)) >> 6;
    return half4(color(f.z), color(f.y), color(f.x), color(clamp(f.w, 384u, 894u)));
}

fragment half4 xr_bgra10_srgb_frag(Out in [[stage_in]], texture2d<uint, access::read> codes [[texture(0)]])
{
    return bgra10Srgb(codes.read(uint2(in.position.xy)) >> 6);
}

fragment half4 xr_planes_frag(Out in [[stage_in]], texture2d<uint, access::read> codes [[texture(0)]])
{
    const uint2 t = codes.read(uint2(in.position.xy)).rg;
    return half4(word(t.x), quotient(int(t.y & 0xff), 255));
}

fragment half4 xr_two_planes_frag(Out in [[stage_in]], texture2d<uint, access::read> words [[texture(0)]],
                                  texture2d<uint, access::read> alphas [[texture(1)]])
{
    const uint2 at = uint2(in.position.xy);
    return half4(word(words.read(at).r), quotient(int(alphas.read(at).r & 0xff), 255));
}

static half3 srgbWord(uint w)
{
    return half3(srgbColor(w >> 20 & 0x3ff), srgbColor(w >> 10 & 0x3ff), srgbColor(w & 0x3ff));
}

// RGB10A8_2P_XR10_sRGB over a surface's two planes (runtime/xr.h xrConversionOf): its word through BGR10_XR_sRGB's
// colour reads, its alpha byte linear
fragment half4 xr_two_planes_srgb_frag(Out in [[stage_in]], texture2d<uint, access::read> words [[texture(0)]],
                                       texture2d<uint, access::read> alphas [[texture(1)]])
{
    const uint2 at = uint2(in.position.xy);
    return half4(srgbWord(words.read(at).r), quotient(int(alphas.read(at).r & 0xff), 255));
}

// RGB10A8_2P_XR10_sRGB of a descriptor (runtime/xr.h xrConversionOf): a texel's word and alpha byte side by side, read
// as xr_two_planes_srgb_frag reads them from two planes
fragment half4 xr_planes_srgb_frag(Out in [[stage_in]], texture2d<uint, access::read> codes [[texture(0)]])
{
    const uint2 t = codes.read(uint2(in.position.xy)).rg;
    return half4(srgbWord(t.x), quotient(int(t.y & 0xff), 255));
}

// An RGB10A8_2P_XR10 texture held in its two planes that a fragment program stores into (runtime/gpu.cpp, written
// textures): the program stores through an RG32Uint image of the texels, the word then the alpha byte, which the planes are
// merged into before its pass and split back out of after it, every texel as it is
fragment uint4 xr_planes_merge_frag(Out in [[stage_in]], texture2d<uint, access::read> words [[texture(0)]],
                                    texture2d<uint, access::read> alphas [[texture(1)]])
{
    const uint2 at = uint2(in.position.xy);
    return uint4(words.read(at).r, alphas.read(at).r & 0xff, 0, 0);
}

struct Planes {
    uint word [[color(0)]];
    uint alpha [[color(1)]];
};

fragment Planes xr_planes_split_frag(Out in [[stage_in]], texture2d<uint, access::read> image [[texture(0)]])
{
    const uint2 t = image.read(uint2(in.position.xy)).rg;
    return {t.x, t.y & 0xff};
}

// A GPU copy through a BGRA10_XR or BGRA10_XR_sRGB texture Metal keeps compressed (runtime/TegraGM20BMetalCopyClamp.h): the copied
// fields as an M4 Max leaves the copy's destination, each its 10-bit code alone (the low 6 bits 0) and the alpha code clamped
// to 384..894 (m2v-textures7 probes/lowbits.mm); `origin` the destination's texel the scratch's (0, 0) holds
fragment uint4 xr_copy_clamp_frag(Out in [[stage_in]], texture2d<uint, access::read> copied [[texture(0)]],
                                  constant uint4 &origin [[buffer(0)]])
{
    uint4 v = copied.read(uint2(in.position.xy) - origin.xy) & 0xffc0u;
    v.w = clamp(v.w, 384u << 6, 894u << 6);
    return v;
}
