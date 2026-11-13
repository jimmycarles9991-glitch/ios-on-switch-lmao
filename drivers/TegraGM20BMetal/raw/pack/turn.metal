// SPDX-License-Identifier: MIT
// raw GM20B path: a texel copy between two images of one texel size, each destination texel (x, y) the source's texel at
// (ax x + ay y + cx, bx x + by y + cy), kept where that falls outside the source rectangle [x0, x1) x [y0, y1): a turned
// plane's texels moved between its frame and its plane (raw/runtime/raw_turn_copy.h). Colour images are viewed as unsigned
// integers of the texel's size, so every bit is moved as it is; a depth image's depth is read and written as the float it
// holds, which is its code (a stencil plane is moved by stencil_bits.metal). One function a texel size or depth format: the
// pack keys an own state by its function's name.
#include <metal_stdlib>
using namespace metal;

struct Map {
    int ax, ay, cx, bx, by, cy;
    int x0, y0, x1, y1;
};

struct Out {
    float4 position [[position]];
};

vertex Out turn_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

static uint2 source(float4 position, constant Map &m)
{
    const int2 d = int2(position.xy);
    const int2 s = int2(m.ax * d.x + m.ay * d.y + m.cx, m.bx * d.x + m.by * d.y + m.cy);
    if (s.x < m.x0 || s.y < m.y0 || s.x >= m.x1 || s.y >= m.y1)
        discard_fragment();
    return uint2(s);
}

fragment uint4 turn_copy8_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                               constant Map &m [[buffer(0)]])
{
    return s.read(source(in.position, m));
}

fragment uint4 turn_copy16_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                constant Map &m [[buffer(0)]])
{
    return s.read(source(in.position, m));
}

fragment uint4 turn_copy32_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                constant Map &m [[buffer(0)]])
{
    return s.read(source(in.position, m));
}

fragment uint4 turn_copy64_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                constant Map &m [[buffer(0)]])
{
    return s.read(source(in.position, m));
}

fragment uint4 turn_copy128_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                 constant Map &m [[buffer(0)]])
{
    return s.read(source(in.position, m));
}

struct Depth {
    float depth [[depth(any)]];
};

fragment Depth turn_depth16_frag(Out in [[stage_in]], depth2d<float, access::read> s [[texture(0)]],
                                 constant Map &m [[buffer(0)]])
{
    return {s.read(source(in.position, m))};
}

fragment Depth turn_depth32f_frag(Out in [[stage_in]], depth2d<float, access::read> s [[texture(0)]],
                                  constant Map &m [[buffer(0)]])
{
    return {s.read(source(in.position, m))};
}

fragment Depth turn_depth32fs8_frag(Out in [[stage_in]], depth2d<float, access::read> s [[texture(0)]],
                                    constant Map &m [[buffer(0)]])
{
    return {s.read(source(in.position, m))};
}

// A masked turn copy: a destination texel whose frame tile, (ax x + ay y + cx, bx x + by y + cy) / 32, no draw reaches (0 in the
// mask, raw/runtime/raw_reach.h), or that lies in no tile of it, is kept
struct Tiles {
    int ax, ay, cx, bx, by, cy;
    int tilesX, tilesY;
};

struct Masked {
    Map map;
    Tiles tiles;
};

static uint2 reached(float4 position, constant Masked &m, texture2d<uint, access::read> mask)
{
    const int2 d = int2(position.xy);
    const constant Tiles &t = m.tiles;
    const int2 f = int2(t.ax * d.x + t.ay * d.y + t.cx, t.bx * d.x + t.by * d.y + t.cy);
    if (f.x < 0 || f.y < 0)
        discard_fragment();
    const int2 tile = f / 32;
    if (tile.x >= t.tilesX || tile.y >= t.tilesY || mask.read(uint2(tile)).x == 0)
        discard_fragment();
    return source(position, m.map);
}

fragment uint4 turn_masked8_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                 texture2d<uint, access::read> mask [[texture(1)]], constant Masked &m [[buffer(0)]])
{
    return s.read(reached(in.position, m, mask));
}

fragment uint4 turn_masked16_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                  texture2d<uint, access::read> mask [[texture(1)]], constant Masked &m [[buffer(0)]])
{
    return s.read(reached(in.position, m, mask));
}

fragment uint4 turn_masked32_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                  texture2d<uint, access::read> mask [[texture(1)]], constant Masked &m [[buffer(0)]])
{
    return s.read(reached(in.position, m, mask));
}

fragment uint4 turn_masked64_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                  texture2d<uint, access::read> mask [[texture(1)]], constant Masked &m [[buffer(0)]])
{
    return s.read(reached(in.position, m, mask));
}

fragment uint4 turn_masked128_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                   texture2d<uint, access::read> mask [[texture(1)]], constant Masked &m [[buffer(0)]])
{
    return s.read(reached(in.position, m, mask));
}

// The reach pass's pixel program (raw/runtime/raw_reach.h): it reads no input, so any draw's vertex program feeds it
fragment uint4 turn_reach_frag()
{
    return uint4(1);
}

struct Image {
    uint width, height, perTile;
};

// A reach image of `perTile` x `perTile` texels a tile reduced to its tiles: 1 where a texel of the tile is
fragment uint4 turn_reach_reduce_frag(Out in [[stage_in]], texture2d<uint, access::read> s [[texture(0)]],
                                      constant Image &f [[buffer(0)]])
{
    const uint2 first = uint2(in.position.xy) * f.perTile, end = min(first + f.perTile, uint2(f.width, f.height));
    for (uint y = first.y; y < end.y; y++)
        for (uint x = first.x; x < end.x; x++)
            if (s.read(uint2(x, y)).x)
                return uint4(1);
    discard_fragment();
    return uint4(0);
}
