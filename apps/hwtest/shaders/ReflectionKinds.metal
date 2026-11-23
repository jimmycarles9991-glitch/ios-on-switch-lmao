#include <metal_stdlib>
using namespace metal;

// gpu.gm20b.reflection_kinds: one binding of every kind, compute and render, as Metal's reflection names them
struct ReflectionKindsS { float4 v; float a[4]; };

kernel void reflection_kinds(device float4 *b4 [[buffer(0)]], device ReflectionKindsS *bs [[buffer(1)]],
                             constant float &cf [[buffer(2)]], device uint *bu [[buffer(3)]],
                             device half *unused [[buffer(4)]], texture2d<float> t [[texture(0)]],
                             array<texture2d<float>, 4> ta [[texture(1)]], texture2d_array<float> tarr [[texture(5)]],
                             depth2d<float> d [[texture(6)]], array<depth2d<float>, 3> tc [[texture(7)]],
                             texture2d<half, access::write> w [[texture(10)]], sampler s [[sampler(0)]],
                             array<sampler, 3> sa [[sampler(1)]], array<sampler, 2> sc [[sampler(4)]],
                             threadgroup float *tg [[threadgroup(0)]], threadgroup float4 *tg4 [[threadgroup(1)]],
                             uint id [[thread_position_in_grid]]) {
    float x = b4[id].x + bs[id].a[1] + cf + float(bu[id]) + t.sample(s, float2(0)).x + ta[3].sample(sa[2], float2(0)).x
            + tarr.sample(s, float2(0), 1).x + d.sample(sc[1], float2(0)) + tc[2].sample(sc[0], float2(0));
    tg[id] = x;
    tg4[id] = float4(x);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    w.write(half4(tg4[0]), uint2(id, 0));
    b4[id] = float4(tg[0]);
}

struct ReflectionKindsV { float4 p [[position]]; float2 uv; };

vertex ReflectionKindsV reflection_vs(device const float4 *pos [[buffer(0)]], constant ReflectionKindsS &u [[buffer(1)]],
                                      texture2d<float> vt [[texture(0)]], uint vid [[vertex_id]]) {
    ReflectionKindsV o;
    o.p = pos[vid] + u.v + vt.read(uint2(0));
    o.uv = float2(0);
    return o;
}

fragment float4 reflection_fs(ReflectionKindsV in [[stage_in]], texture2d<float> ft [[texture(0)]],
                              array<texture2d<float>, 2> fa [[texture(1)]], sampler fsm [[sampler(0)]],
                              array<sampler, 2> fsa [[sampler(1)]], constant float4 &tint [[buffer(0)]]) {
    return ft.sample(fsm, in.uv) + fa[1].sample(fsa[1], in.uv) * tint;
}
