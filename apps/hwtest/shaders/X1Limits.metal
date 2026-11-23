#include <metal_stdlib>
using namespace metal;

struct LimitsVertex {
    float4 position [[position]];
    float2 offset;
};

// A triangle over all of clip space; `offset` is the viewport-space distance from the viewport's centre, y down
vertex LimitsVertex limits_viewport_vertex(uint i [[vertex_id]], constant float2 &halfSize [[buffer(0)]])
{
    const float2 clip = float2(i == 1 ? 3.0f : -1.0f, i == 2 ? 3.0f : -1.0f);
    return {float4(clip, 0, 1), float2(clip.x, -clip.y) * halfSize};
}

// RGBA8 codes 128 + floor(offset)
fragment float4 limits_viewport_fragment(LimitsVertex in [[stage_in]])
{
    return float4((128.0f + floor(in.offset)) / 255.0f, 0, 1);
}

// Each thread writes its place in a 32-wide grid at its flat index in the threadgroup
kernel void limits_threads_kernel(device uint *out [[buffer(0)]], uint2 tid [[thread_position_in_threadgroup]],
                                  uint2 size [[threads_per_threadgroup]])
{
    out[tid.x + tid.y * size.x] = tid.x + 32 * tid.y;
}

// Texel (x, y) of face `at.z` of a cube
kernel void limits_cube_kernel(texturecube<float, access::read> cube [[texture(0)]], device float *out [[buffer(0)]],
                               constant uint3 &at [[buffer(1)]])
{
    out[0] = cube.read(at.xy, at.z).r;
}

// Texel (x, y) of layer `at.z` of a 2D array
kernel void limits_array_kernel(texture2d_array<float, access::read> array [[texture(0)]], device float *out [[buffer(0)]],
                                constant uint3 &at [[buffer(1)]])
{
    out[0] = array.read(at.xy, at.z).r;
}

// The execution range of an indirect command buffer: commands `range.x` to `range.x + range.y - 1`
kernel void limits_range_kernel(device uint2 *out [[buffer(0)]], constant uint2 &range [[buffer(1)]])
{
    out[0] = range;
}

struct ColumnVertex {
    float4 position [[position]];
    uint column [[flat]];
};

// Command k's six vertices (vertexStart 6k) cover column k of eight
vertex ColumnVertex limits_column_vertex(uint i [[vertex_id]])
{
    const uint column = i / 6, corner = i % 6;
    const float x0 = -1.0f + 0.25f * float(column), x1 = x0 + 0.25f;
    const bool right = corner == 1 || corner == 4 || corner == 5, top = corner == 2 || corner == 3 || corner == 5;
    return {float4(right ? x1 : x0, top ? 1.0f : -1.0f, 0, 1), column};
}

fragment float4 limits_column_fragment(ColumnVertex in [[stage_in]])
{
    return float4(float(30 * (in.column + 1)) / 255.0f, 0, 0, 1);
}
