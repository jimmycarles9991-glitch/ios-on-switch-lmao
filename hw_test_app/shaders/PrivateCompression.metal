#include <metal_stdlib>
using namespace metal;

// tests/PrivateCompressionTest.swift's chain: a hashed pattern, then each level the bilinear 2:1 reduction of the one
// above (the glass pyramid's shape)
static float4 comp_pattern(uint2 p)
{
    uint h = (p.x * 73856093u) ^ (p.y * 19349663u);
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    const bool flat = ((p.x >> 5) + (p.y >> 5)) & 1;
    return flat ? float4(0.25f, 0.5f, 0.75f, 1.0f) : float4(float3(uint3(h, h >> 8, h >> 16) & 255u), 255.0f) / 255.0f;
}

fragment float4 comp_pattern_fragment(float4 position [[position]])
{
    return comp_pattern(uint2(position.xy));
}

// The same pattern written by the surface unit (gpu.raw.private_compression shader_write)
kernel void comp_pattern_kernel(texture2d<float, access::write> out [[texture(0)]],
                                uint2 id [[thread_position_in_grid]])
{
    out.write(comp_pattern(id), id);
}

fragment float4 comp_reduce_fragment(float4 position [[position]], texture2d<float> above [[texture(0)]])
{
    constexpr sampler linear(filter::linear, address::clamp_to_edge);
    return above.sample(linear, position.xy / float2(above.get_width() / 2, above.get_height() / 2));
}

// tests/StoredDepthTest.swift: the texture unit's reading of a depth texture, in red
fragment float4 stored_depth_read_fragment(float4 position [[position]], depth2d<float> depth [[texture(0)]])
{
    return float4(depth.read(uint2(position.xy)), 0, 0, 1);
}

// tests/SharedLayoutTest.swift: the texel under the fragment, as read
fragment float4 shared_read_fragment(float4 position [[position]], texture2d<float, access::read> source [[texture(0)]])
{
    return source.read(uint2(position.xy));
}
