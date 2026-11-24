#include <metal_stdlib>
using namespace metal;

// compiler_stages_test's states: a vertex function that reads two attributes, a fragment function that reads its
// position, one that fetches its colour attachment, and a kernel that reads a texture

struct StagesIn {
    float4 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct StagesOut {
    float4 position [[position]];
    float2 uv;
};

vertex StagesOut stages_vertex(StagesIn in [[stage_in]])
{
    return {in.position, in.uv};
}

fragment float4 stages_fragment(StagesOut in [[stage_in]])
{
    float4 v = float4(in.uv, in.position.xy * 0.001f);
    for (int i = 0; i < 8; i++)
        v = fract(fma(v, float4(1.618034f, 1.414214f, 1.732051f, 2.236068f), float4(0.1f, 0.2f, 0.3f, 0.4f)));
    return v;
}

fragment float4 stages_fetch_fragment(StagesOut in [[stage_in]], float4 fetched [[color(0)]])
{
    return fract(fetched + float4(in.uv, 0.25f, 0.5f));
}

kernel void stages_kernel(texture2d<float, access::read> t [[texture(0)]], device float4 *out [[buffer(0)]],
                          uint2 id [[thread_position_in_grid]])
{
    if (id.x < t.get_width() && id.y < t.get_height())
        out[id.y * t.get_width() + id.x] = t.read(id);
}
