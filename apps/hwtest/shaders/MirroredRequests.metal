#include <metal_stdlib>
using namespace metal;

struct MirroredVertex {
    float4 position [[position]];
};

// Point i of a 4x4 grid over the target, 1.5 clip units apart: a draw of indices (a, b, c) covers their triangle, so
// which indices an indexed draw read shows in the pixels it covered
vertex MirroredVertex mirrored_grid_vertex(uint i [[vertex_id]])
{
    return {float4(float(i % 4) / 1.5f - 1.0f, float(i / 4 % 4) / 1.5f - 1.0f, 0, 1)};
}

fragment float4 mirrored_red_fragment()
{
    return float4(1, 0, 0, 1);
}

// A triangle over the whole target
vertex MirroredVertex mirrored_cover_vertex(uint i [[vertex_id]])
{
    return {float4(i == 1 ? 3.0f : -1.0f, i == 2 ? 3.0f : -1.0f, 0, 1)};
}

// Vertices 3k to 3k + 2: a triangle inside quadrant k of the target (column k % 2, row k / 2 from the top), so the
// quadrants drawn name the commands of an indirect command buffer that ran
vertex MirroredVertex mirrored_quadrant_vertex(uint i [[vertex_id]])
{
    const uint k = i / 3, corner = i % 3;
    const float x = -1.0f + float(k % 2) + (corner == 1 ? 0.9f : 0.1f);
    const float y = -float(k / 2) + (corner == 2 ? 0.9f : 0.1f);
    return {float4(x, y, 0, 1)};
}

// Sample s of pixel (x, y) holds (x, y, s, 0xa0) in RGBA8Unorm codes (shaded per sample)
fragment float4 mirrored_sample_codes(MirroredVertex in [[stage_in]], uint sample [[sample_id]])
{
    return float4(floor(in.position.x), floor(in.position.y), float(sample), 160.0f) / 255.0f;
}
