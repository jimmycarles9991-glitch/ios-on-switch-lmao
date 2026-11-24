/* dispatch.sync_handoff: libdispatch's sync hand-offs in the shapes iOS 27's daemons crash in. Threads at mixed QoS
 * dispatch_sync (a quarter nested one level, across workloops), dispatch_barrier_sync and dispatch_async_and_wait onto
 * serial queues on two dispatch_workloops, plain serial queues and a child queue on each (a sync on the child takes
 * both), queues retargeted to the other workloop while inactive and a concurrent queue; producers dispatch_async numbered items under their own os_activity, some of which
 * dispatch_sync from the workloop's servicer onto another workloop's queue; and a queue is retargeted from one serial
 * hierarchy to another while a dispatch_sync waits on it, so the waiter waits on one workloop and is woken from the
 * other (libdispatch's _dispatch_event_loop_cancel_waiter). Every block checks that it is alone in its queue and its
 * workloop and runs under its caller's activity; a thread that finds another inside parks there for good, so a waiter
 * that left its wait early never returns into a frame libdispatch still links. The caller waits on a semaphore with a
 * deadline, never on a sync: a wedge is reported with the queues and the thread ids inside and waiting. */
#ifndef SWITCH_SYNC_HANDOFF_H
#define SWITCH_SYNC_HANDOFF_H

#include <stdbool.h>
#include <stdint.h>

/* The probe's own checks, each made to fire once (the host test): BREAK_NONE everywhere else. */
enum switch_sync_break {
    SWITCH_SYNC_BREAK_NONE,
    SWITCH_SYNC_BREAK_OVERLAP,   /* the first sync thread enters its queue a second time from inside it */
    SWITCH_SYNC_BREAK_WEDGE,     /* the first sync thread's first block never returns */
    SWITCH_SYNC_BREAK_ACTIVITY,  /* the first producer's first item expects another activity than its producer's */
    SWITCH_SYNC_BREAK_DUPLICATE, /* the first producer's first item is enqueued twice */
};

struct switch_sync_config {
    uint32_t sync_threads; /* 1 to 16 */
    uint32_t syncs_per_thread;
    uint32_t producers; /* 1 to 4 */
    uint32_t items_per_producer;
    uint32_t retarget_rounds; /* 1 to 32 */
    uint32_t deadline_ms;
    enum switch_sync_break inject;
};

struct switch_sync_result {
    bool finished; /* every thread and item done before the deadline, no fault, no thread parked */
    double elapsed_ms;
    uint64_t syncs; /* sync-family calls that returned */
    uint64_t async_sent, async_received;
    uint32_t overlaps;           /* a block that found another thread inside its queue or workloop */
    uint32_t counter_mismatches; /* queues whose blocks run differ from the calls made on them */
    uint32_t duplicates, out_of_order, activity_mismatches;
    uint32_t activities; /* sync threads and producers whose os_activity has an identifier */
    uint32_t nested, barriers, async_and_waits, servicer_syncs, inactive_waiters;
    uint32_t retarget_rounds, retarget_parked; /* rounds, and those whose waiter was seen blocked before the release */
    uint32_t retarget_crossed;                 /* parked rounds whose waiter's block ran under the second hierarchy */
    uint32_t parked;                           /* threads parked inside a block that found another there */
    uint32_t faults;                           /* signals caught in libdispatch or in the probe's threads */
    int32_t fault_signal;
    uint64_t fault_pc, fault_tid;
    char first_overlap[24]; /* the queue of the first overlap */
    char wedge[640];        /* unfinished: per queue, the threads inside and waiting; items left; the shape's round */
};

/* False when the configuration is out of range or a thread or queue could not be made. */
bool switch_sync_handoff(const struct switch_sync_config *config, struct switch_sync_result *out);

#endif
