// SPDX-License-Identifier: MIT
// An extended-range colour attachment's pass (runtime/format_standin.h): the pass renders into an RGBA16Float stand-in,
// Metal's tile, which the load states fill from the texture's codes and the store states write back into them.
//  - A load reads each code as Metal's blend reads the tile (an M4 Max, xr-float probe "tile"): colour codes past 1022 as
//    1022 (BGRA10_XR_sRGB's past its sRGB 1022, BGR10_XR_sRGB's and RGB10A8_2P_XR10_sRGB's at most 1281/1024, its linear
//    1022), BGRA10_XR's alpha code clamped to 384..894; BGR10_XR's alpha is 1.
//  - A store writes Metal's write code of each stand-in texel (runtime/xr.h writeCode, xr_srgb.h): BGR10_XR's bits 30 and 31
//    set, RGB10A8_2P_XR10's clear, BGRA10_XR's low 6 bits of a field 0. With `keep` (a pass that loaded the texture), a
//    texel whose stand-in still holds the load's value of its own codes is not written: its bytes stay as they were. Metal
//    rewrites every texel of the 32x32 tiles a pass's fragments reach and none elsewhere; GM20B keeps no record of those
//    tiles, so the store keeps the texels no draw changed.
//  - A resolve of a multisampled stand-in into a one-sample one takes each sample's store code and Metal's mean of the
//    samples' codes (colour rounded half up, BGRA10_XR's alpha with a tie down: harness/resolvecodesoracle.mm), as the
//    value whose store is that code.
// Codes come through the texture's integer headers (gpu.cpp kFormats): R32Uint (BGR10_XR), RGBA16Uint (BGRA10_XR, blue's
// field first), RG32Uint (RGB10A8_2P_XR10 of a descriptor: the word, then the alpha byte), or a surface's two planes, the
// words as R32Uint and the alpha bytes as R8Uint. Built without fast math: the NaN tests and the rounding are Metal's.
#include <metal_stdlib>
using namespace metal;

struct Out {
    float4 position [[position]];
};

vertex Out xr_target_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

// The half nearest n / d for |n / d| < 2 and 0 < d <= 510, from integers alone (pack/xr.metal quotient)
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

constant ushort kSrgbDecode[640] = {
#include "xr_srgb_decode.h"
};

constant ushort kSrgbFirst[640] = {
#include "xr_srgb_encode.h"
};

// Metal's read of an sRGB colour code (pack/xr.metal srgbColor)
static half srgbValue(uint code)
{
    const uint s = kSrgbDecode[code < 384 ? 384 - code : code - 384];
    const uint k = s < 2048 ? 1 : s < 4096 ? 2 : s < 8192 ? 4 : 8;
    const half v = half(float((s + k / 2) / k * k) * (1.0f / 8192.0f));
    return code < 384 ? -v : v;
}

static half linearTile(uint code)
{
    return quotient(int(min(code, 1022u)) - 384, 510);
}

static half alphaTile(uint code)
{
    return quotient(int(clamp(code, 384u, 894u)) - 384, 510);
}

static half byteTile(uint byte)
{
    return quotient(int(byte & 0xff), 255);
}

// BGRA10_XR_sRGB's colour (its source range's top is its sRGB code 1022)
static half srgbTile(uint code)
{
    return srgbValue(min(code, 1022u));
}

// BGR10_XR_sRGB's and RGB10A8_2P_XR10_sRGB's colour (their source range is the linear forms')
static half srgbLinearTile(uint code)
{
    return min(srgbValue(code), 1.2509765625h);
}

static half3 wordTile(uint w, bool srgb)
{
    const uint3 c = uint3(w >> 20, w >> 10, w) & 1023u;
    return srgb ? half3(srgbLinearTile(c.x), srgbLinearTile(c.y), srgbLinearTile(c.z))
                : half3(linearTile(c.x), linearTile(c.y), linearTile(c.z));
}

static half4 fieldsTile(uint4 f, bool srgb)
{
    const uint4 c = f >> 6;
    return srgb ? half4(srgbTile(c.z), srgbTile(c.y), srgbTile(c.x), alphaTile(c.w))
                : half4(linearTile(c.z), linearTile(c.y), linearTile(c.x), alphaTile(c.w));
}

// Metal's write code of a colour value (runtime/xr.h writeCode): a NaN 0, 384 +- RoundHalfEven(min(|v|, 1.5) * 510)
// (exact in a float for a half), clamped to 0..1023
static uint linearCode(float v)
{
    if (isnan(v))
        v = 0.0f;
    const int k = int(rint(min(fabs(v), 1.5f) * 510.0f));
    return uint(clamp(v < 0.0f ? 384 - k : 384 + k, 0, 1023));
}

// Metal's write code of a BGR10_XR_sRGB colour value (runtime/xr_srgb.h bgr10XrSrgbCode)
static uint srgbCode(float v)
{
    if (isnan(v))
        v = 0.0f;
    const uint i = uint(rint(min(fabs(v), 2.0f) * 8192.0f));
    uint n = 0;
    for (uint step = 512; step; step >>= 1)
        if (n + step < 640 && kSrgbFirst[n + step] <= i)
            n += step;
    return v < 0.0f ? 384 - min(n, 384u) : 384 + n;
}

static uint colourCode(float v, bool srgb)
{
    return srgb ? srgbCode(v) : linearCode(v);
}

static float clampedAlpha(float a)
{
    return isnan(a) ? 0.0f : clamp(a, 0.0f, 1.0f);
}

static uint wordOf(float4 v, bool srgb)
{
    return colourCode(v.r, srgb) << 20 | colourCode(v.g, srgb) << 10 | colourCode(v.b, srgb);
}

static uint4 fieldsOf(float4 v, bool srgb)
{
    const uint alpha = 384u + uint(rint(clampedAlpha(v.a) * 510.0f));
    return uint4(colourCode(v.b, srgb), colourCode(v.g, srgb), colourCode(v.r, srgb), alpha) << 6;
}

static uint byteOf(float a)
{
    return uint(rint(clampedAlpha(a) * 255.0f));
}

static bool holds(float3 v, half3 t)
{
    return all(v == float3(t));
}

typedef texture2d<uint, access::read> Codes;
typedef texture2d_ms<uint, access::read> SampleCodes;
typedef texture2d<float, access::read> Tile;
typedef texture2d_ms<float, access::read> SampleTile;

struct WordAndAlpha {
    uint word [[color(0)]];
    uint alpha [[color(1)]];
};

#define XR_TARGET_FORM(name, srgb)                                                                                    \
    fragment half4 xr_target_load_bgr10##name##_frag(Out in [[stage_in]], Codes codes [[texture(0)]])                 \
    {                                                                                                                 \
        return half4(wordTile(codes.read(uint2(in.position.xy)).r, srgb), 1.0h);                                      \
    }                                                                                                                 \
    fragment half4 xr_target_load_bgra10##name##_frag(Out in [[stage_in]], Codes codes [[texture(0)]])                \
    {                                                                                                                 \
        return fieldsTile(codes.read(uint2(in.position.xy)), srgb);                                                   \
    }                                                                                                                 \
    fragment half4 xr_target_load_planes##name##_frag(Out in [[stage_in]], Codes codes [[texture(0)]])                \
    {                                                                                                                 \
        const uint2 t = codes.read(uint2(in.position.xy)).rg;                                                         \
        return half4(wordTile(t.x, srgb), byteTile(t.y));                                                             \
    }                                                                                                                 \
    fragment half4 xr_target_load_two_planes##name##_frag(Out in [[stage_in]], Codes words [[texture(0)]],            \
                                                          Codes alphas [[texture(1)]])                                \
    {                                                                                                                 \
        const uint2 at = uint2(in.position.xy);                                                                       \
        return half4(wordTile(words.read(at).r, srgb), byteTile(alphas.read(at).r));                                  \
    }                                                                                                                 \
    fragment uint xr_target_store_bgr10##name##_frag(Out in [[stage_in]], Tile s [[texture(0)]],                      \
                                                     Codes codes [[texture(1)]], constant uint &keep [[buffer(0)]])   \
    {                                                                                                                 \
        const uint2 at = uint2(in.position.xy);                                                                       \
        const float4 v = s.read(at);                                                                                  \
        if (keep && holds(v.rgb, wordTile(codes.read(at).r, srgb)))                                                   \
            discard_fragment();                                                                                       \
        return 0xC0000000u | wordOf(v, srgb);                                                                         \
    }                                                                                                                 \
    fragment uint4 xr_target_store_bgra10##name##_frag(Out in [[stage_in]], Tile s [[texture(0)]],                    \
                                                       Codes codes [[texture(1)]], constant uint &keep [[buffer(0)]]) \
    {                                                                                                                 \
        const uint2 at = uint2(in.position.xy);                                                                       \
        const float4 v = s.read(at);                                                                                  \
        const half4 t = fieldsTile(codes.read(at), srgb);                                                             \
        if (keep && holds(v.rgb, t.rgb) && v.a == float(t.a))                                                         \
            discard_fragment();                                                                                       \
        return fieldsOf(v, srgb);                                                                                     \
    }                                                                                                                 \
    fragment uint2 xr_target_store_planes##name##_frag(Out in [[stage_in]], Tile s [[texture(0)]],                    \
                                                       Codes codes [[texture(1)]], constant uint &keep [[buffer(0)]]) \
    {                                                                                                                 \
        const uint2 at = uint2(in.position.xy);                                                                       \
        const float4 v = s.read(at);                                                                                  \
        const uint2 t = codes.read(at).rg;                                                                            \
        if (keep && holds(v.rgb, wordTile(t.x, srgb)) && v.a == float(byteTile(t.y)))                                 \
            discard_fragment();                                                                                       \
        return uint2(wordOf(v, srgb), byteOf(v.a));                                                                   \
    }                                                                                                                 \
    fragment WordAndAlpha xr_target_store_two_planes##name##_frag(                                                    \
        Out in [[stage_in]], Tile s [[texture(0)]], Codes words [[texture(1)]], Codes alphas [[texture(2)]],          \
        constant uint &keep [[buffer(0)]])                                                                            \
    {                                                                                                                 \
        const uint2 at = uint2(in.position.xy);                                                                       \
        const float4 v = s.read(at);                                                                                  \
        if (keep && holds(v.rgb, wordTile(words.read(at).r, srgb)) && v.a == float(byteTile(alphas.read(at).r)))      \
            discard_fragment();                                                                                       \
        return {wordOf(v, srgb), byteOf(v.a)};                                                                        \
    }                                                                                                                 \
    fragment float4 xr_target_resolve_bgr10##name##_frag(Out in [[stage_in]], SampleTile s [[texture(0)]],            \
                                                         constant uint &n [[buffer(0)]])                              \
    {                                                                                                                 \
        const uint2 at = uint2(in.position.xy);                                                                       \
        uint3 sum = 0;                                                                                                \
        for (uint k = 0; k < n; k++) {                                                                                \
            const float4 v = s.read(at, k);                                                                           \
            sum += uint3(colourCode(v.r, srgb), colourCode(v.g, srgb), colourCode(v.b, srgb));                        \
        }                                                                                                             \
        const uint3 c = (sum * 2 + n) / (2 * n);                                                                      \
        return srgb ? float4(srgbValue(c.x), srgbValue(c.y), srgbValue(c.z), 1.0f)                                    \
                    : float4(quotient(int(c.x) - 384, 510), quotient(int(c.y) - 384, 510),                            \
                             quotient(int(c.z) - 384, 510), 1.0f);                                                    \
    }                                                                                                                 \
    fragment float4 xr_target_resolve_bgra10##name##_frag(Out in [[stage_in]], SampleTile s [[texture(0)]],           \
                                                          constant uint &n [[buffer(0)]])                             \
    {                                                                                                                 \
        const uint2 at = uint2(in.position.xy);                                                                       \
        uint4 sum = 0;                                                                                                \
        for (uint k = 0; k < n; k++)                                                                                  \
            sum += fieldsOf(s.read(at, k), srgb) >> 6;                                                                \
        const uint4 c = uint4((sum.xyz * 2 + n) / (2 * n), (sum.w * 2 + n - 1) / (2 * n));                            \
        const half a = quotient(int(c.w) - 384, 510);                                                                 \
        return srgb ? float4(srgbValue(c.z), srgbValue(c.y), srgbValue(c.x), a)                                       \
                    : float4(quotient(int(c.z) - 384, 510), quotient(int(c.y) - 384, 510),                            \
                             quotient(int(c.x) - 384, 510), a);                                                       \
    }

XR_TARGET_FORM(, false)
XR_TARGET_FORM(_srgb, true)

// A multisampled target's stand-in: each sample loaded from and stored into that sample of the texture, the fragment run
// once a sample; a function a sample count and form, as the pack keys an own state by its function's name
#define XR_TARGET_SAMPLES(count, name, srgb)                                                                      \
    fragment half4 xr_target_load_bgr10##name##_ms##count##_frag(Out in [[stage_in]], uint k [[sample_id]],       \
                                                                 SampleCodes codes [[texture(0)]])                \
    {                                                                                                             \
        return half4(wordTile(codes.read(uint2(in.position.xy), k).r, srgb), 1.0h);                               \
    }                                                                                                             \
    fragment half4 xr_target_load_bgra10##name##_ms##count##_frag(Out in [[stage_in]], uint k [[sample_id]],      \
                                                                  SampleCodes codes [[texture(0)]])               \
    {                                                                                                             \
        return fieldsTile(codes.read(uint2(in.position.xy), k), srgb);                                            \
    }                                                                                                             \
    fragment uint xr_target_store_bgr10##name##_ms##count##_frag(                                                 \
        Out in [[stage_in]], uint k [[sample_id]], SampleTile s [[texture(0)]], SampleCodes codes [[texture(1)]], \
        constant uint &keep [[buffer(0)]])                                                                        \
    {                                                                                                             \
        const uint2 at = uint2(in.position.xy);                                                                   \
        const float4 v = s.read(at, k);                                                                           \
        if (keep && holds(v.rgb, wordTile(codes.read(at, k).r, srgb)))                                            \
            discard_fragment();                                                                                   \
        return 0xC0000000u | wordOf(v, srgb);                                                                     \
    }                                                                                                             \
    fragment uint4 xr_target_store_bgra10##name##_ms##count##_frag(                                               \
        Out in [[stage_in]], uint k [[sample_id]], SampleTile s [[texture(0)]], SampleCodes codes [[texture(1)]], \
        constant uint &keep [[buffer(0)]])                                                                        \
    {                                                                                                             \
        const uint2 at = uint2(in.position.xy);                                                                   \
        const float4 v = s.read(at, k);                                                                           \
        const half4 t = fieldsTile(codes.read(at, k), srgb);                                                      \
        if (keep && holds(v.rgb, t.rgb) && v.a == float(t.a))                                                     \
            discard_fragment();                                                                                   \
        return fieldsOf(v, srgb);                                                                                 \
    }

XR_TARGET_SAMPLES(2, , false)
XR_TARGET_SAMPLES(4, , false)
XR_TARGET_SAMPLES(2, _srgb, true)
XR_TARGET_SAMPLES(4, _srgb, true)
