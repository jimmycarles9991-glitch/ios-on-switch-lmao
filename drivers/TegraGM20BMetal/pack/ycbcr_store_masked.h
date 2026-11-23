// SPDX-License-Identifier: MIT
// Part of ycbcr_store.metal, included in place after its plain store functions, whose shapes and helpers it uses
#pragma once

// A loaded pass's store (runtime/ycbcr_load.h): Metal stores only the 32x32 tiles of the pass's frame its draws reach, the
// rest of each plane keeping its bytes. `reach` holds a texel a tile, nonzero where a draw reaches it (runtime/reach.h); a
// chroma block lies in the tile of its first luma texel. Luma texel (x, y) of the plane is frame pixel (ax x + ay y + cx,
// bx x + by y + cy): a turned target's tiles are its frame's, from the frame's origin (runtime/reach.h TileMap). A packed
// word's codes each keep the plane's code where their tile is not reached, read from the word the plane holds (`kept`, the
// target itself: each fragment reads its own word alone).
struct Tiles {
    int ax, ay, cx, bx, by, cy;
    int tilesX, tilesY;
};

typedef texture2d<uint, access::read> Reach;

static bool reached(Reach reach, constant Tiles &t, uint2 pixel)
{
    const int2 p = int2(pixel);
    const int2 f = int2(t.ax * p.x + t.ay * p.y + t.cx, t.bx * p.x + t.by * p.y + t.cy);
    if (f.x < 0 || f.y < 0)
        return false;
    const int2 tile = f / 32;
    return tile.x < t.tilesX && tile.y < t.tilesY && reach.read(uint2(tile)).r != 0;
}

static uint2 blockPixel(constant StoreShape &s, uint2 block)
{
    return block * uint2(s.across ? 2 : 1, s.down ? 2 : 1);
}

fragment float ycbcr_store_luma8_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                             Reach reach [[texture(1)]], constant StoreShape &s [[buffer(0)]],
                                             constant Tiles &tiles [[buffer(2)]])
{
    if (!reached(reach, tiles, uint2(in.position.xy)))
        discard_fragment();
    return float(lumaCode<false>(stand_in, s, uint2(in.position.xy), nullptr)) / 255.0f;
}

fragment float ycbcr_store_luma16_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                              Reach reach [[texture(1)]], constant StoreShape &s [[buffer(0)]],
                                              constant Tiles &tiles [[buffer(2)]])
{
    if (!reached(reach, tiles, uint2(in.position.xy)))
        discard_fragment();
    return float(lumaCode<false>(stand_in, s, uint2(in.position.xy), nullptr) << s.shift) / 65535.0f;
}

fragment float4 ycbcr_store_chroma8_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                Reach reach [[texture(1)]], constant StoreShape &s [[buffer(0)]],
                                                constant Tiles &tiles [[buffer(2)]])
{
    if (!reached(reach, tiles, blockPixel(s, uint2(in.position.xy))))
        discard_fragment();
    return float4(float2(chromaCodes<false>(stand_in, s, uint2(in.position.xy), nullptr)) / 255.0f, 0.0f, 1.0f);
}

fragment float4 ycbcr_store_chroma16_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                 Reach reach [[texture(1)]], constant StoreShape &s [[buffer(0)]],
                                                 constant Tiles &tiles [[buffer(2)]])
{
    if (!reached(reach, tiles, blockPixel(s, uint2(in.position.xy))))
        discard_fragment();
    return float4(float2(chromaCodes<false>(stand_in, s, uint2(in.position.xy), nullptr) << s.shift) / 65535.0f, 0.0f,
                  1.0f);
}

fragment float ycbcr_store_luma8_srgb_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                  Reach reach [[texture(1)]], constant StoreShape &s [[buffer(0)]],
                                                  Encode encode [[buffer(1)]], constant Tiles &tiles [[buffer(2)]])
{
    if (!reached(reach, tiles, uint2(in.position.xy)))
        discard_fragment();
    return float(lumaCode<true>(stand_in, s, uint2(in.position.xy), encode)) / 255.0f;
}

fragment float ycbcr_store_luma16_srgb_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                   Reach reach [[texture(1)]], constant StoreShape &s [[buffer(0)]],
                                                   Encode encode [[buffer(1)]], constant Tiles &tiles [[buffer(2)]])
{
    if (!reached(reach, tiles, uint2(in.position.xy)))
        discard_fragment();
    return float(lumaCode<true>(stand_in, s, uint2(in.position.xy), encode) << s.shift) / 65535.0f;
}

fragment float4 ycbcr_store_chroma8_srgb_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                     Reach reach [[texture(1)]], constant StoreShape &s [[buffer(0)]],
                                                     Encode encode [[buffer(1)]], constant Tiles &tiles [[buffer(2)]])
{
    if (!reached(reach, tiles, blockPixel(s, uint2(in.position.xy))))
        discard_fragment();
    return float4(float2(chromaCodes<true>(stand_in, s, uint2(in.position.xy), encode)) / 255.0f, 0.0f, 1.0f);
}

fragment float4 ycbcr_store_chroma16_srgb_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                      Reach reach [[texture(1)]], constant StoreShape &s [[buffer(0)]],
                                                      Encode encode [[buffer(1)]], constant Tiles &tiles [[buffer(2)]])
{
    if (!reached(reach, tiles, blockPixel(s, uint2(in.position.xy))))
        discard_fragment();
    return float4(float2(chromaCodes<true>(stand_in, s, uint2(in.position.xy), encode) << s.shift) / 65535.0f, 0.0f,
                  1.0f);
}

typedef texture2d<uint, access::read> Kept;

// Word `at` of a packed luma row, each code of a tile no draw reaches the plane's own
template <bool Srgb>
static uint packedLumaMasked(StandIn stand_in, constant StoreShape &s, uint2 at, Encode e, Reach reach,
                             constant Tiles &tiles, Kept kept)
{
    const uint width = stand_in.get_width();
    uint word = 0, mine = 0;
    for (uint i = 0; i < 3; i++) {
        const uint x = 3 * at.x + i;
        if (x < width && reached(reach, tiles, uint2(x, at.y))) {
            word |= lumaCode<Srgb>(stand_in, s, uint2(x, at.y), e) << (10 * i);
            mine |= 1023u << (10 * i);
        }
    }
    if (!mine)
        discard_fragment();
    return word | (kept.read(at).r & ~mine);
}

// Word `at` of a packed chroma row, likewise by each code's block
template <bool Srgb>
static uint packedChromaMasked(StandIn stand_in, constant StoreShape &s, uint2 at, Encode e, Reach reach,
                               constant Tiles &tiles, Kept kept)
{
    const uint blocks = (stand_in.get_width() + (s.across ? 1 : 0)) / (s.across ? 2 : 1), first = 3 * at.x;
    uint word = 0, mine = 0;
    for (uint i = 0; i < 3; i++) {
        const uint block = (first + i) / 2;
        if (block < blocks && reached(reach, tiles, blockPixel(s, uint2(block, at.y)))) {
            const uint2 pair = chromaCodes<Srgb>(stand_in, s, uint2(block, at.y), e);
            word |= ((first + i) & 1 ? pair.y : pair.x) << (10 * i);
            mine |= 1023u << (10 * i);
        }
    }
    if (!mine)
        discard_fragment();
    return word | (kept.read(at).r & ~mine);
}

fragment uint ycbcr_store_luma_packed_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                  Reach reach [[texture(1)]], Kept kept [[texture(2)]],
                                                  constant StoreShape &s [[buffer(0)]],
                                                  constant Tiles &tiles [[buffer(2)]])
{
    return packedLumaMasked<false>(stand_in, s, uint2(in.position.xy), nullptr, reach, tiles, kept);
}

fragment uint ycbcr_store_chroma_packed_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                    Reach reach [[texture(1)]], Kept kept [[texture(2)]],
                                                    constant StoreShape &s [[buffer(0)]],
                                                    constant Tiles &tiles [[buffer(2)]])
{
    return packedChromaMasked<false>(stand_in, s, uint2(in.position.xy), nullptr, reach, tiles, kept);
}

fragment uint ycbcr_store_luma_packed_srgb_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                       Reach reach [[texture(1)]], Kept kept [[texture(2)]],
                                                       constant StoreShape &s [[buffer(0)]],
                                                       Encode encode [[buffer(1)]], constant Tiles &tiles [[buffer(2)]])
{
    return packedLumaMasked<true>(stand_in, s, uint2(in.position.xy), encode, reach, tiles, kept);
}

fragment uint ycbcr_store_chroma_packed_srgb_masked_frag(Out in [[stage_in]], StandIn stand_in [[texture(0)]],
                                                         Reach reach [[texture(1)]], Kept kept [[texture(2)]],
                                                         constant StoreShape &s [[buffer(0)]],
                                                         Encode encode [[buffer(1)]],
                                                         constant Tiles &tiles [[buffer(2)]])
{
    return packedChromaMasked<true>(stand_in, s, uint2(in.position.xy), encode, reach, tiles, kept);
}
