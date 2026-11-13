#include <metal_stdlib>
using namespace metal;

// gpu.coherence.cpu_rewrite: a draw that reads its destination through the ROP and takes nothing the CPU wrote, so no
// buffer of the job can make the kernel drop the L2 for it
vertex float4 coherence_vertex(uint vid [[vertex_id]])
{
    return float4(vid == 1 ? 3.0f : -1.0f, vid == 2 ? 3.0f : -1.0f, 0, 1);
}

fragment float4 coherence_fragment()
{
    return float4(1, 2, 3, 4) / 255.0f;
}
