#include <metal_stdlib>
using namespace metal;

// gpu/PlainRows.swift: each plane texel's code, so a byte several plane rows reach tells which row's store it kept.
// `shape` is (bytes a texel, the plane's width, its height, the target's rotation); a turned target's frame texel is
// taken back to its plane texel first.
static uint2 plain_rows_texel(float4 position, uint4 shape)
{
    const uint2 f = uint2(position.xy);
    switch (shape.w) {
    case 1:
        return uint2(shape.y - 1 - f.y, f.x);
    case 2:
        return uint2(shape.y - 1 - f.x, shape.z - 1 - f.y);
    case 3:
        return uint2(f.y, shape.z - 1 - f.x);
    default:
        return f;
    }
}

fragment float4 plain_rows_code_unorm(float4 position [[position]], constant uint4 &shape [[buffer(0)]])
{
    const uint2 p = plain_rows_texel(position, shape);
    return float4(float((p.x + 7 * p.y) & 255) / 255.0f);
}

fragment uint4 plain_rows_code_words(float4 position [[position]], constant uint4 &shape [[buffer(0)]])
{
    const uint2 p = plain_rows_texel(position, shape);
    const uint c = (p.x * 7 + p.y * 131 + 1) & 0xffff;
    uint4 words = 0;
    for (uint b = 0; b < shape.x; b++)
        words[b / 4] |= ((((b & 1) ? c >> 8 : c) ^ (b / 2) * 0x11) & 0xff) << (8 * (b % 4));
    return words;
}
