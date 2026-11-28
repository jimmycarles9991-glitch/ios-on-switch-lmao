#include <metal_stdlib>
using namespace metal;

// gpu.perf.rates (gpu/GPURates.swift): one unit at a time, each kernel or draw bound by the unit it names.

// Eight independent FMA chains a thread (two float4), so issue, not latency, bounds it.
kernel void rates_fma(device float4 *data [[buffer(0)]], constant uint &iterations [[buffer(1)]],
                      uint id [[thread_position_in_grid]])
{
    float4 x = data[id], y = x + 0.5f;
    for (uint i = 0; i < iterations; i++) {
        x = fma(x, float4(0.999f), float4(0.001f));
        y = fma(y, float4(0.999f), float4(0.002f));
    }
    data[id] = x + y;
}

// Every float4 of `data` read once. shape.y 0: element e is e (lanes side by side). shape.y P: element e is at page
// e % P, slot e / P, so the 32 lanes of a warp read 32 pages 4 KiB apart, one 128 KiB big page between them.
kernel void rates_read(device const float4 *data [[buffer(0)]], device float4 *out [[buffer(1)]],
                       constant uint2 &shape [[buffer(2)]], uint id [[thread_position_in_grid]],
                       uint threads [[threads_per_grid]])
{
    float4 sum = float4(0);
    for (uint e = id; e < shape.x; e += threads)
        sum += data[shape.y ? (e % shape.y) * 256u + e / shape.y : e];
    out[id] = sum;
}

struct RatesVertex {
    float4 position [[position]];
};

// One triangle over one pixel's centre, triangle t over pixel t of a grid.x x grid.y target, row by row; grid.z 1
// moves it past the right edge, where the clipper drops it.
vertex RatesVertex rates_triangle_vertex(uint vid [[vertex_id]], constant uint4 &grid [[buffer(0)]])
{
    const uint t = vid / 3u, corner = vid % 3u, pixel = t % (grid.x * grid.y);
    const float2 at = float2(pixel % grid.x, pixel / grid.x) + float2(corner == 1u ? 1.5f : 0, corner == 2u ? 1.5f : 0);
    float2 ndc = float2(at.x / float(grid.x) * 2 - 1, 1 - at.y / float(grid.y) * 2);
    ndc.x += grid.z ? 4.0f : 0.0f;
    return {float4(ndc, 0.5f, 1)};
}

fragment float4 rates_heavy_fragment(float4 at [[position]], constant uint &iterations [[buffer(0)]])
{
    float4 v = float4(at.xy * 0.001f, 0.25f, 0.5f);
    for (uint i = 0; i < iterations; i++)
        v = fma(v, float4(0.999f), float4(0.001f));
    return v;
}

// Eight bilinear samples a pixel of a 256x256 texture that the screen tiles, so neighbours share cache lines.
fragment float4 rates_texel_fragment(float4 at [[position]], texture2d<float> image [[texture(0)]])
{
    constexpr sampler bilinear(filter::linear, address::repeat);
    const float2 uv = at.xy / 256.0f;
    float4 sum = float4(0);
    for (int i = 0; i < 8; i++)
        sum += image.sample(bilinear, uv + float2(i & 3, i >> 2) * (0.5f / 256.0f));
    return sum * 0.125f;
}

// Every thread adds one to the same counter: the L2 resolves it when the buffer is cached, DRAM when it is not.
kernel void rates_atomic(device atomic_uint *counter [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
}

// rates_triangle_vertex's triangles with their corners read from a buffer, three float4 a triangle.
vertex RatesVertex rates_fetch_vertex(uint vid [[vertex_id]], device const float4 *corners [[buffer(0)]])
{
    return {corners[vid]};
}

// Sixteen reads a pixel of a 4 KiB table, chosen by the pixel's position.
fragment float4 rates_table_fragment(float4 at [[position]], device const float4 *table [[buffer(0)]])
{
    uint h = uint(at.x) * 73856093u ^ uint(at.y) * 19349663u;
    float4 sum = float4(0);
    for (uint i = 0; i < 16u; i++) {
        h = h * 1664525u + 1013904223u;
        sum += table[h >> 24];
    }
    return sum * (1.0f / 16.0f);
}

// gpu.perf.knobs: rates_heavy_fragment's work into four colour targets, and with a depth the program writes.
struct RatesFour {
    float4 a [[color(0)]];
    float4 b [[color(1)]];
    float4 c [[color(2)]];
    float4 d [[color(3)]];
};

fragment RatesFour rates_heavy_mrt_fragment(float4 at [[position]], constant uint &iterations [[buffer(0)]])
{
    float4 v = float4(at.xy * 0.001f, 0.25f, 0.5f);
    for (uint i = 0; i < iterations; i++)
        v = fma(v, float4(0.999f), float4(0.001f));
    return {v, v.yzwx, v.zwxy, v.wxyz};
}

struct RatesDepthWritten {
    float4 colour [[color(0)]];
    float depth [[depth(any)]];
};

fragment RatesDepthWritten rates_heavy_depth_fragment(float4 at [[position]], constant uint &iterations [[buffer(0)]])
{
    float4 v = float4(at.xy * 0.001f, 0.25f, 0.5f);
    for (uint i = 0; i < iterations; i++)
        v = fma(v, float4(0.999f), float4(0.001f));
    return {v, fract(v.x)};
}

// One bilinear sample a pixel at `scale` texture widths a pixel: 1/2048 magnifies nothing, larger steps minify.
fragment float4 rates_sample_fragment(float4 at [[position]], texture2d<float> image [[texture(0)]],
                                      constant float &scale [[buffer(0)]])
{
    constexpr sampler bilinear(filter::linear, address::repeat);
    return image.sample(bilinear, at.xy * scale);
}

// Eight point reads a pixel at scattered texels of a 2048x2048 texture.
fragment float4 rates_scatter_fragment(float4 at [[position]], texture2d<float, access::read> image [[texture(0)]])
{
    uint h = uint(at.x) * 73856093u ^ uint(at.y) * 19349663u;
    float4 sum = float4(0);
    for (uint i = 0; i < 8u; i++) {
        h = h * 1664525u + 1013904223u;
        sum += image.read(uint2(h & 2047u, (h >> 11) & 2047u));
    }
    return sum * 0.125f;
}

// gpu.perf.nvn: a program that writes the depth its buffer gives, whatever its triangle's, in a constant colour.
fragment RatesDepthWritten rates_set_depth_fragment(constant float4 &colourDepth [[buffer(0)]])
{
    return {float4(colourDepth.rgb, 1), colourDepth.w};
}

// The texel under the pixel, for a pass that reads the one before it.
fragment float4 rates_copy_fragment(float4 at [[position]], texture2d<float, access::read> image [[texture(0)]])
{
    return image.read(uint2(at.xy));
}
