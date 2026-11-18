#include "switch_a64x_probe.h"

__attribute__((noinline)) void switch_a64x_hammer(uint64_t *counter, uint64_t times)
{
    for (uint64_t i = 0; i < times; i++)
        __atomic_fetch_add(counter, 1, __ATOMIC_SEQ_CST);
}

__attribute__((noinline)) float switch_a64x_fp16_mul_add(float a, float b, float c)
{
    const _Float16 product = (_Float16)a * (_Float16)b;
    const _Float16 sum = product + (_Float16)c;
    return (float)sum;
}
