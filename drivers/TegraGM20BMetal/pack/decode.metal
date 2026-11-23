// SPDX-License-Identifier: MIT
// raw GM20B path: the formats GM20B's texture unit decodes none of (runtime/decoded.h: PVRTC, ASTC 4x2 and 8x4,
// ASTC HDR), decoded once as their blocks are uploaded. One pass a level's (or plane's) region, one texel a fragment,
// into the texture's stored image viewed as integers, so each texel is the exact value an M4 Max samples
// (harness/decodeoracle.mm; astc_decode.h, pvrtc_decode.h, which the host tests compile too). Buffer 0 is the step
// (runtime/decoded.h DecodeStep), buffer 1 the blocks.
#include <metal_stdlib>
using namespace metal;

#include "astc_decode.h"
#include "pvrtc_decode.h"

struct Out {
    float4 position [[position]];
};

struct DecodeStep {
    uint x, y;              // the region's first texel in the level
    uint blockW, blockH;    // the footprint
    uint blocksX, blocksY;  // the level's blocks (PVRTC's upscale wraps across them)
    uint rowBytes;          // the source's bytes a row of blocks
    uint form;              // astc_texel.h's form; PVRTC: 1 for a level without alpha
    uint first;             // the first block's byte in buffer 1
};

struct Rows {
    device const uint *words;
    uint first, rowBytes;
};

static gm20b::decode::Words2 blockAt(Rows r, int x, int y)
{
    const uint at = (r.first + uint(y) * r.rowBytes + uint(x) * 8) / 4;
    return {r.words[at], r.words[at + 1]};
}

vertex Out decode_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

static gm20b::decode::Texel astcTexelAt(Out in, constant DecodeStep &s, device const uint *words)
{
    const uint2 p = uint2(in.position.xy);
    const uint2 block = (p - uint2(s.x, s.y)) / uint2(s.blockW, s.blockH);
    const uint at = (s.first + block.y * s.rowBytes + block.x * 16) / 4;
    const gm20b::decode::Words4 b = {words[at], words[at + 1], words[at + 2], words[at + 3]};
    return gm20b::decode::astcTexel(b, int(s.blockW), int(s.blockH), int(p.x % s.blockW), int(p.y % s.blockH),
                                  int(s.form));
}

// sRGB 4x2 and 8x4: the 8-bit codes, into RGBA8Unorm_sRGB's words viewed as R32Uint
fragment uint decode_astc_unorm8_frag(Out in [[stage_in]], constant DecodeStep &s [[buffer(0)]],
                                      device const uint *words [[buffer(1)]])
{
    const gm20b::decode::Texel t = astcTexelAt(in, s, words);
    return t.r | t.g << 8 | t.b << 16 | t.a << 24;
}

// Linear 4x2 and 8x4: Apple's 12-bit k / 4095 as its nearest 16-bit code, into RGBA16Unorm's words viewed as RGBA16Uint
fragment uint4 decode_astc_unorm16_frag(Out in [[stage_in]], constant DecodeStep &s [[buffer(0)]],
                                        device const uint *words [[buffer(1)]])
{
    const gm20b::decode::Texel t = astcTexelAt(in, s, words);
    return uint4(gm20b::decode::unorm12To16(t.r), gm20b::decode::unorm12To16(t.g), gm20b::decode::unorm12To16(t.b),
                 gm20b::decode::unorm12To16(t.a));
}

// HDR: the half bits, into RGBA16Float's words viewed as RGBA16Uint
fragment uint4 decode_astc_half_frag(Out in [[stage_in]], constant DecodeStep &s [[buffer(0)]],
                                     device const uint *words [[buffer(1)]])
{
    const gm20b::decode::Texel t = astcTexelAt(in, s, words);
    return uint4(t.r, t.g, t.b, t.a);
}

// PVRTC: the whole level's blocks in rows (a blit's MTLBlitOptionRowLinearPVRTC order), into RGBA8Unorm's or its sRGB
// form's words viewed as R32Uint; the RGB formats read alpha one
fragment uint decode_pvrtc_frag(Out in [[stage_in]], constant DecodeStep &s [[buffer(0)]],
                                device const uint *words [[buffer(1)]])
{
    const Rows rows = {words, s.first, s.rowBytes};
    const uint2 p = uint2(in.position.xy);
    const gm20b::decode::Texel t =
        gm20b::decode::pvrtcTexel(rows, int(p.x), int(p.y), int(s.blocksX), int(s.blocksY), s.blockW == 8);
    return t.r | t.g << 8 | t.b << 16 | (s.form == 1 ? 0xffu : t.a) << 24;
}
