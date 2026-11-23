#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.ycbcr_overlap_rows: a 4:2:0 codes target's (Cr, Y, Cb): Y (x + 7y) & 255, and over chroma block b
/// Cb (b.x + 5 b.y) & 255, Cr (3 b.x + 11 b.y + 1) & 255
fragment float4 overlap_codes(float4 position [[position]])
{
    const uint2 x = uint2(position.xy), b = x >> 1;
    return float4(float((3 * b.x + 11 * b.y + 1) & 255), float((x.x + 7 * x.y) & 255), float((b.x + 5 * b.y) & 255), 255) /
           255.0;
}

/// gpu.gm20b.ycbcr_overlap_words: hashed 10-bit codes of each pixel, a 4:4:4 target's (Cr, Y, Cb)
fragment float4 overlap_hash(float4 position [[position]])
{
    const uint2 x = uint2(position.xy);
    uint h = x.x * 2654435761u ^ x.y * 2246822519u;
    h ^= h >> 13;
    h *= 3266489917u;
    h ^= h >> 16;
    uint g = x.x * 2246822519u ^ x.y * 2654435761u;
    g ^= g >> 15;
    g *= 668265263u;
    g ^= g >> 13;
    return float4(float(g & 1023), float(h & 1023), float((g >> 12) & 1023), 1023) / 1023.0;
}
