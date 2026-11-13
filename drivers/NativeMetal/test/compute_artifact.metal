#include <metal_stdlib>
using namespace metal;

struct ArtifactInner {
    float2 uv;
    uint tag;
};

struct ArtifactInput {
    float4 weights;
    ArtifactInner inner;
    float4 rows[2];
};

kernel void artifact_probe(constant ArtifactInput &input [[buffer(0)]], device uint *output [[buffer(1)]],
                           texture2d<float, access::read> image [[texture(0)]],
                           threadgroup float *dynamic [[threadgroup(0)]], uint tid [[thread_index_in_threadgroup]],
                           uint gid [[thread_position_in_grid]])
{
    threadgroup float fixed[32];
    fixed[tid & 31] = input.weights.x + input.inner.uv.y + input.rows[tid & 1].z;
    dynamic[tid] = image.read(uint2(gid & 7, 0)).x;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    output[gid] = as_type<uint>(fixed[(tid + 1) & 31] + dynamic[tid]) ^ input.inner.tag;
}
