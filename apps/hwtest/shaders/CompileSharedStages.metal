#include <metal_stdlib>
using namespace metal;

/// gpu.compile.shared_stages: a fragment long enough that its stage compile is most of its state's compile, salted with
/// each run's own value so that no pack or store holds it before the run
constant uint shared_stages_salt [[function_constant(0)]];

#define SHARED_STEP(v) \
    v = fract(fma(v, float4(1.618034f, 1.414214f, 1.732051f, 2.236068f), float4(0.1f, 0.2f, 0.3f, 0.4f)));
#define SHARED_4(v) SHARED_STEP(v) SHARED_STEP(v) SHARED_STEP(v) SHARED_STEP(v)
#define SHARED_16(v) SHARED_4(v) SHARED_4(v) SHARED_4(v) SHARED_4(v)
#define SHARED_64(v) SHARED_16(v) SHARED_16(v) SHARED_16(v) SHARED_16(v)

struct SharedStagesVertex {
    float4 position [[position]];
};

fragment float4 shared_stages_fragment(SharedStagesVertex v [[stage_in]])
{
    float4 x = float4(v.position.xy * 0.001f, float(shared_stages_salt & 0xffffu) * 0.00001f, 0.5f);
    SHARED_64(x)
    return float4(x.rgb, 0.5f);
}
