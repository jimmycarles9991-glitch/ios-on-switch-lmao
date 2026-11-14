#include <metal_stdlib>
using namespace metal;

// The paths behind the answers above the identity (tests/NativeAnswerPathsTest.swift)

// A triangle over the whole target
static float4 answers_corner(uint vid)
{
    return float4(float(vid & 1) * 4 - 1, float(vid >> 1) * 4 - 1, 0, 1);
}

vertex float4 answers_full_vertex(uint vid [[vertex_id]])
{
    return answers_corner(vid);
}

// metal.native.wide_targets: eight RGBA32Float targets, each its own value at each texel
struct WideTargets {
    float4 c0 [[color(0)]];
    float4 c1 [[color(1)]];
    float4 c2 [[color(2)]];
    float4 c3 [[color(3)]];
    float4 c4 [[color(4)]];
    float4 c5 [[color(5)]];
    float4 c6 [[color(6)]];
    float4 c7 [[color(7)]];
};

fragment WideTargets wide_targets_fragment(float4 position [[position]])
{
    const float2 xy = floor(position.xy);
    return {float4(xy.x + 0.0f, xy.y, 0.5f, -0.0f),
            float4(xy.x + 1000.0f, xy.y, 1.5f, -1.0f),
            float4(xy.x + 2000.0f, xy.y, 2.5f, -2.0f),
            float4(xy.x + 3000.0f, xy.y, 3.5f, -3.0f),
            float4(xy.x + 4000.0f, xy.y, 4.5f, -4.0f),
            float4(xy.x + 5000.0f, xy.y, 5.5f, -5.0f),
            float4(xy.x + 6000.0f, xy.y, 6.5f, -6.0f),
            float4(xy.x + 7000.0f, xy.y, 7.5f, -7.0f)};
}

// metal.native.interpolants: as many float4 varyings as the device answers interpolated components, each read
static float4 answers_varying(uint i)
{
    return float4(float(i), float(i) + 0.25f, 0.5f * float(i), 1.0f + float(i));
}

struct Interpolants32 {
    float4 position [[position]];
    float4 v0 [[user(v0)]];
    float4 v1 [[user(v1)]];
    float4 v2 [[user(v2)]];
    float4 v3 [[user(v3)]];
    float4 v4 [[user(v4)]];
    float4 v5 [[user(v5)]];
    float4 v6 [[user(v6)]];
    float4 v7 [[user(v7)]];
    float4 v8 [[user(v8)]];
    float4 v9 [[user(v9)]];
    float4 v10 [[user(v10)]];
    float4 v11 [[user(v11)]];
    float4 v12 [[user(v12)]];
    float4 v13 [[user(v13)]];
    float4 v14 [[user(v14)]];
    float4 v15 [[user(v15)]];
    float4 v16 [[user(v16)]];
    float4 v17 [[user(v17)]];
    float4 v18 [[user(v18)]];
    float4 v19 [[user(v19)]];
    float4 v20 [[user(v20)]];
    float4 v21 [[user(v21)]];
    float4 v22 [[user(v22)]];
    float4 v23 [[user(v23)]];
    float4 v24 [[user(v24)]];
    float4 v25 [[user(v25)]];
    float4 v26 [[user(v26)]];
    float4 v27 [[user(v27)]];
    float4 v28 [[user(v28)]];
    float4 v29 [[user(v29)]];
    float4 v30 [[user(v30)]];
    float4 v31 [[user(v31)]];
};

vertex Interpolants32 interpolants32_vertex(uint vid [[vertex_id]])
{
    Interpolants32 out;
    out.position = answers_corner(vid);
    out.v0 = answers_varying(0);
    out.v1 = answers_varying(1);
    out.v2 = answers_varying(2);
    out.v3 = answers_varying(3);
    out.v4 = answers_varying(4);
    out.v5 = answers_varying(5);
    out.v6 = answers_varying(6);
    out.v7 = answers_varying(7);
    out.v8 = answers_varying(8);
    out.v9 = answers_varying(9);
    out.v10 = answers_varying(10);
    out.v11 = answers_varying(11);
    out.v12 = answers_varying(12);
    out.v13 = answers_varying(13);
    out.v14 = answers_varying(14);
    out.v15 = answers_varying(15);
    out.v16 = answers_varying(16);
    out.v17 = answers_varying(17);
    out.v18 = answers_varying(18);
    out.v19 = answers_varying(19);
    out.v20 = answers_varying(20);
    out.v21 = answers_varying(21);
    out.v22 = answers_varying(22);
    out.v23 = answers_varying(23);
    out.v24 = answers_varying(24);
    out.v25 = answers_varying(25);
    out.v26 = answers_varying(26);
    out.v27 = answers_varying(27);
    out.v28 = answers_varying(28);
    out.v29 = answers_varying(29);
    out.v30 = answers_varying(30);
    out.v31 = answers_varying(31);
    return out;
}

fragment float4 interpolants32_fragment(Interpolants32 in [[stage_in]])
{
    return in.v0 * 1.0f +
           in.v1 * 2.0f +
           in.v2 * 3.0f +
           in.v3 * 4.0f +
           in.v4 * 5.0f +
           in.v5 * 6.0f +
           in.v6 * 7.0f +
           in.v7 * 8.0f +
           in.v8 * 9.0f +
           in.v9 * 10.0f +
           in.v10 * 11.0f +
           in.v11 * 12.0f +
           in.v12 * 13.0f +
           in.v13 * 14.0f +
           in.v14 * 15.0f +
           in.v15 * 16.0f +
           in.v16 * 17.0f +
           in.v17 * 18.0f +
           in.v18 * 19.0f +
           in.v19 * 20.0f +
           in.v20 * 21.0f +
           in.v21 * 22.0f +
           in.v22 * 23.0f +
           in.v23 * 24.0f +
           in.v24 * 25.0f +
           in.v25 * 26.0f +
           in.v26 * 27.0f +
           in.v27 * 28.0f +
           in.v28 * 29.0f +
           in.v29 * 30.0f +
           in.v30 * 31.0f +
           in.v31 * 32.0f;
}

struct Interpolants31 {
    float4 position [[position]];
    float4 v0 [[user(v0)]];
    float4 v1 [[user(v1)]];
    float4 v2 [[user(v2)]];
    float4 v3 [[user(v3)]];
    float4 v4 [[user(v4)]];
    float4 v5 [[user(v5)]];
    float4 v6 [[user(v6)]];
    float4 v7 [[user(v7)]];
    float4 v8 [[user(v8)]];
    float4 v9 [[user(v9)]];
    float4 v10 [[user(v10)]];
    float4 v11 [[user(v11)]];
    float4 v12 [[user(v12)]];
    float4 v13 [[user(v13)]];
    float4 v14 [[user(v14)]];
    float4 v15 [[user(v15)]];
    float4 v16 [[user(v16)]];
    float4 v17 [[user(v17)]];
    float4 v18 [[user(v18)]];
    float4 v19 [[user(v19)]];
    float4 v20 [[user(v20)]];
    float4 v21 [[user(v21)]];
    float4 v22 [[user(v22)]];
    float4 v23 [[user(v23)]];
    float4 v24 [[user(v24)]];
    float4 v25 [[user(v25)]];
    float4 v26 [[user(v26)]];
    float4 v27 [[user(v27)]];
    float4 v28 [[user(v28)]];
    float4 v29 [[user(v29)]];
    float4 v30 [[user(v30)]];
};

vertex Interpolants31 interpolants31_vertex(uint vid [[vertex_id]])
{
    Interpolants31 out;
    out.position = answers_corner(vid);
    out.v0 = answers_varying(0);
    out.v1 = answers_varying(1);
    out.v2 = answers_varying(2);
    out.v3 = answers_varying(3);
    out.v4 = answers_varying(4);
    out.v5 = answers_varying(5);
    out.v6 = answers_varying(6);
    out.v7 = answers_varying(7);
    out.v8 = answers_varying(8);
    out.v9 = answers_varying(9);
    out.v10 = answers_varying(10);
    out.v11 = answers_varying(11);
    out.v12 = answers_varying(12);
    out.v13 = answers_varying(13);
    out.v14 = answers_varying(14);
    out.v15 = answers_varying(15);
    out.v16 = answers_varying(16);
    out.v17 = answers_varying(17);
    out.v18 = answers_varying(18);
    out.v19 = answers_varying(19);
    out.v20 = answers_varying(20);
    out.v21 = answers_varying(21);
    out.v22 = answers_varying(22);
    out.v23 = answers_varying(23);
    out.v24 = answers_varying(24);
    out.v25 = answers_varying(25);
    out.v26 = answers_varying(26);
    out.v27 = answers_varying(27);
    out.v28 = answers_varying(28);
    out.v29 = answers_varying(29);
    out.v30 = answers_varying(30);
    return out;
}

fragment float4 interpolants31_fragment(Interpolants31 in [[stage_in]])
{
    return in.v0 * 1.0f +
           in.v1 * 2.0f +
           in.v2 * 3.0f +
           in.v3 * 4.0f +
           in.v4 * 5.0f +
           in.v5 * 6.0f +
           in.v6 * 7.0f +
           in.v7 * 8.0f +
           in.v8 * 9.0f +
           in.v9 * 10.0f +
           in.v10 * 11.0f +
           in.v11 * 12.0f +
           in.v12 * 13.0f +
           in.v13 * 14.0f +
           in.v14 * 15.0f +
           in.v15 * 16.0f +
           in.v16 * 17.0f +
           in.v17 * 18.0f +
           in.v18 * 19.0f +
           in.v19 * 20.0f +
           in.v20 * 21.0f +
           in.v21 * 22.0f +
           in.v22 * 23.0f +
           in.v23 * 24.0f +
           in.v24 * 25.0f +
           in.v25 * 26.0f +
           in.v26 * 27.0f +
           in.v27 * 28.0f +
           in.v28 * 29.0f +
           in.v29 * 30.0f +
           in.v30 * 31.0f;
}

// metal.native.threadgroup_memory: every word of `words` written by one thread and read by the next after the barrier
kernel void threadgroup_span(threadgroup uint *memory [[threadgroup(0)]], device uint *out [[buffer(0)]],
                             constant uint &words [[buffer(1)]], uint tid [[thread_index_in_threadgroup]],
                             uint threads [[threads_per_threadgroup]])
{
    for (uint i = tid; i < words; i += threads)
        memory[i] = i * 2654435761u;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint x = 0;
    for (uint i = (tid + 1) % threads; i < words; i += threads)
        x ^= memory[i] + i;
    out[tid] = x;
}
