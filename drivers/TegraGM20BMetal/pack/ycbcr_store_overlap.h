// SPDX-License-Identifier: MIT
// Part of ycbcr_store.metal, included in place after its one-plane word store functions, whose shapes and helpers it uses
#pragma once

// A target whose plane rows overlap (runtime/overlap_rows.h OverlapRows, overlapOrder, copyOrder and overlapWinner step
// for step; `copied`: a copy's stores in copyOrder):
// each plane byte from the pool image of its rows' own texels, the row whose store lands last as an M4 Max stores a pass:
// tiles of 64x32 frame texels from the last tile row up and each row's last tile back (at a quarter turn the 64-wide frame
// column further left later, then the tile further down), a tile by 32-texel column, then 2x2 quad row, then quad column,
// the even row of a quad after the odd one. The target is the plane's bytes `pitch` apart from `base`; a texel of the
// source is a texel of the target, its bits moved as the unorm it is. A one-plane 4:2:2 target's bytes come from its luma
// (texture 0) and chroma (texture 2) images.
struct OverlapRows {
    uint pitch, row, rows;
    uint element;
    uint base;
    uint texelBytes, across, down;
    uint masked;
    uint kept;
    uint pairs;
    uint copied;
    Tiles tiles;
};

static bool overlapReached(Reach reach, constant OverlapRows &s, uint2 pixel)
{
    return reached(reach, s.tiles, pixel);
}

static uint inTileOrder(uint x, uint y)
{
    return (x % 64 / 32) << 9 | (y % 32 / 2) << 5 | (x % 32 / 2) << 1 | ((y & 1) == 0 ? 1u : 0u);
}

static uint overlapOrder(uint x, uint y)
{
    const uint tileRow = y / 32, tileColumn = x / 64;
    return (0x3ffu - tileRow) << 18 | (0xffu - tileColumn) << 10 | inTileOrder(x, y);
}

static uint copyOrder(uint x, uint y)
{
    return (0x3ffu - y / 32) << 18 | (0xffu - x / 32) << 10 | (y % 32 / 2) << 5 | (x % 32 / 2) << 1 |
           ((y & 1) == 0 ? 1u : 0u);
}

static uint2 frameTexel(constant Tiles &t, uint x, uint y, uint across, uint down)
{
    const int2 p = int2(int(x), int(y)), s = int2(int(across) - 1, int(down) - 1);
    return uint2(int2(t.ax * p.x + t.ay * p.y + t.cx + min(t.ax, 0) * s.x + min(t.ay, 0) * s.y,
                      t.bx * p.x + t.by * p.y + t.cy + min(t.bx, 0) * s.x + min(t.by, 0) * s.y));
}

static uint clearOrder(constant Tiles &t, uint2 f)
{
    if (t.ax == 0)
        return f.x << 16 | f.y;
    const uint column = f.x / 32;
    return (column & 1) << 29 | (f.y / 32 & 0x3ffu) << 19 | (column & 0x3ffu) << 9 | (f.y % 32 / 2) << 5 |
           (f.x % 32 / 2) << 1 | ((f.y & 1) == 0 ? 1u : 0u);
}

static uint frameOrder(constant Tiles &t, uint2 f)
{
    if (t.ax != 0)
        return overlapOrder(f.x, f.y);
    return (0x3ffu - f.x / 64) << 22 | (f.y / 32) << 10 | inTileOrder(f.x, f.y);
}

static bool overlapWinner(constant OverlapRows &s, Reach reach, uint a, thread uint &row, thread uint &offset)
{
    const uint last = min(a / s.pitch, s.rows - 1);
    const uint first = a >= s.row ? (a - s.row) / s.pitch + 1 : 0;
    bool found = false;
    uint best = 0;
    for (uint r = first; r <= last; r++) {
        const uint off = a - r * s.pitch, x = off * s.across / s.texelBytes, y = r * s.down;
        if (off == s.kept || (s.masked != 0 && !overlapReached(reach, s, uint2(x, y))))
            continue;
        const uint2 f = frameTexel(s.tiles, x, y, s.across, s.down);
        const uint order = s.copied != 0 ? copyOrder(f.x, f.y) : frameOrder(s.tiles, f);
        if (!found || order > best) {
            found = true;
            best = order;
            row = r;
            offset = off;
        }
    }
    return found;
}

static float4 overlapTexel(StandIn source, Reach reach, StandIn chroma, constant OverlapRows &s, uint2 at)
{
    uint row = 0, off = 0;
    if (!overlapWinner(s, reach, s.base + at.y * s.pitch + at.x * s.element, row, off)) {
        discard_fragment();
        return float4(0.0f);
    }
    if (s.pairs == 0)
        return source.read(uint2(off / s.element, row));
    const uint pair = off / 4, k = off % 4;
    const bool luma = s.pairs == 1 ? (k & 1) == 0 : (k & 1) == 1;
    if (luma)
        return float4(source.read(uint2(2 * pair + k / 2, row)).r);
    const float4 c = chroma.read(uint2(pair, row));
    return float4(k / 2 == 0 ? c.r : c.g);
}

fragment float4 ycbcr_overlap_r8_frag(Out in [[stage_in]], StandIn source [[texture(0)]], Reach reach [[texture(1)]],
                                      StandIn chroma [[texture(2)]], constant OverlapRows &s [[buffer(0)]])
{
    return overlapTexel(source, reach, chroma, s, uint2(in.position.xy));
}

fragment float4 ycbcr_overlap_r16_frag(Out in [[stage_in]], StandIn source [[texture(0)]], Reach reach [[texture(1)]],
                                       constant OverlapRows &s [[buffer(0)]])
{
    return overlapTexel(source, reach, source, s, uint2(in.position.xy));
}

fragment float4 ycbcr_overlap_rg8_frag(Out in [[stage_in]], StandIn source [[texture(0)]], Reach reach [[texture(1)]],
                                       constant OverlapRows &s [[buffer(0)]])
{
    return overlapTexel(source, reach, source, s, uint2(in.position.xy));
}

fragment float4 ycbcr_overlap_rg16_frag(Out in [[stage_in]], StandIn source [[texture(0)]], Reach reach [[texture(1)]],
                                        constant OverlapRows &s [[buffer(0)]])
{
    return overlapTexel(source, reach, source, s, uint2(in.position.xy));
}

fragment float4 ycbcr_overlap_rgb10a2_frag(Out in [[stage_in]], StandIn source [[texture(0)]],
                                           Reach reach [[texture(1)]], constant OverlapRows &s [[buffer(0)]])
{
    return overlapTexel(source, reach, source, s, uint2(in.position.xy));
}

fragment uint ycbcr_overlap_r32ui_frag(Out in [[stage_in]], texture2d<uint, access::read> source [[texture(0)]],
                                       Reach reach [[texture(1)]], constant OverlapRows &s [[buffer(0)]])
{
    const uint2 at = uint2(in.position.xy);
    uint row = 0, off = 0;
    if (!overlapWinner(s, reach, s.base + at.y * s.pitch + at.x * s.element, row, off)) {
        discard_fragment();
        return 0;
    }
    return source.read(uint2(off / s.element, row)).r;
}

// A plain format's rows of 8 or 16 bytes a texel, read through an integer view of its pool (runtime/plain_rows.h)
static uint4 overlapWords(texture2d<uint, access::read> source, Reach reach, constant OverlapRows &s, uint2 at)
{
    uint row = 0, off = 0;
    if (!overlapWinner(s, reach, s.base + at.y * s.pitch + at.x * s.element, row, off)) {
        discard_fragment();
        return uint4(0);
    }
    return source.read(uint2(off / s.element, row));
}

fragment uint2 ycbcr_overlap_rg32ui_frag(Out in [[stage_in]], texture2d<uint, access::read> source [[texture(0)]],
                                         Reach reach [[texture(1)]], constant OverlapRows &s [[buffer(0)]])
{
    return overlapWords(source, reach, s, uint2(in.position.xy)).rg;
}

fragment uint4 ycbcr_overlap_rgba32ui_frag(Out in [[stage_in]], texture2d<uint, access::read> source [[texture(0)]],
                                           Reach reach [[texture(1)]], constant OverlapRows &s [[buffer(0)]])
{
    return overlapWords(source, reach, s, uint2(in.position.xy));
}

// Two planes whose stored rows reach one another's bytes: an RGB10A8_2P_XR10's words and alpha (runtime/xr_pools.h
// xrPlanesCross) from their R32Uint and R8Uint pools, a YCbCr target's luma and chroma (runtime/ycbcr_meet.h) from their
// unorm pools (runtime/overlap_order.h overlapPlanesWriters step for step): each byte of the planes' span from the
// element, of either plane, whose store lands last as an M4 Max stores a pass, by frameOrder (a pass that drew nothing by
// clearOrder) and then the texel further right in the frame, plane 0 at one x, ORed with every other store of its 2x2
// frame quad; a loaded pass's only where its draws reach. The target is the span's bytes `pitch` apart from `base`, a byte
// a texel.
struct MeetPlane {
    uint at, pitch, row, rows;
    uint unit, channel, across, down;
};

struct OverlapPlanes {
    uint base, pitch;
    MeetPlane plane[2];
    Tiles tiles;
    uint cleared, masked;
};

static uint meetByte(texture2d<uint, access::read> t, uint2 at, uint b, uint channel)
{
    return (t.read(at).r >> (8 * b)) & 0xffu;
}

// Byte `b` of a unorm element of `channel`-byte codes
static uint meetByte(StandIn t, uint2 at, uint b, uint channel)
{
    const float v = t.read(at)[b / channel];
    const uint code = uint(rint(v * float((1u << (8 * channel)) - 1u)));
    return (code >> (8 * (b % channel))) & 0xffu;
}

template <typename T>
static uint meetValue(constant OverlapPlanes &s, Reach reach, texture2d<T, access::read> p0,
                      texture2d<T, access::read> p1, uint a, thread bool &found)
{
    found = false;
    uint best = 0, bestKey = 0, value = 0, kept = 0;
    uint2 quad = uint2(0);
    for (uint pass = 0; pass < 2; pass++)
        for (uint k = 0; k < 2; k++) {
            constant MeetPlane &p = s.plane[k];
            if (p.rows == 0 || a < p.at)
                continue;
            const uint off = a - p.at;
            const uint last = min(off / p.pitch, p.rows - 1), first = off >= p.row ? (off - p.row) / p.pitch + 1 : 0;
            for (uint r = first; r <= last; r++) {
                const uint b = off - r * p.pitch, e = b / p.unit;
                const uint2 at = uint2(e * p.across, r * p.down);
                if (s.masked != 0 && !reached(reach, s.tiles, at))
                    continue;
                const uint2 f = frameTexel(s.tiles, at.x, at.y, p.across, p.down);
                if (pass == 1) {
                    if (all(f / 2 == quad) && kept < 4) {
                        kept++;
                        value |= k == 0 ? meetByte(p0, uint2(e, r), b % p.unit, p.channel)
                                        : meetByte(p1, uint2(e, r), b % p.unit, p.channel);
                    }
                    continue;
                }
                const uint order = s.cleared != 0 ? clearOrder(s.tiles, f) : frameOrder(s.tiles, f);
                const uint key = f.x << 1 | (k == 0 ? 1u : 0u);
                if (!found || order > best || (order == best && key > bestKey)) {
                    found = true;
                    best = order;
                    bestKey = key;
                    quad = f / 2;
                }
            }
        }
    return value;
}

fragment uint ycbcr_overlap_planes_frag(Out in [[stage_in]], texture2d<uint, access::read> words [[texture(0)]],
                                        texture2d<uint, access::read> alpha [[texture(1)]], Reach reach [[texture(2)]],
                                        constant OverlapPlanes &s [[buffer(0)]])
{
    bool found = false;
    const uint value =
        meetValue(s, reach, words, alpha, s.base + uint(in.position.y) * s.pitch + uint(in.position.x), found);
    if (!found)
        discard_fragment();
    return value;
}

fragment uint ycbcr_overlap_meet_frag(Out in [[stage_in]], StandIn luma [[texture(0)]], StandIn chroma [[texture(1)]],
                                      Reach reach [[texture(2)]], constant OverlapPlanes &s [[buffer(0)]])
{
    bool found = false;
    const uint value =
        meetValue(s, reach, luma, chroma, s.base + uint(in.position.y) * s.pitch + uint(in.position.x), found);
    if (!found)
        discard_fragment();
    return value;
}
