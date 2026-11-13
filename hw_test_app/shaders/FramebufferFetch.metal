#include <metal_stdlib>
using namespace metal;

struct FetchVertex {
    float4 position [[position]];
    uint step [[flat]];
};

vertex FetchVertex fetch_vertex(uint vid [[vertex_id]], uint instance [[instance_id]])
{
    const float2 points[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return {float4(points[vid % 3], 0, 1), vid / 3 + instance};
}

fragment float4 fetch_order_fragment(FetchVertex v [[stage_in]], float4 previous [[color(0)]],
                                     constant uint &discardOdd [[buffer(0)]])
{
    if (discardOdd && (v.step & 1))
        discard_fragment();
    const float3 next = float3((v.step * 3 + 1) % 7, (v.step * 5 + 2) % 7, (v.step + 3) % 7) / 8.0f;
    return float4(previous.rgb * 0.5f + next * 0.5f, 1);
}

// fetch_order_fragment's step, discarded over the left `rule.y` columns on odd steps when rule.x is set: whole regions of
// an interlocked draw that never reach the pixel's ticket
fragment float4 fetch_region_fragment(FetchVertex v [[stage_in]], float4 previous [[color(0)]],
                                      constant uint2 &rule [[buffer(0)]])
{
    if (rule.x && (v.step & 1) && v.position.x < float(rule.y))
        discard_fragment();
    const float3 next = float3((v.step * 3 + 1) % 7, (v.step * 5 + 2) % 7, (v.step + 3) % 7) / 8.0f;
    return float4(previous.rgb * 0.5f + next * 0.5f, 1);
}

vertex float4 fetch_depth_vertex(uint vid [[vertex_id]], constant float &depth [[buffer(0)]])
{
    const float2 points[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    return float4(points[vid % 3], depth, 1);
}

fragment float4 fetch_add_fragment(float4 previous [[color(0)]], constant float4 &value [[buffer(0)]])
{
    if (value.a < 0)
        discard_fragment();
    return float4(previous.rgb + value.rgb, 1);
}

struct FetchBlendPoint {
    float4 point [[attribute(0)]];
};

struct FetchBlendVertex {
    float4 position [[position]];
    uint tag [[flat]];
};

// point: clip x and y, the primitive's tag; instances[instance]: a clip offset and a tag the instance folds in
vertex FetchBlendVertex fetch_blend_vertex(FetchBlendPoint in [[stage_in]], uint instance [[instance_id]],
                                           constant float4 *instances [[buffer(1)]])
{
    const float4 shift = instances[instance];
    return {float4(in.point.xy + shift.xy, 0, 1), uint(in.point.z) ^ uint(shift.z)};
}

// What One/One blending adds to a pixel's state, in steps of 1/scale per channel: r counts the fragments that reached
// it (g once r is full), b and a fold the fragment's tag into the count it fetched, so a lost fragment shortens the
// count and one taken out of primitive order changes the fold. Every value is exact in unorm8 (scale 255) and half
// (scale 256).
static float4 fetch_blend_step(float4 fetched, uint tag, float scale)
{
    const uint4 n = uint4(round(fetched * scale));
    const uint mixed = (n.r + n.g) ^ tag;
    const uint full = n.r < 255 ? 0 : 1;
    return float4(uint4(1 - full, full, mixed & 1, (mixed >> 1) & 1)) / scale;
}

fragment float4 fetch_blend_fragment(FetchBlendVertex v [[stage_in]], float4 fetched [[color(0)]],
                                     constant float &scale [[buffer(0)]])
{
    return fetch_blend_step(fetched, v.tag, scale);
}

// a rectangle's two triangles share primitive_id >> 1, so the pixels on its diagonal take the same step either way
fragment float4 fetch_blend_primitive_fragment(FetchBlendVertex v [[stage_in]], uint primitive [[primitive_id]],
                                               float4 fetched [[color(0)]], constant float &scale [[buffer(0)]])
{
    return fetch_blend_step(fetched, v.tag ^ (primitive >> 1), scale);
}

struct FetchBlendSecond {
    float4 color [[color(1)]];
};

fragment FetchBlendSecond fetch_blend_second_fragment(FetchBlendVertex v [[stage_in]], float4 fetched [[color(1)]],
                                                      constant float &scale [[buffer(0)]])
{
    return {fetch_blend_step(fetched, v.tag, scale)};
}

fragment float4 fetch_blend_resolve_fragment(float4 second [[color(1)]])
{
    return second;
}
