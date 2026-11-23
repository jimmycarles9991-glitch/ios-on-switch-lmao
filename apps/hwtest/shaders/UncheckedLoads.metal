#include <metal_stdlib>
using namespace metal;

struct UncheckedArgs {
    device const uint *data;
};

/// gpu.gm20b.unchecked_buffer_loads: word i read through the bound buffer and through the argument buffer's pointer
kernel void unchecked_reads(device const uint *direct [[buffer(0)]], device const UncheckedArgs &args [[buffer(1)]],
                            device uint2 *out [[buffer(2)]], uint i [[thread_position_in_grid]])
{
    out[i] = uint2(direct[i], args.data[i]);
}

/// Word i of buffer slot 3, which the test leaves unbound
kernel void unchecked_unbound(device const uint *unbound [[buffer(3)]], device uint *out [[buffer(2)]],
                              uint i [[thread_position_in_grid]])
{
    out[i] = unbound[i];
}
