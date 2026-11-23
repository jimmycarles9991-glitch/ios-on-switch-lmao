#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.ended_encoder: quad q of the strip (vertex q * 6 on) covers kEndedRects[q] (left, right, whole, top), flat
/// coloured kEndedColours[q]
struct EndedVertex {
    float4 p [[position]];
    uint quad [[flat]];
};

constant float4 kEndedRects[4] = {float4(-1, -1, 0, 1), float4(0, -1, 1, 1), float4(-1, -1, 1, 1), float4(-1, 0, 1, 1)};
constant float4 kEndedColours[4] = {float4(1, 0, 0, 1), float4(0, 1, 0, 1), float4(0, 0, 1, 1), float4(1, 1, 1, 1)};

vertex EndedVertex ended_quad(uint vid [[vertex_id]])
{
    const uint q = (vid / 6) & 3, c = vid % 6;
    const float4 r = kEndedRects[q];
    const float2 k[6] = {float2(r.x, r.y), float2(r.z, r.y), float2(r.x, r.w),
                         float2(r.z, r.y), float2(r.z, r.w), float2(r.x, r.w)};
    return {float4(k[c], 0, 1), q};
}

fragment float4 ended_flat(EndedVertex in [[stage_in]]) { return kEndedColours[in.quad]; }

fragment float4 ended_dark(EndedVertex in [[stage_in]]) { return kEndedColours[in.quad] * float4(0.5, 0.5, 0.5, 1); }

fragment float4 ended_buffer(EndedVertex in [[stage_in]], constant float4 &c [[buffer(0)]]) { return c; }

kernel void ended_set(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = 3; }

kernel void ended_double(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = o[i] * 2; }

kernel void ended_increment(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = o[i] + 1; }
