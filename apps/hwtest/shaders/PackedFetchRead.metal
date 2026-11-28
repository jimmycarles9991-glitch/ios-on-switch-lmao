#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.packed_fetch_read (gpu/PackedFetchRead.swift): what a fragment's framebuffer fetch reads of a 16-bit packed
/// target's loaded texel, as float bits in target 1; target 0 keeps the texel
struct PackedFetchRead {
    float4 texel [[color(0)]];
    uint4 bits [[color(1)]];
};

fragment PackedFetchRead packed_fetch_read_float(float4 texel [[color(0)]])
{
    return {texel, as_type<uint4>(texel)};
}

struct PackedFetchReadHalf {
    half4 texel [[color(0)]];
    uint4 bits [[color(1)]];
};

fragment PackedFetchReadHalf packed_fetch_read_half(half4 texel [[color(0)]])
{
    return {texel, as_type<uint4>(float4(texel))};
}
