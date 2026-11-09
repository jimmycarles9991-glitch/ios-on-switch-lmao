/* A chain of dependent ADDs, one cycle each on the Cortex-A57 (and on Apple's cores): the cycles a thread ran are
 * `blocks` * SWITCH_CPU_CLOCK_BLOCK, so cycles over seconds is its clock. */
#ifndef SWITCH_CPU_CLOCK_H
#define SWITCH_CPU_CLOCK_H

#include <stdint.h>

#define SWITCH_CPU_CLOCK_BLOCK 1000

uint64_t switch_cpu_add_chain(uint64_t blocks);

#endif
