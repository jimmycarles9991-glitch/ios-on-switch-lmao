// SPDX-License-Identifier: MIT
// raw GM20B path: Metal's store of a pass into a two-plane YCbCr attachment (runtime/raw_ycbcr_store_rule.h), drawn from the
// pass's RGBA32Float stand-in into the planes: the luma plane one texel a fragment, the chroma plane one block a fragment.
//  - codes (RGB*_2P): each texel held as a half (a drawn float rounded to nearest even, or toward zero for 10-bit codes), its
//    code round(clamp(h, 0, 1) * max) with ties up, a NaN 0; a chroma block's code (sum + n / 2) / n of its n texels' codes;
//  - converted (YCBCR*_2P, m2v bind/ycbcr_write.rs): a channel's step x of its held half on the grid of 1/8192, 4 rint(h * 2048),
//    or through the sRGB encode E[rint(h * 8192)] (the _srgb functions, srgb_encode.h); a code's term
//    t = floor(row . x * mul / 2^20) >> (8 - T), T its bits past a code (6 at 8 bits, 4 at 10); the code
//    offset + ((sum t + 2^(T - 1 + k)) >> (T + k)) over a block of 2^k texels, clamped to [0, max].
// The codes are written as code << shift over the target's whole range, so the ROP's unorm store of the exact quotient is the
// code. A packed target's (the _packed functions, R32Uint planes) are its unpacked twin's codes, three to a little-endian
// word at bits 0, 10 and 20, as Metal reads them (m2v bind/ycbcr_convert.rs): luma code x in word x / 3, chroma pair x as
// codes 2 x (Cb) and 2 x + 1 (Cr); bits 30 and 31 and a row's slots past its last code 0. One function a target format: the
// pack keys an own state by its function's name. Built without fast math (rawpack.py): the NaN tests and the half rounding
// are kept.
#include <metal_stdlib>
using namespace metal;

struct StoreShape {
    uint max;         // the largest code: 255 or 1023
    uint shift;       // a code's place in its unorm word: 0, or 6 for a 10-bit code in 16 bits
    uint towardZero;  // drawn floats held toward zero
    uint across;      // chroma blocks two texels across
    uint down;        // and two down
    uint converted;   // the matrix rule, else codes
    int offset[3];    // converted: luma's, Cb's and Cr's
    int row[3][3];    // and each code's red, green and blue coefficients
    uint mul[3];      // and its terms' scale over 2^20
    uint termBits;    // and a term's bits past a code
    uint srgb;        // the runtime's choice of the _srgb functions
    uint packed;      // and of the _packed ones
    uint plane;       // a packed 10-bit plane's codes a texel (ycbcr_store_plane_packed_frag)
};

struct Out {
    float4 position [[position]];
};

vertex Out ycbcr_store_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

typedef texture2d<float, access::read> StandIn;

static float held(float v, bool towardZero)
{
    half h = half(v);
    if (towardZero && float(h) > v)
        h = as_type<half>(ushort(as_type<ushort>(h) - 1));
    return float(h);
}

static uint texelCode(float v, constant StoreShape &s)
{
    if (isnan(v) || v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return s.max;
    return uint(floor(held(v, s.towardZero != 0) * float(s.max) + 0.5f));
}

// pack/srgb_encode.h, read from a constant buffer the runtime binds at buffer(1) (raw_ycbcr_store.h kSrgbEncodeSlot): a
// program-scope array of its 16 KiB would be private memory a thread, past m2v's 16384-byte limit and the constant cache
typedef constant ushort *Encode;

// A held half's step: specialised, so only the _srgb functions read the table
template <bool Srgb> static int heldStep(float h, Encode e);

template <> int heldStep<false>(float h, Encode)
{
    return 4 * int(rint(h * 2048.0f));
}

template <> int heldStep<true>(float h, Encode e)
{
    return int(e[uint(rint(h * 8192.0f))]);
}

template <bool Srgb> static int gridStep(float v, constant StoreShape &s, Encode e)
{
    if (isnan(v) || v <= 0.0f)
        return 0;
    if (v >= 1.0f)
        return 8192;
    return heldStep<Srgb>(held(v, s.towardZero != 0), e);
}

template <bool Srgb> static int3 gridSteps(float3 rgb, constant StoreShape &s, Encode e)
{
    return int3(gridStep<Srgb>(rgb.r, s, e), gridStep<Srgb>(rgb.g, s, e), gridStep<Srgb>(rgb.b, s, e));
}

// floor(S * mul / 2^20) in 32-bit steps: S = 4096 q + r, q mul = 256 s + u
static int scaled(int sum, uint mul)
{
    const int q = sum >> 12, r = sum & 4095, qm = q * int(mul);
    return (qm >> 8) + (((qm & 255) * 4096 + r * int(mul)) >> 20);
}

static int term(constant StoreShape &s, uint code, int3 x)
{
    int sum = 0;
    for (uint c = 0; c < 3; c++)
        sum += s.row[code][c] * x[c];
    return scaled(sum, s.mul[code]) >> (8 - s.termBits);
}

static uint encoded(constant StoreShape &s, uint code, int sum, uint k)
{
    const uint shift = s.termBits + k;
    return uint(clamp(s.offset[code] + ((sum + (1 << (shift - 1))) >> shift), 0, int(s.max)));
}

template <bool Srgb> static uint lumaCode(StandIn stand_in, constant StoreShape &s, uint2 at, Encode e)
{
    const float4 v = stand_in.read(at);
    return s.converted ? encoded(s, 0, term(s, 0, gridSteps<Srgb>(v.rgb, s, e)), 0) : texelCode(v.g, s);
}

// The block's Cb and Cr codes
template <bool Srgb> static uint2 chromaCodes(StandIn stand_in, constant StoreShape &s, uint2 block, Encode e)
{
    const uint w = s.across ? 2 : 1, h = s.down ? 2 : 1, n = w * h, k = (s.across ? 1 : 0) + (s.down ? 1 : 0);
    const uint2 first = block * uint2(w, h);
    int cb = 0, cr = 0;
    for (uint dy = 0; dy < h; dy++)
        for (uint dx = 0; dx < w; dx++) {
            const float4 v = stand_in.read(first + uint2(dx, dy));
            if (s.converted) {
                const int3 x = gridSteps<Srgb>(v.rgb, s, e);
                cb += term(s, 1, x);
                cr += term(s, 2, x);
            } else {
                cb += int(texelCode(v.b, s));
                cr += int(texelCode(v.r, s));
            }
        }
    if (s.converted)
        return uint2(encoded(s, 1, cb, k), encoded(s, 2, cr, k));
    return (uint2(cb, cr) + n / 2) / n;
}

fragment float ycbcr_store_luma8_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                      constant StoreShape &s [[buffer(0)]])
{
    return float(lumaCode<false>(stand_in, s, uint2(in.position.xy), nullptr)) / 255.0f;
}

fragment float ycbcr_store_luma16_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                       constant StoreShape &s [[buffer(0)]])
{
    return float(lumaCode<false>(stand_in, s, uint2(in.position.xy), nullptr) << s.shift) / 65535.0f;
}

fragment float4 ycbcr_store_chroma8_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                         constant StoreShape &s [[buffer(0)]])
{
    return float4(float2(chromaCodes<false>(stand_in, s, uint2(in.position.xy), nullptr)) / 255.0f, 0.0f, 1.0f);
}

fragment float4 ycbcr_store_chroma16_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                          constant StoreShape &s [[buffer(0)]])
{
    return float4(float2(chromaCodes<false>(stand_in, s, uint2(in.position.xy), nullptr) << s.shift) / 65535.0f, 0.0f, 1.0f);
}

fragment float ycbcr_store_luma8_srgb_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                           constant StoreShape &s [[buffer(0)]], Encode encode [[buffer(1)]])
{
    return float(lumaCode<true>(stand_in, s, uint2(in.position.xy), encode)) / 255.0f;
}

fragment float ycbcr_store_luma16_srgb_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                            constant StoreShape &s [[buffer(0)]], Encode encode [[buffer(1)]])
{
    return float(lumaCode<true>(stand_in, s, uint2(in.position.xy), encode) << s.shift) / 65535.0f;
}

fragment float4 ycbcr_store_chroma8_srgb_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                              constant StoreShape &s [[buffer(0)]], Encode encode [[buffer(1)]])
{
    return float4(float2(chromaCodes<true>(stand_in, s, uint2(in.position.xy), encode)) / 255.0f, 0.0f, 1.0f);
}

fragment float4 ycbcr_store_chroma16_srgb_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                               constant StoreShape &s [[buffer(0)]], Encode encode [[buffer(1)]])
{
    return float4(float2(chromaCodes<true>(stand_in, s, uint2(in.position.xy), encode) << s.shift) / 65535.0f, 0.0f, 1.0f);
}

// Word `at` of a packed luma row: codes 3 x, 3 x + 1 and 3 x + 2
template <bool Srgb> static uint packedLuma(StandIn stand_in, constant StoreShape &s, uint2 at, Encode e)
{
    const uint width = stand_in.get_width();
    uint word = 0;
    for (uint i = 0; i < 3; i++) {
        const uint x = 3 * at.x + i;
        if (x < width)
            word |= lumaCode<Srgb>(stand_in, s, uint2(x, at.y), e) << (10 * i);
    }
    return word;
}

// Word `at` of a packed chroma row: codes 3 x .. 3 x + 2 of its pairs, the pairs of blocks 3 x / 2 and the one after it
template <bool Srgb> static uint packedChroma(StandIn stand_in, constant StoreShape &s, uint2 at, Encode e)
{
    const uint blocks = stand_in.get_width() / (s.across ? 2 : 1), first = 3 * at.x;
    const uint block = first / 2;
    const uint2 pairs[2] = {block < blocks ? chromaCodes<Srgb>(stand_in, s, uint2(block, at.y), e) : uint2(0),
                            block + 1 < blocks ? chromaCodes<Srgb>(stand_in, s, uint2(block + 1, at.y), e) : uint2(0)};
    uint word = 0;
    for (uint i = 0; i < 3; i++) {
        const uint code = first + i, pair = code / 2 - block;
        word |= (code & 1 ? pairs[pair].y : pairs[pair].x) << (10 * i);
    }
    return word;
}

fragment uint ycbcr_store_luma_packed_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                           constant StoreShape &s [[buffer(0)]])
{
    return packedLuma<false>(stand_in, s, uint2(in.position.xy), nullptr);
}

fragment uint ycbcr_store_chroma_packed_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                             constant StoreShape &s [[buffer(0)]])
{
    return packedChroma<false>(stand_in, s, uint2(in.position.xy), nullptr);
}

fragment uint ycbcr_store_luma_packed_srgb_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                constant StoreShape &s [[buffer(0)]], Encode encode [[buffer(1)]])
{
    return packedLuma<true>(stand_in, s, uint2(in.position.xy), encode);
}

fragment uint ycbcr_store_chroma_packed_srgb_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                  constant StoreShape &s [[buffer(0)]], Encode encode [[buffer(1)]])
{
    return packedChroma<true>(stand_in, s, uint2(in.position.xy), encode);
}

// Word `at` of a row of R10Unorm_PACKED's or RG10Unorm_PACKED's plane (runtime/raw_packed_plane.h): code n of the row is
// channel n % k of texel n / k, red then green, each a 10-bit code (max 1023, held toward zero)
fragment uint ycbcr_store_plane_packed_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                            constant StoreShape &s [[buffer(0)]])
{
    const uint2 at = uint2(in.position.xy);
    const uint codes = stand_in.get_width() * s.plane;
    uint word = 0;
    for (uint i = 0; i < 3; i++) {
        const uint n = 3 * at.x + i;
        if (n < codes)
            word |= texelCode(stand_in.read(uint2(n / s.plane, at.y))[n % s.plane], s) << (10 * i);
    }
    return word;
}
