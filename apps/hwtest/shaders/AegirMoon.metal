#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.aegir_moon (gpu/AegirMoon.swift): the three passes NanoUniverse's Aegir renderer draws a frame of Weather's
/// moon card with, its luna, threshold and post shaders cut down to what decides the bytes
struct AegirMoonVertex {
    float4 position [[position]];
    float2 uv;
};

vertex AegirMoonVertex aegir_moon_vertex(uint vid [[vertex_id]])
{
    const float2 uv = float2(vid & 1, vid >> 1);
    return {float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1), uv};
}

struct AegirMoonTargets {
    half4 colour [[color(0)]];
    half4 mask [[color(1)]];
};

/// The scene pass: grey 0.8 and a mask of 0 inside a disc of radius 0.35, at alpha 1; outside, colour 0 and mask 1 at
/// alpha 0, as Aegir's luna writes them, so its source-over blends keep both clears there
fragment AegirMoonTargets aegir_moon_scene(AegirMoonVertex in [[stage_in]])
{
    const bool inside = distance(in.uv, float2(0.5)) < 0.35;
    return inside ? AegirMoonTargets{half4(0.8h, 0.8h, 0.8h, 1), half4(0, 0, 0, 1)}
                  : AegirMoonTargets{half4(0), half4(1, 1, 1, 0)};
}

/// Aegir's threshold: the drawable's luminance less 0.365, times 3.18
fragment half4 aegir_moon_threshold(AegirMoonVertex in [[stage_in]], texture2d<half> drawable [[texture(0)]])
{
    constexpr sampler linear(filter::linear);
    const half3 c = drawable.sample(linear, in.uv).rgb;
    return half4((dot(c, half3(0.2126h, 0.7152h, 0.0722h)) - 0.365h) * 3.18h);
}

/// Aegir's post pass at bloomStrength 0: nothing added to the colour (alpha 1, which BGR10_XR_sRGB holds none of), bloom
/// level 2 taken off the mask
fragment AegirMoonTargets aegir_moon_post(AegirMoonVertex in [[stage_in]], texture2d<half> bloom [[texture(0)]])
{
    constexpr sampler linear(filter::linear, mip_filter::linear);
    return {half4(0, 0, 0, 1), half4(bloom.sample(linear, in.uv, level(2)).r)};
}
