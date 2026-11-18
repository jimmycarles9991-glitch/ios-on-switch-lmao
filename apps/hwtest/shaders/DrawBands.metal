#include <metal_stdlib>
using namespace metal;

// gpu.sched.long_draw_bands: a full-screen draw whose fragment work alone lasts over ten milliseconds on GM20B, straight-line
// so its static length is what it executes, and whose result depends on every pixel's position, so a pixel drawn twice
// or left out by its bands shows.
#define BANDS_STEP(v) v = fract(fma(v, float4(1.618034f, 1.414214f, 1.732051f, 2.236068f), float4(0.1f, 0.2f, 0.3f, 0.4f)));
#define BANDS_4(v) BANDS_STEP(v) BANDS_STEP(v) BANDS_STEP(v) BANDS_STEP(v)
#define BANDS_16(v) BANDS_4(v) BANDS_4(v) BANDS_4(v) BANDS_4(v)
#define BANDS_64(v) BANDS_16(v) BANDS_16(v) BANDS_16(v) BANDS_16(v)

struct BandsVertex {
    float4 position [[position]];
};

vertex BandsVertex bands_vertex(uint vid [[vertex_id]])
{
    const float2 corner = float2(vid == 1 ? 3.0f : -1.0f, vid == 2 ? 3.0f : -1.0f);
    return {float4(corner, 0, 1)};
}

static float4 bands_work(float2 at)
{
    float4 v = float4(at.x * 0.0013f, at.y * 0.0017f, (at.x + at.y) * 0.0007f, 0.5f);
    BANDS_64(v) BANDS_64(v) BANDS_64(v) BANDS_64(v)
    return v;
}

fragment float4 bands_blend_fragment(BandsVertex v [[stage_in]])
{
    return float4(bands_work(v.position.xy).rgb, 0.5f);
}

fragment float4 bands_fetch_fragment(BandsVertex v [[stage_in]], float4 fetched [[color(0)]])
{
    return float4(fract(fetched.rgb + bands_work(v.position.xy).rgb), 1.0f);
}

// The learned arm's draw: a loop whose length is a fragment argument, so the program's code is a few dozen instructions,
// each pass a dependent read at a scattered texel of a 32 MB texture: priced far under a band by its instructions, many
// milliseconds of memory traffic on the GPU.
fragment float4 bands_memory_fragment(BandsVertex v [[stage_in]], texture2d<float, access::read> big [[texture(0)]],
                                      constant uint &reads [[buffer(0)]])
{
    const uint2 at = uint2(v.position.xy);
    uint h = at.x * 73856093u ^ at.y * 19349663u;
    float4 sum = float4(0);
    for (uint i = 0; i < reads; i++) {
        h = h * 1664525u + 1013904223u + uint(sum.x * 7.0f);
        sum += big.read(uint2(h & 2047u, (h >> 11) & 2047u));
    }
    return float4(fract(sum.rgb), 1.0f);
}

// gpu.sched.long_dispatch_pieces: a loop whose length is an argument (a few dozen instructions of code, milliseconds of
// work), each thread writing what its grid builtins say and a value of its position, so a threadgroup a piece launched
// twice, never, or at another base shows.
kernel void pieces_kernel(device uint4 *out [[buffer(0)]], constant uint &steps [[buffer(1)]],
                          uint gid [[thread_position_in_grid]], uint group [[threadgroup_position_in_grid]],
                          uint groups [[threadgroups_per_grid]], uint threads [[threads_per_grid]])
{
    float v = float(gid & 1023u) * 0.000977f;
    for (uint i = 0; i < steps; i++)
        v = fract(fma(v, 1.618034f, 0.1f));
    out[gid] = uint4(group, groups, threads, as_type<uint>(v));
}
