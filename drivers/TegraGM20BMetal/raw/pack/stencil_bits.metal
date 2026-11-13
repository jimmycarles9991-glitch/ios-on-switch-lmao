// SPDX-License-Identifier: MIT
// raw GM20B path: a stencil plane written by the fixed-function stencil (raw/runtime/raw_stencil_bits.h). The target's stencil
// is cleared to 0 (or zeroed by a draw of bit 0 where only a rectangle of the source is read), then a full-target quad is drawn
// once a bit, the ROP writing 0xff through write mask `bit`; each function
// discards where `bit` of its source stencil is clear. Target texel (x, y) reads source texel (ax x + ay y + cx, bx x + by y +
// cy); a multisampled source's stencil is that of sample `sample`, or of its sample of least or greatest depth, the first
// such in sample order. The stencil is read through the source's stencil header, the depth as its float's bits through the
// source's own (one texture class for both: the probe binds one). One function a target format and sample: the pack keys an
// own state by its function's name. Built without fast math (rawpack.py), so the depth comparisons keep IEEE order.
#include <metal_stdlib>
using namespace metal;

struct Bits {
    int ax, ay, cx, bx, by, cy;
    int x0, y0, x1, y1;
    uint bit, samples, sample;
};

struct Out {
    float4 position [[position]];
};

vertex Out stencil_bits_vert(uint vid [[vertex_id]])
{
    Out o;
    o.position = float4((vid & 1) ? 1.0 : -1.0, (vid & 2) ? 1.0 : -1.0, 0.0, 1.0);
    return o;
}

typedef texture2d<uint, access::read> Texels;
typedef texture2d_ms<uint, access::read> Samples;

static uint2 source(float4 position, constant Bits &b)
{
    const int2 d = int2(position.xy);
    const int2 s = int2(b.ax * d.x + b.ay * d.y + b.cx, b.bx * d.x + b.by * d.y + b.cy);
    if (s.x < b.x0 || s.y < b.y0 || s.x >= b.x1 || s.y >= b.y1)
        discard_fragment();
    return uint2(s);
}

// bit 0: the draw that zeroes the target inside the source rectangle ahead of the bits
static void keep(uint stencil, constant Bits &b)
{
    if (b.bit && !(stencil & b.bit))
        discard_fragment();
}

static uint picked(Samples depth, uint2 at, uint n, bool greatest)
{
    uint pick = 0;
    float best = as_type<float>(depth.read(at, 0).r);
    for (uint k = 1; k < n; k++) {
        const float d = as_type<float>(depth.read(at, k).r);
        if (greatest ? d > best : d < best) {
            best = d;
            pick = k;
        }
    }
    return pick;
}

fragment void stencil_bits_s8_frag(Out in [[stage_in]], Texels s [[texture(0)]], constant Bits &b [[buffer(0)]])
{
    keep(s.read(source(in.position, b)).r, b);
}

fragment void stencil_bits_d32fs8_frag(Out in [[stage_in]], Texels s [[texture(0)]], constant Bits &b [[buffer(0)]])
{
    keep(s.read(source(in.position, b)).r, b);
}

fragment void stencil_bits_ms_s8_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant Bits &b [[buffer(0)]])
{
    keep(s.read(source(in.position, b), b.sample).r, b);
}

fragment void stencil_bits_ms_d32fs8_frag(Out in [[stage_in]], Samples s [[texture(0)]], constant Bits &b [[buffer(0)]])
{
    keep(s.read(source(in.position, b), b.sample).r, b);
}

fragment void stencil_bits_ms_d32fs8_min_frag(Out in [[stage_in]], Samples z [[texture(0)]], Samples s [[texture(1)]],
                                              constant Bits &b [[buffer(0)]])
{
    const uint2 at = source(in.position, b);
    keep(s.read(at, picked(z, at, b.samples, false)).r, b);
}

fragment void stencil_bits_ms_d32fs8_max_frag(Out in [[stage_in]], Samples z [[texture(0)]], Samples s [[texture(1)]],
                                              constant Bits &b [[buffer(0)]])
{
    const uint2 at = source(in.position, b);
    keep(s.read(at, picked(z, at, b.samples, true)).r, b);
}

// The one-draw route (raw/runtime/raw_zeta_colour.h): the target is the zeta's memory as a colour image of its texel, RG32Uint
// over Depth32Float_Stencil8 (R its depth's bits, G its stencil) and R8Uint over Stencil8. From one sample the whole texel is
// written; a multisampled Depth32Float_Stencil8 state writes G alone (its target's write mask), the depth word kept.
fragment uint4 stencil_bits_s8_colour_frag(Out in [[stage_in]], Texels s [[texture(0)]], constant Bits &b [[buffer(0)]])
{
    return uint4(s.read(source(in.position, b)).r, 0, 0, 0);
}

fragment uint4 stencil_bits_d32fs8_colour_frag(Out in [[stage_in]], Texels z [[texture(0)]], Texels s [[texture(1)]],
                                               constant Bits &b [[buffer(0)]])
{
    const uint2 at = source(in.position, b);
    return uint4(z.read(at).r, s.read(at).r, 0, 0);
}

fragment uint4 stencil_bits_ms_s8_colour_frag(Out in [[stage_in]], Samples s [[texture(0)]],
                                              constant Bits &b [[buffer(0)]])
{
    return uint4(s.read(source(in.position, b), b.sample).r, 0, 0, 0);
}

fragment uint4 stencil_bits_ms_d32fs8_colour_frag(Out in [[stage_in]], Samples s [[texture(0)]],
                                                  constant Bits &b [[buffer(0)]])
{
    return uint4(0, s.read(source(in.position, b), b.sample).r, 0, 0);
}

fragment uint4 stencil_bits_ms_d32fs8_min_colour_frag(Out in [[stage_in]], Samples z [[texture(0)]],
                                                      Samples s [[texture(1)]], constant Bits &b [[buffer(0)]])
{
    const uint2 at = source(in.position, b);
    return uint4(0, s.read(at, picked(z, at, b.samples, false)).r, 0, 0);
}

fragment uint4 stencil_bits_ms_d32fs8_max_colour_frag(Out in [[stage_in]], Samples z [[texture(0)]],
                                                      Samples s [[texture(1)]], constant Bits &b [[buffer(0)]])
{
    const uint2 at = source(in.position, b);
    return uint4(0, s.read(at, picked(z, at, b.samples, true)).r, 0, 0);
}
