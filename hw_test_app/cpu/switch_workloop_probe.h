/* system.workloop_sync (switch_workloop_probe.c): the kernel's workloop sync handoff, the one dispatch_sync waits on,
 * driven by hand. Waiter threads park in kevent_id(NOTE_WL_SYNC_WAIT); one waker hands each of them the workloop in
 * turn (NOTE_WL_SYNC_WAKE | NOTE_WL_DISCOVER_OWNER, the owner named in a state word as dq_state names it) and waits for
 * it back (EV_DELETE | NOTE_WL_END_OWNERSHIP). Nothing on that path runs libdispatch or any dyld cache code: the
 * syscalls are made here and the atomics are ARMv8.0 instructions, so the a64x loader has nothing in it to stub. */
#ifndef SWITCH_WORKLOOP_PROBE_H
#define SWITCH_WORKLOOP_PROBE_H

#include <stdbool.h>
#include <stdint.h>

struct switch_workloop_result {
    uint32_t handoffs;    /* wakes the waker made */
    uint32_t early;       /* SYNC_WAITs that returned before the waker had handed that waiter the workloop */
    uint32_t parked;      /* SYNC_WAITs that blocked in the kernel (the rest found their wake already posted) */
    uint32_t interrupted; /* SYNC_WAITs that came back EINTR and were made again */
    uint32_t errors;      /* any other kevent_id error, on either side */
    int32_t first_error;  /* the first of those: its errno, 0 if none */
    uint32_t stuck;       /* waits of the waker's that ran past their deadline (the run then stops) */
};

/* `generations` times: `waiters` new threads, each taking the workloop `rounds` times, then exiting. `keep_knotes`
 * leaves out the waiters' EV_DELETE, so every later wait finds the last wake still posted and returns at once: the
 * early count's own check. False when an argument is out of range or a thread could not be made. */
bool switch_workloop_probe(unsigned generations, unsigned waiters, unsigned rounds, bool keep_knotes,
                           struct switch_workloop_result *out);

#endif
