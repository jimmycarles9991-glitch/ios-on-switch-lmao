#include <metal_stdlib>
using namespace metal;

// The shapes of Maps' 4x MSAA framebuffer-fetch states (VectorKit 24B119: Hillshade::hillshade_fragment,
// GlowAlpha::glowAlpha_fragment), each beside a fixed-function blend of the same arithmetic

struct MapsVertex {
    float4 position [[position]];
    float2 texcoord;
    float ramp;
};

vertex MapsVertex maps_fetch_vertex(uint vid [[vertex_id]])
{
    const float2 points[3] = {float2(-1, -1), float2(3, -1), float2(-1, 3)};
    const float2 p = points[vid % 3];
    return {float4(p, 0, 1), p * 0.5f + 0.5f, saturate(p.x * 0.5f + 0.5f)};
}

struct MapsStyle {
    float4 low;
    float4 high;
    half opacity;
    int mode;
};

// VectorKit's Blending::blend: `src` over `dst` by `mode`, mixed in by `k`, alpha 1; `dst` as it was where src has no
// alpha. Every mode reads the fetched colour, as the original's 19 do.
static half4 maps_blend(half4 dst, half4 src, half k, int mode)
{
    if (!(src.a > 0))
        return dst;
    const half3 s = src.rgb, d = dst.rgb;
    half3 b = s;
    switch (mode) {
    case 1:
        b = s * d;
        break;
    case 2:
        b = s + d - s * d;
        break;
    case 3:
        b = select(1 - 2 * (1 - d) * (1 - s), 2 * d * s, d < 0.5h);
        break;
    case 4:
        b = min(d, s);
        break;
    case 5:
        b = max(d, s);
        break;
    case 6:
        b = select(min(1.0h, d / max(1 - s, 0.001h)), half3(0), d == 0);
        break;
    case 7:
        b = select(1 - min(1.0h, (1 - d) / max(s, 0.001h)), half3(1), d == 1);
        break;
    case 8:
        b = select(1 - 2 * (1 - d) * (1 - s), 2 * d * s, s < 0.5h);
        break;
    case 9:
        b = (1 - 2 * s) * d * d + 2 * s * d;
        break;
    case 10:
        b = abs(d - s);
        break;
    case 11:
        b = d + s - 2 * d * s;
        break;
    default:
        break;
    }
    return half4(b * k + (1 - k) * d, 1);
}

fragment half4 maps_hillshade_fragment(MapsVertex v [[stage_in]], constant MapsStyle &style [[buffer(0)]],
                                       half4 dst [[color(0)]], texture2d<half> map [[texture(0)]],
                                       sampler smp [[sampler(0)]])
{
    const half shade = map.sample(smp, float2(v.texcoord.x, 1 - v.texcoord.y)).r;
    const half4 src = half4(mix(style.low, style.high, float(shade)));
    return maps_blend(dst, src, style.opacity * dst.a, style.mode);
}

// The multiply mode over an opaque colour by the ROP: dst * (1 - k (1 - src)) is dst times this colour (blend factors
// zero and source colour), alpha 1
fragment half4 maps_hillshade_rop_fragment(MapsVertex v [[stage_in]], constant MapsStyle &style [[buffer(0)]],
                                           texture2d<half> map [[texture(0)]], sampler smp [[sampler(0)]])
{
    const half shade = map.sample(smp, float2(v.texcoord.x, 1 - v.texcoord.y)).r;
    const half3 src = half3(mix(style.low, style.high, float(shade)).rgb);
    return half4(1 - style.opacity * (1 - src), 1);
}

fragment half4 maps_glow_fragment(MapsVertex v [[stage_in]], half4 dst [[color(0)]])
{
    return half4(dst.rgb, min(half(1 - v.ramp), dst.a));
}

// GlowAlpha by the ROP: colour channels masked, alpha blended by min
fragment half4 maps_glow_rop_fragment(MapsVertex v [[stage_in]])
{
    return half4(0, 0, 0, half(1 - v.ramp));
}

fragment half4 maps_plain_fragment(MapsVertex v [[stage_in]])
{
    return half4(half2(v.texcoord), 0.5h, 1);
}
