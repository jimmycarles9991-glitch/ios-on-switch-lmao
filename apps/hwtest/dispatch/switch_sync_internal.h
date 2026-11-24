#ifndef SWITCH_SYNC_INTERNAL_H
#define SWITCH_SYNC_INTERNAL_H

#include "switch_sync_handoff.h"
#include "switch_v80_atomics.h"

#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <os/activity.h>
#include <pthread.h>

#define SYNC_MAX_THREADS   16
#define SYNC_MAX_PRODUCERS 4
#define SYNC_MAX_ROUNDS    32
#define SYNC_WORKLOOPS     2
#define SYNC_PER_WORKLOOP  4
#define SYNC_PLAIN         4
#define SYNC_MOVED         4
#define SYNC_CHILDREN      4 /* one on each plain queue: a sync on a child takes its parent too */
#define SYNC_EARLY_WAITERS 2 /* per moved queue, calling into it while it is inactive */
#define SYNC_PLAIN_BASE    (SYNC_WORKLOOPS * SYNC_PER_WORKLOOP)
#define SYNC_MOVED_BASE    (SYNC_PLAIN_BASE + SYNC_PLAIN)
#define SYNC_CHILD_BASE    (SYNC_MOVED_BASE + SYNC_MOVED)
#define SYNC_SERIALS       (SYNC_CHILD_BASE + SYNC_CHILDREN)
#define SYNC_CONCURRENT_ID SYNC_SERIALS
#define SYNC_MOVING_ID     32 /* + the round: the queue retargeted while a sync waits on it */
#define SYNC_QUEUE_IDS     (SYNC_MOVING_ID + SYNC_MAX_ROUNDS)

#define SYNC_PRODUCER_SLOT SYNC_MAX_THREADS
#define SYNC_EARLY_SLOT    (SYNC_PRODUCER_SLOT + SYNC_MAX_PRODUCERS)
#define SYNC_SHAPER_SLOT   (SYNC_EARLY_SLOT + SYNC_MOVED * SYNC_EARLY_WAITERS)
#define SYNC_HANDED_SLOT   (SYNC_SHAPER_SLOT + 1)
#define SYNC_SLOTS         (SYNC_HANDED_SLOT + 1)

/* Readers inside the concurrent queue count up from 0; a barrier inside sets this bit. */
#define SYNC_BARRIER (1ull << 63)

enum sync_phase {
    SYNC_IDLE,
    SYNC_WAITING, /* called into the queue, its block not yet running */
    SYNC_INSIDE,
    SYNC_NESTED, /* inside the queue, waiting on the inner one */
    SYNC_SENDING,
    SYNC_SHAPING, /* the shaper, at step `inner` of the round whose queue is `queue` */
    SYNC_DONE,
};

struct __attribute__((aligned(64))) sync_queue {
    dispatch_queue_t queue;
    uint64_t *hierarchy; /* the owner word of the workloop it targets; NULL for a queue that is its own */
    uint64_t owner;      /* the thread inside, 0 when none */
    uint64_t ran;        /* sync-family blocks run, counted inside: plain, the queue serialises it */
    uint64_t made;       /* sync-family calls that returned */
    uint64_t async_ran;  /* plain */
    uint64_t async_sent;
    uint32_t last_seq[SYNC_MAX_PRODUCERS]; /* plain: each producer's last item here, plus one */
    unsigned id;
    unsigned rank; /* a nested sync only goes to a higher rank, so no two threads wait on each other */
    char label[24];
};

struct __attribute__((aligned(64))) sync_concurrent {
    dispatch_queue_t queue;
    uint64_t state;
    uint64_t barrier_ran; /* plain: barriers exclude each other */
    uint64_t barrier_made, reader_ran, reader_made;
};

struct sync_slot {
    uint64_t tid;
    uint64_t state; /* phase << 16 | queue id << 8 | inner queue id */
};

struct sync_probe;

struct sync_arg {
    struct sync_probe *p;
    unsigned slot;
};

struct sync_activity {
    os_activity_t activity;
    struct os_activity_scope_state_s scope;
    uint64_t id;
};

struct sync_call {
    struct sync_probe *p;
    struct sync_queue *q, *inner;
    uint64_t activity;
    unsigned slot; /* SYNC_SLOTS for a sync an async item makes */
};

struct sync_item {
    struct sync_probe *p;
    struct sync_queue *q, *inner;
    uint64_t activity;
    uint64_t received;
    uint32_t producer, seq;
};

struct sync_shape {
    dispatch_queue_t home[2]; /* serial queues of their own: two workloops in the kernel */
    dispatch_semaphore_t started, release, go, finished;
    uint64_t parked_rounds, woken_on_second; /* bit per round */
    struct sync_queue moving[SYNC_MAX_ROUNDS];
};

struct sync_probe {
    struct switch_sync_config config;
    dispatch_workloop_t workloop[SYNC_WORKLOOPS];
    uint64_t hierarchy[SYNC_WORKLOOPS];
    struct sync_queue serial[SYNC_SERIALS];
    struct sync_concurrent concurrent;
    struct sync_shape shape;
    struct sync_item *items;
    uint64_t async_runs; /* item runs that complete the async side, the injected duplicate's included */
    uint64_t async_received;
    struct sync_slot slots[SYNC_SLOTS];
    struct sync_arg args[SYNC_SLOTS];
    pthread_t threads[SYNC_SLOTS];
    bool started[SYNC_SLOTS];
    uint64_t remaining; /* participants still running; the last one signals `done` */
    semaphore_t done;
    uint64_t injected;
    uint64_t overlaps, first_overlap, duplicates, out_of_order, activity_mismatches, activities;
    uint64_t nested, barriers, async_and_waits, servicer_syncs, inactive_waiters, retarget_rounds, parked;
    uint64_t faults, fault_signal, fault_pc, fault_tid;
};

/* switch_sync_handoff.c */
uint64_t sync_self_tid(void);
void sync_slot_set(struct sync_probe *p, unsigned slot, enum sync_phase phase, unsigned queue, unsigned inner);
void sync_finish(struct sync_probe *p);
bool sync_start(struct sync_probe *p, unsigned slot, void *(*body)(void *));
void sync_activity_enter(struct sync_probe *p, struct sync_activity *a, bool counted);
void sync_activity_leave(struct sync_activity *a);
void sync_enter(struct sync_probe *p, struct sync_queue *q, uint64_t me);
void sync_leave(struct sync_probe *p, struct sync_queue *q, uint64_t me);
void sync_serial_block(void *call);

/* switch_sync_shapes.c */
void sync_make_moved(struct sync_probe *p);
bool sync_start_early_waiters(struct sync_probe *p);
void sync_activate_moved(struct sync_probe *p);
void sync_make_shape(struct sync_probe *p);
bool sync_start_shape(struct sync_probe *p);
void sync_release_shape(struct sync_probe *p);

/* switch_sync_watch.c */
const char *sync_queue_label(const struct sync_probe *p, unsigned id);
void sync_describe_wedge(const struct sync_probe *p, char *out, size_t size);
void sync_catch_faults(struct sync_probe *p);
void sync_release_faults(void);

#endif
