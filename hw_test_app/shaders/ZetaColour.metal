#include <metal_stdlib>
using namespace metal;

/// gpu.raw.zeta_colour: a zeta's pattern, every stencil value and a depth of its own a texel (ZetaColourCells.pattern)
struct ZetaPattern {
    uint seed, bit;
};

struct ZetaIn {
    float4 position [[position]];
};

static uint zeta_stencil(float4 position, uint seed)
{
    const uint2 p = uint2(position.xy);
    return (p.x * 7u + p.y * 13u + (p.x >> 3) * 5u + seed) & 255u;
}

static float zeta_depth(float4 position, uint seed)
{
    const uint2 p = uint2(position.xy);
    return float((p.x * 37u + p.y * 101u + seed * 17u) % 4093u + 1u) / 4096.0f;
}

struct ZetaDepth {
    float depth [[depth(any)]];
};

/// The pattern's depth
fragment ZetaDepth zeta_depth_fragment(ZetaIn in [[stage_in]], constant ZetaPattern &p [[buffer(0)]])
{
    return {zeta_depth(in.position, p.seed)};
}

/// Reaches the texels whose pattern stencil has `bit` set
fragment void zeta_bit_fragment(ZetaIn in [[stage_in]], constant ZetaPattern &p [[buffer(0)]])
{
    if (!(zeta_stencil(in.position, p.seed) & p.bit))
        discard_fragment();
}

struct ZetaDepthMark {
    uint4 mark [[color(0)]];
    float depth [[depth(any)]];
};

/// 1 where the depth test passes against the pattern's depth
fragment ZetaDepthMark zeta_depth_mark_fragment(ZetaIn in [[stage_in]], constant ZetaPattern &p [[buffer(0)]])
{
    return {uint4(1, 0, 0, 0), zeta_depth(in.position, p.seed)};
}
