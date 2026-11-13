#include <metal_stdlib>
using namespace metal;

kernel void semantic_edges(device uint4 *output [[buffer(0)]],
                            uint3 global [[thread_position_in_grid]],
                            uint3 group [[threadgroup_position_in_grid]],
                            uint3 grid [[threads_per_grid]],
                            uint3 size [[threads_per_threadgroup]],
                            uint3 local [[thread_position_in_threadgroup]],
                            uint3 groups [[threadgroups_per_grid]],
                            uint index [[thread_index_in_threadgroup]],
                            uint lane [[thread_index_in_simdgroup]],
                            uint subgroup [[simdgroup_index_in_threadgroup]],
                            uint width [[threads_per_simdgroup]])
{
    output += index * 8;
    output[0] = uint4(global, 0x101);
    output[1] = uint4(group, 0x202);
    output[2] = uint4(grid, 0x303);
    output[3] = uint4(size, 0x404);
    output[4] = uint4(local, 0x505);
    output[5] = uint4(groups, 0x606);
    output[6] = uint4(index, lane, subgroup, width);
}

kernel void semantic_edges_barrier(device uint4 *output [[buffer(0)]],
                                    threadgroup uint *scratch [[threadgroup(0)]],
                                    uint3 global [[thread_position_in_grid]],
                                    uint3 size [[threads_per_threadgroup]],
                                    uint index [[thread_index_in_threadgroup]])
{
    scratch[index] = global.x ^ (global.y << 8) ^ (global.z << 16);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    output[index * 8] = uint4(scratch[index], size);
}
