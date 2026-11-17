#include <metal_stdlib>
using namespace metal;

// tests/FunctionTagsTest.swift: functions whose constants and attributes Metal answers from the metallib's public metadata
// (the same functions as drivers/TegraGM20BMetal/test/testdata/function_tags/probe.metal)
constant bool tags_tint [[function_constant(0)]];
constant float tags_scale [[function_constant(1)]];
constant int tags_mode [[function_constant(3)]];
constant bool tags_has_normal [[function_constant(4)]];

struct TagsVertexIn {
    float4 position [[attribute(0)]];
    float2 uv [[attribute(2)]];
    float3 normal [[attribute(5), function_constant(tags_has_normal)]];
    half4 colour [[attribute(7)]];
};

struct TagsVertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex TagsVertexOut tags_vertex(TagsVertexIn in [[stage_in]])
{
    TagsVertexOut o;
    o.position = in.position * tags_scale;
    o.uv = in.uv;
    if (is_function_constant_defined(tags_has_normal) && tags_has_normal)
        o.position.xyz += in.normal;
    return o;
}

fragment float4 tags_fragment(TagsVertexOut in [[stage_in]], texture2d<float> t [[texture(0)]],
                              sampler s [[sampler(0)]])
{
    float4 c = t.sample(s, in.uv);
    if (is_function_constant_defined(tags_tint) && tags_tint)
        c *= 0.5;
    return c;
}

struct TagsKernelIn {
    float2 p [[attribute(1)]];
    uint id [[attribute(4)]];
};

kernel void tags_kernel(TagsKernelIn in [[stage_in]], device float *out [[buffer(1)]],
                        uint gid [[thread_position_in_grid]])
{
    out[gid] = in.p.x + float(tags_mode);
}

struct TagsPatchPoint {
    float4 p [[attribute(0)]];
};

struct TagsPatchIn {
    patch_control_point<TagsPatchPoint> cp;
    float4 extra [[attribute(3)]];
};

[[patch(triangle, 3)]] vertex float4 tags_patch(TagsPatchIn in [[stage_in]], float3 bary [[position_in_patch]])
{
    return in.cp[0].p * bary.x + in.cp[1].p * bary.y + in.cp[2].p * bary.z + in.extra;
}
