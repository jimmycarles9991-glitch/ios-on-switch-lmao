// SPDX-License-Identifier: MIT
// raw GM20B path: an extended-range texture's stand-in (runtime/raw_xr.h), drawn one texel a fragment: each channel's value as
// Metal reads it into a texture2d<half>, (code - 384) / 510 (Metal's MTLPixelFormat documentation) and the two-plane alpha byte
// over 255, as the half nearest that quotient. No quotient is a midpoint between two halves, so the rounding has no ties, and its
// float lies nowhere near one, so Metal's read through float rounds to the same half. BGRA10_XR's alpha code is clamped to
// [384, 894], the codes of [0, 1]; BGR10_XR's alpha is 1.
// Texture 0 is the codes through the raw path's integer header (raw_gpu.cpp kFormats): R32Uint (BGR10_XR), RGBA16Uint
// (BGRA10_XR: blue's field first) or RG32Uint (RGB10A8_2P_XR10: the color word, then the alpha byte); an RGB10A8_2P_XR10
// texture held in Metal's two planes (runtime/raw_xr_pass.h) is plane 0's words as R32Uint and plane 1's bytes, texture 1, as
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

fragment half4 xr_bgra10_frag(Out in [[stage_in]], texture2d<uint, access::read> codes [[texture(0)]])
{
    const uint4 f = codes.read(uint2(in.position.xy)) >> 6;
    return half4(color(f.z), color(f.y), color(f.x), color(clamp(f.w, 384u, 894u)));
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
