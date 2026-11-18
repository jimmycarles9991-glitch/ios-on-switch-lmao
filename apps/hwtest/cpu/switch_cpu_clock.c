#include "switch_cpu_clock.h"

#define SWITCH_CPU_CLOCK_TEXT(x)  #x
#define SWITCH_CPU_CLOCK_CHAIN(n) ".rept " SWITCH_CPU_CLOCK_TEXT(n) "\n add %0, %0, #1\n .endr"

__attribute__((noinline)) uint64_t switch_cpu_add_chain(uint64_t blocks)
{
    uint64_t x = blocks;
    for (uint64_t i = 0; i < blocks; i++)
        __asm__ volatile(SWITCH_CPU_CLOCK_CHAIN(SWITCH_CPU_CLOCK_BLOCK) : "+r"(x));
    return x;
}
