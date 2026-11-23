// The frontend's moves between an RGB8_444_2P texture's two planes and its RGBA16Float render stand-in
// (NativeBridgeMetalFrontend.mm, "Two-plane render targets"). Built by TwoPlaneStandIn.sh into TwoPlaneStandIn.h.
#include <metal_stdlib>
using namespace metal;

struct Position {
    float4 position [[position]];
};

// One triangle over the whole target.
vertex Position two_plane_cover(uint id [[vertex_id]])
{
    const float2 uv = float2((id << 1) & 2, id & 2);
    return {float4(uv * 2.0 - 1.0, 0.0, 1.0)};
}

// The planes' codes into the stand-in: each texel's code / 255 (what the texture reads, alpha 1), rounded to half as Metal's
// tile holds the attachment it loads.
fragment half4 two_plane_load(Position in [[stage_in]], texture2d<half, access::read> planes [[texture(0)]])
{
    return half4(planes.read(uint2(in.position.xy)).rgb, 1.0h);
}

// The stand-in into the planes: green to the luma plane, blue and red to the chroma plane's pairs, each stored as the
// attachment's unorm8 code, round(h * 255) of the half the stand-in holds (Metal's store, measured on an M4).
struct Planes {
    half luma [[color(0)]];
    half2 chroma [[color(1)]];
};
fragment Planes two_plane_store(Position in [[stage_in]], texture2d<half, access::read> stand_in [[texture(0)]])
{
    const half4 c = stand_in.read(uint2(in.position.xy));
    return {c.g, half2(c.b, c.r)};
}
