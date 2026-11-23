#include <metal_stdlib>
using namespace metal;

/// gpu.gm20b.zcull side_effects: every fragment the function runs for counts once (no early fragment tests)
fragment float4 zcull_side_effect_fragment(constant float4 &colour [[buffer(0)]], device atomic_uint *count [[buffer(1)]])
{
    atomic_fetch_add_explicit(count, 1, memory_order_relaxed);
    return colour;
}
