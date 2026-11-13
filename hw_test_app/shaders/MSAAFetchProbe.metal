#include <metal_stdlib>
using namespace metal;

// gpu.m2v.msaa_fetch's fragments, over fetch_vertex's triangles (FramebufferFetch.metal): its output, member for member
struct MSAAProbeVertex {
    float4 position [[position]];
    uint step [[flat]];
};

static float3 msaa_probe_step(uint step)
{
    return float3((step * 3 + 1) % 7, (step * 5 + 2) % 7, (step + 3) % 7) / 8.0f;
}

fragment float4 msaa_probe_plain_fragment(MSAAProbeVertex v [[stage_in]])
{
    return float4(msaa_probe_step(v.step), 1);
}

// `zero` keeps every sample's colour the same, so the resolve is exact
fragment float4 msaa_probe_sample_fragment(MSAAProbeVertex v [[stage_in]], uint id [[sample_id]],
                                           constant uint &zero [[buffer(0)]])
{
    return float4(msaa_probe_step(v.step) + float(id * zero), 1);
}

// m2v orders a raster order group's accesses in the pixel interlock; a read alone writes no memory, so the pipeline
// keeps one invocation per pixel
fragment float4 msaa_probe_ordered_fragment(MSAAProbeVertex v [[stage_in]],
                                            device uint *zero [[buffer(1), raster_order_group(0)]])
{
    return float4(msaa_probe_step(v.step) + float(zero[0]), 1);
}

fragment float4 msaa_probe_ordered_sample_fragment(MSAAProbeVertex v [[stage_in]], uint id [[sample_id]],
                                                   device uint *zero [[buffer(1), raster_order_group(0)]])
{
    return float4(msaa_probe_step(v.step) + float(id * zero[0]), 1);
}

// fetch_order_fragment's step, counting its invocations: a fragment that writes memory runs under the pipeline's
// sample shading on m2v (metal2vulkan link::graphics sample_invocations)
fragment float4 msaa_probe_fetch_count_fragment(MSAAProbeVertex v [[stage_in]], float4 previous [[color(0)]],
                                                device atomic_uint *count [[buffer(1)]])
{
    atomic_fetch_add_explicit(count, 1, memory_order_relaxed);
    return float4(previous.rgb * 0.5f + msaa_probe_step(v.step) * 0.5f, 1);
}

fragment void msaa_probe_count_fragment(device atomic_uint *count [[buffer(1)]])
{
    atomic_fetch_add_explicit(count, 1, memory_order_relaxed);
}
