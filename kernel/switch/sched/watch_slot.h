// SPDX-License-Identifier: GPL-2.0-only
/*
 * The watched threads' slots and the focal app's launch record, shared by watch.c (the hooks, which fill them) and
 * watch_write.c (the writer and the 10 s report, which read them), debug kernels.
 */
#ifndef SWITCH_SCHED_WATCH_SLOT_H
#define SWITCH_SCHED_WATCH_SLOT_H

#include <kern/thread_call.h>

#include "busy.h"
#include "slots.h"
#include "watch.h"

#define WATCH_RING     2
#define COMMIT_LATE_US 16667
#define TOUCH_LATE_US  8000

struct watch_slot {
    int pid;
    char name[SWITCH_WATCH_NAME];
    struct switch_watch_thread t;        /* to each event */
    struct switch_busy run;              /* between two run-loop waits, while the focal app's main thread */
    struct switch_watch_counts counts;   /* the thread's */
    struct switch_watch_counts reported; /* the report's */
    struct switch_watch_event ring[WATCH_RING];
    uint32_t head; /* the thread's */
    uint32_t tail; /* the writer's */
};

extern struct switch_slots switch_watch_table;
extern struct watch_slot switch_watch_slots[SWITCH_SLOTS];
extern struct switch_watch_launch switch_watch_launch;
extern uint32_t switch_watch_claimed, switch_watch_freed, switch_watch_refused;

void switch_watch_write(thread_call_param_t p0, thread_call_param_t p1);

#endif /* SWITCH_SCHED_WATCH_SLOT_H */
