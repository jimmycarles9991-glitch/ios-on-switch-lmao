/* cpu.sched's threads (switch_sched_probe.c): spinners that keep cores busy and time their own stalls, and waiters that
 * time how long a wakeup takes to reach them. Times are mach ticks. */
#ifndef SWITCH_SCHED_PROBE_H
#define SWITCH_SCHED_PROBE_H

#include <stdbool.h>
#include <stdint.h>

/* One spinner's account: loop passes, and the longest time between two of them (time spent off the core). */
struct switch_sched_spin {
    uint64_t passes;
    uint64_t longest_stall;
};

/* Makes the calling thread a time-constraint (realtime) thread: no timer coalescing slack, above every other class.
 * False if the system refused it or did not keep it. */
bool switch_sched_make_realtime(uint64_t period, uint64_t computation, uint64_t constraint);

/* Spins on the calling thread until *stop, accounting into *out. */
void switch_sched_spin(const volatile bool *stop, struct switch_sched_spin *out);

/* Sleeps until each of `count` deadlines `period` apart; late[i] is how long after its deadline the thread ran. */
void switch_sched_timer_wakes(uint64_t period, unsigned count, uint64_t *late);

/* A wakeup handed from a signalling thread to a waiting one through a Mach semaphore. */
struct switch_sched_handoff;

struct switch_sched_handoff *switch_sched_handoff_create(unsigned count);
void switch_sched_handoff_destroy(struct switch_sched_handoff *h);

/* The signalling side: spins `gap` ticks between signals, `count` times, each once the waiter took the last. */
void switch_sched_handoff_signal(struct switch_sched_handoff *h, uint64_t gap);

/* The waiting side: blocks for each signal; latency[i] is the time from the signal to this thread running. */
void switch_sched_handoff_wait(struct switch_sched_handoff *h, uint64_t *latency);

#endif
