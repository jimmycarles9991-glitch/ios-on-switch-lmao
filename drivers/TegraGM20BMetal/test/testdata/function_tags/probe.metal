#include <metal_stdlib>
using namespace metal;
constant bool useTint [[function_constant(0)]];
constant float scale [[function_constant(1)]];
constant int mode [[function_constant(3)]];
constant bool hasNormal [[function_constant(4)]];
struct VIn {
    float4 position [[attribute(0)]];
    float2 uv [[attribute(2)]];
    float3 normal [[attribute(5), function_constant(hasNormal)]];
    half4 colour [[attribute(7)]];
};
struct VOut { float4 position [[position]]; float2 uv; };
vertex VOut v_attrs(VIn in [[stage_in]]) {
    VOut o; o.position = in.position * scale; o.uv = in.uv;
    if (is_function_constant_defined(hasNormal) && hasNormal) o.position.xyz += in.normal;
    return o;
}
fragment float4 f_tint(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {
    float4 c = t.sample(s, in.uv);
    if (is_function_constant_defined(useTint) && useTint) c *= 0.5;
    return c;
}
struct KIn { float2 p [[attribute(1)]]; uint id [[attribute(4)]]; };
kernel void k_stage(KIn in [[stage_in]], device float *out [[buffer(1)]], uint gid [[thread_position_in_grid]]) {
    out[gid] = in.p.x + float(mode);
}
kernel void k_plain(device float *out [[buffer(0)]], uint gid [[thread_position_in_grid]]) { out[gid] = 1; }
struct PIn { float4 p [[attribute(0)]]; };
struct PatchIn { patch_control_point<PIn> cp; float4 extra [[attribute(3)]]; };
[[patch(triangle, 3)]] vertex float4 v_patch(PatchIn in [[stage_in]], float3 bary [[position_in_patch]]) {
    return in.cp[0].p * bary.x + in.cp[1].p * bary.y + in.cp[2].p * bary.z + in.extra;
}
