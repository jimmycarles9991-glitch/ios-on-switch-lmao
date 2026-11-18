/* cpu.ipi_pingpong's two threads (switch_ipi_probe.c): each blocks on its own Mach semaphore and wakes the other's, so
 * every half trip wakes a thread that is not running. After its signal the waker spins `hold` ticks before it blocks,
 * keeping its own core busy, so the woken thread runs on another core: each wake is a cross-core IPI. Times are mach
 * ticks. */
#ifndef SWITCH_IPI_PROBE_H
#define SWITCH_IPI_PROBE_H

#include <stdbool.h>
#include <stdint.h>

struct switch_ipi_pingpong;

/* `rounds` round trips; NULL if the semaphores cannot be made. */
struct switch_ipi_pingpong *switch_ipi_pingpong_create(unsigned rounds);
void switch_ipi_pingpong_destroy(struct switch_ipi_pingpong *p);

/* One side of it, on a thread of its own: `first` starts each round. latency[i] is the time from the other side's
 * signal to this thread running, for each of its `rounds` wakes. Returns the wakes that never came within `timeout`
 * (the side stops at the first). */
unsigned switch_ipi_pingpong_side(struct switch_ipi_pingpong *p, bool first, uint64_t hold, uint64_t timeout,
                                  uint64_t *latency);

#endif
