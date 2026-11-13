// SPDX-License-Identifier: MIT
// raw GM20B path (r8-rawca): a two-plane YCbCr surface's texture as Metal reads it, drawn into the image the raw path samples in
// its place, one texel a fragment. The arithmetic is m2v's (metal2vulkan bind/ycbcr_convert.rs, its kernel and `ycbcr_read`),
// which reproduces an Apple M4's measured reads exactly:
//  - codes: a byte, or the high 10 bits of a 16-bit word, or (the _packed functions, R32Uint planes) one of three 10-bit codes
//    of a little-endian 32-bit word at its bits 0, 10 and 20: luma code x in word x / 3, chroma pair x as codes 2 x and
//    2 x + 1, read as the unpacked formats' of the same codes;
//  - chroma along a halved axis: the two nearest chroma texels weighted 3/1 (column x/2 and the next for an odd x, the one before
//    for an even x, each clamped to the plane), along a whole one its own texel weighted 4; the 2x2 sum S of weight 16 rounds
//    to a code, C = (S + 8) >> 4 (4:2:0's 9/3/3/1, 4:2:2's (S' + 2) >> 2, 4:4:4's own pair alike);
//  - converted (YCBCR*_2P): with u = 10 - bits, Cb' = (Cb << u) - 512, Cr' = (Cr << u) - 512 and the luma term L (video range
//    9576 ((Y << u) - 64), full range 256 floor((32768 Y + max / 2) / max), max = 2^bits - 1), each channel
//    v = clamp((L + kb Cb' + kr Cr' + 2048) >> 12, 0, 2048) of its (kb, kr), read as v / 2048 (an RGBA16Float image holds it
//    exactly), alpha 1;
//  - sRGB (YCBCR*_2P_sRGB): q = clamp((T + 512) >> 10, 0, 8192) of the same sum T through the knots of the sRGB curve, read as
//    step / 8192 (RGBA32Float), alpha 1;
//  - codes (RGB*_2P): red Cr, green Y, blue Cb, each code / max (RGBA8Unorm, RGB10A2Unorm), alpha 1.
// Texture 0 is the luma plane (R8Unorm or R16Unorm, a code a texel), texture 1 the chroma plane (RG8Unorm or RG16Unorm, Cb then
// Cr a texel), buffer 0 the shape (raw_ycbcr.h); the vertex function draws the image's rectangle as a 4-vertex strip.
#include <metal_stdlib>
using namespace metal;

struct Shape {
    uint width, height;  // the texture's, in luma texels
    int full;            // full range, else video range
    int k[6];            // red's kb, kr, then green's and blue's (m2v's `coefficients` of the range and matrix)
    uint unit;           // a texel's largest value read as a whole number: 255 or 65535
    uint drop;           // the low bits under a code
    uint up;             // 10 - bits
    uint max;            // the largest code
    uint halved;         // bit 0 across, bit 1 down
    uint chroma;         // 0: Cb and Cr codes 0 (a texture over plane 1)
};

struct Out {
    float4 position [[position]];
};

vertex Out ycbcr_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

static uint code(float v, constant Shape &s)
{
    return uint(rint(v * float(s.unit))) >> s.drop;
}

typedef texture2d<float, access::read> Plane;
typedef texture2d<uint, access::read> PackedPlane;

// code x of row y of a packed plane
static uint packedCode(PackedPlane p, uint x, uint y)
{
    return (p.read(uint2(x / 3, y)).r >> (10 * (x % 3))) & 1023;
}

static uint lumaCode(Plane luma, constant Shape &s, uint2 at)
{
    return code(luma.read(at).r, s);
}

static uint lumaCode(PackedPlane luma, constant Shape &, uint2 at)
{
    return packedCode(luma, at.x, at.y);
}

// chroma pair `at`'s Cb and Cr codes
static uint2 pairCodes(Plane chroma, constant Shape &s, uint2 at)
{
    const float2 pair = chroma.read(at).rg;
    return uint2(code(pair.x, s), code(pair.y, s));
}

static uint2 pairCodes(PackedPlane chroma, constant Shape &, uint2 at)
{
    return uint2(packedCode(chroma, 2 * at.x, at.y), packedCode(chroma, 2 * at.x + 1, at.y));
}

// the two chroma texels of luma column (or row) p of a plane n across, and the first one's weight (the second's is 4 less it)
static uint3 taps(uint p, uint n, bool halved)
{
    if (!halved)
        return uint3(p, p, 4);
    const uint c = p >> 1, odd = p & 1, a = c + odd, last = n - 1;
    const uint second = a > last ? last : a;
    const uint first = a == 0 ? 0 : a - 1;
    return uint3(odd ? c : first, second, odd ? 3 : 1);
}

// the texel's luma code and its filtered chroma codes (Cb, Cr)
template <typename P> static uint3 codes(P luma, P chroma, constant Shape &s, uint2 at)
{
    const uint y = lumaCode(luma, s, at);
    if (!s.chroma)
        return uint3(y, 0, 0);
    const bool across = s.halved & 1, down = s.halved & 2;
    const uint3 tx = taps(at.x, across ? (s.width + 1) >> 1 : s.width, across);
    const uint3 ty = taps(at.y, down ? (s.height + 1) >> 1 : s.height, down);
    const uint wx[2] = {tx.z, 4 - tx.z}, wy[2] = {ty.z, 4 - ty.z};
    const uint cx[2] = {tx.x, tx.y}, cy[2] = {ty.x, ty.y};
    uint cb = 0, cr = 0;
    for (uint j = 0; j < 2; j++)
        for (uint i = 0; i < 2; i++) {
            const uint2 pair = pairCodes(chroma, s, uint2(cx[i], cy[j]));
            const uint w = wx[i] * wy[j];
            cb += w * pair.x;
            cr += w * pair.y;
        }
    return uint3(y, (cb + 8) >> 4, (cr + 8) >> 4);
}

// T = L + kb Cb' + kr Cr' for each channel
static int3 sums(uint3 c, constant Shape &s)
{
    const int luma = s.full ? 256 * int((c.x * 32768 + s.max / 2) / s.max) : 9576 * (int(c.x << s.up) - 64);
    const int cb = int(c.y << s.up) - 512, cr = int(c.z << s.up) - 512;
    return int3(luma + s.k[0] * cb + s.k[1] * cr, luma + s.k[2] * cb + s.k[3] * cr, luma + s.k[4] * cb + s.k[5] * cr);
}

fragment float4 ycbcr_converted_frag(Out in [[stage_in]], texture2d<float, access::read> luma [[texture(0)]],
                                     texture2d<float, access::read> chroma [[texture(1)]],
                                     constant Shape &s [[buffer(0)]])
{
    const int3 t = sums(codes(luma, chroma, s, uint2(in.position.xy)), s);
    const int3 v = clamp((t + 2048) >> 12, 0, 2048);
    return float4(float3(v) / 2048.0f, 1.0f);
}

constant ushort kSrgbKnots[513] = {
#include "srgb_knots.h"
};

static float srgbStep(int t)
{
    const uint q = uint(clamp((t + 512) >> 10, 0, 8192));
    const uint i = min(q >> 4, 511u), f = q - 16 * i;
    const uint a = kSrgbKnots[i], b = kSrgbKnots[i + 1];
    const uint n = 16 * a + (b - a) * f;
    return float((n + 7 + ((n >> 4) & 1)) >> 4) / 8192.0f;
}

fragment float4 ycbcr_srgb_frag(Out in [[stage_in]], texture2d<float, access::read> luma [[texture(0)]],
                                texture2d<float, access::read> chroma [[texture(1)]], constant Shape &s [[buffer(0)]])
{
    const int3 t = sums(codes(luma, chroma, s, uint2(in.position.xy)), s);
    return float4(srgbStep(t.x), srgbStep(t.y), srgbStep(t.z), 1.0f);
}

template <typename P> static float4 codesTexel(P luma, P chroma, constant Shape &s, uint2 at)
{
    const float3 c = float3(codes(luma, chroma, s, at)) / float(s.max);
    return float4(c.z, c.x, c.y, 1.0f);
}

// RGBA8Unorm's image (8-bit codes)
fragment float4 ycbcr_codes_frag(Out in [[stage_in]], texture2d<float, access::read> luma [[texture(0)]],
                                 texture2d<float, access::read> chroma [[texture(1)]], constant Shape &s [[buffer(0)]])
{
    return codesTexel(luma, chroma, s, uint2(in.position.xy));
}

// RGB10A2Unorm's image (10-bit codes): its own state, the pack keying the raw path's states by fragment function
fragment float4 ycbcr_codes10_frag(Out in [[stage_in]], texture2d<float, access::read> luma [[texture(0)]],
                                   texture2d<float, access::read> chroma [[texture(1)]], constant Shape &s [[buffer(0)]])
{
    return codesTexel(luma, chroma, s, uint2(in.position.xy));
}

fragment float4 ycbcr_packed_converted_frag(Out in [[stage_in]], PackedPlane luma [[texture(0)]],
                                            PackedPlane chroma [[texture(1)]], constant Shape &s [[buffer(0)]])
{
    const int3 t = sums(codes(luma, chroma, s, uint2(in.position.xy)), s);
    const int3 v = clamp((t + 2048) >> 12, 0, 2048);
    return float4(float3(v) / 2048.0f, 1.0f);
}

fragment float4 ycbcr_packed_srgb_frag(Out in [[stage_in]], PackedPlane luma [[texture(0)]],
                                       PackedPlane chroma [[texture(1)]], constant Shape &s [[buffer(0)]])
{
    const int3 t = sums(codes(luma, chroma, s, uint2(in.position.xy)), s);
    return float4(srgbStep(t.x), srgbStep(t.y), srgbStep(t.z), 1.0f);
}

fragment float4 ycbcr_packed_codes10_frag(Out in [[stage_in]], PackedPlane luma [[texture(0)]],
                                          PackedPlane chroma [[texture(1)]], constant Shape &s [[buffer(0)]])
{
    return codesTexel(luma, chroma, s, uint2(in.position.xy));
}

// R10Unorm_PACKED's and RG10Unorm_PACKED's image (runtime/raw_packed_plane.h): texture 0 the plane's words, buffer 0 the
// texture's width, height and codes a texel (k); texel x's codes are codes k x and k x + 1 of its row
struct PlaneShape {
    uint width, height, channels;
};

fragment float4 ycbcr_packed_plane_frag(Out in [[stage_in]], PackedPlane words [[texture(0)]],
                                        constant PlaneShape &s [[buffer(0)]])
{
    const uint2 at = uint2(in.position.xy);
    const uint first = s.channels * at.x;
    const float r = float(packedCode(words, first, at.y)) / 1023.0f;
    const float g = s.channels == 2 ? float(packedCode(words, first + 1, at.y)) / 1023.0f : 0.0f;
    return float4(r, g, 0.0f, 1.0f);
}
