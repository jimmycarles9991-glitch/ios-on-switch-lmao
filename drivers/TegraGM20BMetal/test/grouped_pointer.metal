#include <metal_stdlib>
using namespace metal;

[[visible]] uint grouped_add(uint x)
{
    return x + 5;
}

[[visible]] uint grouped_mul(uint x)
{
    return x * 3;
}

kernel void grouped_pointer(device uint *output [[buffer(0)]], uint tid [[thread_position_in_grid]])
{
    uint (*function)(uint) = (tid & 1) ? grouped_add : grouped_mul;
    output[tid] = function(tid);
}
