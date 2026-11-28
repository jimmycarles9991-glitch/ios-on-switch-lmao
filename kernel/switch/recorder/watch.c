// SPDX-License-Identifier: GPL-2.0-only
/*
 * The flight recorder's hang trigger (--//kernel:recorder). SpringBoard's and backboardd's main threads (diag/stall.c's
 * exec hook) and every focal app's (sched/stats.c's role hook, <switch/focal.h>) are read at the scheduler's tick
 * (diag/stall.c's, on the
 * maintenance thread: no timer of its own). A hang is the thread's busy stretch as sched/busy.c keeps it from its
 * departures and dispatches: open, away from its run loop's wait, for SWITCH_RECORDER_HANG_NS. Its trigger carries
 * the thread id and the milliseconds the stretch has lasted, and its dump takes the stackshot while it still hangs.
 */

#include <switch/focal.h>
#include <switch/recorder.h>
#include <switch/sched.h>
#include <switch/xnu.h>

#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/misc_protos.h>
#include <kern/sched_prim.h>
#include <kern/simple_lock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/task_policy.h>
#include <string.h>

#include "recorder.h"

#if SWITCH_RECORDER

enum switch_recorder_watch_slot {
    SWITCH_RECORDER_WATCH_SPRINGBOARD,
    SWITCH_RECORDER_WATCH_BACKBOARDD,
    SWITCH_RECORDER_WATCH_APP, /* the first of SWITCH_FOCAL_PLACES, one per focal app */
    SWITCH_RECORDER_WATCHES = SWITCH_RECORDER_WATCH_APP + SWITCH_FOCAL_PLACES,
};

#define SWITCH_RECORDER_WATCH_EVERY_NS 100000000ull /* the most often the tick reads them */

/* The slots: exec, role and the tick all take it, none of them on a recorded path, and none holds it for more than a
 * few thread-state reads. */
static SIMPLE_LOCK_DECLARE(switch_recorder_watch_lock, 0);
static thread_t switch_recorder_watch_thread[SWITCH_RECORDER_WATCHES]; /* +1 each */
static struct switch_recorder_watch switch_recorder_watch_state[SWITCH_RECORDER_WATCHES];
static struct switch_focal switch_recorder_focal; /* the tasks of the app slots */
static uint64_t switch_recorder_watch_bound, switch_recorder_watch_every, switch_recorder_watch_last;

/* Under the watch lock: the slot watches thread (+1, consumed; THREAD_NULL: nothing), and the old one's reference. */
static thread_t switch_recorder_watch_put(unsigned slot, thread_t thread)
{
    const thread_t old = switch_recorder_watch_thread[slot];

    if (switch_recorder_watch_bound == 0) {
        nanoseconds_to_absolutetime(SWITCH_RECORDER_HANG_NS, &switch_recorder_watch_bound);
        nanoseconds_to_absolutetime(SWITCH_RECORDER_WATCH_EVERY_NS, &switch_recorder_watch_every);
    }
    switch_recorder_watch_thread[slot] = thread;
    if (old != thread) {
        switch_recorder_watch_state[slot] = (struct switch_recorder_watch){0};
    }
    return old;
}

/* Consumes the reference on thread (THREAD_NULL: stop watching the slot). */
static void switch_recorder_watch_set(enum switch_recorder_watch_slot slot, thread_t thread)
{
    thread_t old = THREAD_NULL;

    simple_lock(&switch_recorder_watch_lock, LCK_GRP_NULL);
    old = switch_recorder_watch_put(slot, thread);
    simple_unlock(&switch_recorder_watch_lock);
    if (old != THREAD_NULL) {
        thread_deallocate(old);
    }
}

void switch_recorder_exec(struct thread *thread)
{
    char name[32];

    switch_task_name(get_threadtask(thread), name, sizeof(name));
    if (strcmp(name, "SpringBoard") == 0 || strcmp(name, "backboardd") == 0) {
        thread_reference(thread);
        switch_recorder_watch_set(name[0] == 'S' ? SWITCH_RECORDER_WATCH_SPRINGBOARD : SWITCH_RECORDER_WATCH_BACKBOARDD,
                                  thread);
    }
}

/* main: the task's main thread, the caller's reference (THREAD_NULL when it has none). Every focal app has its own
 * slot: one leaving the role leaves the others watched. */
void switch_recorder_role(struct task *task, int role, struct thread *main)
{
    const uint64_t key = (uint64_t)(uintptr_t)task;
    const bool focal = role == TASK_FOREGROUND_APPLICATION && main != THREAD_NULL;
    char name[32];
    thread_t gone = THREAD_NULL;
    uint64_t evicted = 0;
    unsigned place = SWITCH_FOCAL_PLACES;

    switch_task_name(task, name, sizeof(name));
    if (strcmp(name, "SpringBoard") == 0) {
        return;
    }
    if (focal) {
        thread_reference(main);
    }
    simple_lock(&switch_recorder_watch_lock, LCK_GRP_NULL);
    place = focal ? switch_focal_enter(&switch_recorder_focal, key, mach_absolute_time(), &evicted)
                  : switch_focal_leave(&switch_recorder_focal, key);
    if (place < SWITCH_FOCAL_PLACES) {
        gone = switch_recorder_watch_put(SWITCH_RECORDER_WATCH_APP + place, focal ? main : THREAD_NULL);
    }
    simple_unlock(&switch_recorder_watch_lock);
    if (gone != THREAD_NULL) {
        thread_deallocate(gone);
    }
}

void switch_recorder_tick(uint64_t now)
{
    uint64_t tid[SWITCH_RECORDER_WATCHES] = {0}, woke[SWITCH_RECORDER_WATCHES] = {0}, ms[SWITCH_RECORDER_WATCHES] = {0};
    uint64_t split[SWITCH_RECORDER_WATCHES][3] = {{0}};

    simple_lock(&switch_recorder_watch_lock, LCK_GRP_NULL);
    if (switch_recorder_watch_every == 0 || now - switch_recorder_watch_last < switch_recorder_watch_every) {
        simple_unlock(&switch_recorder_watch_lock);
        return;
    }
    switch_recorder_watch_last = now;
    for (unsigned int i = 0; i < SWITCH_RECORDER_WATCHES; i++) {
        thread_t t = switch_recorder_watch_thread[i];
        uint64_t since = 0, ns = 0;
        bool open = false;
        spl_t s;

        if (t == THREAD_NULL) {
            continue;
        }
        s = splsched();
        thread_lock(t);
        open = (t->state & TH_TERMINATE) == 0 && switch_sched_busy_open(t, &since);
        if (open) {
            switch_sched_busy_split(t, &split[i][0], &split[i][1], &split[i][2]);
        }
        thread_unlock(t);
        splx(s);
        if (switch_recorder_watch_due(&switch_recorder_watch_state[i], open, since, now, switch_recorder_watch_bound)) {
            absolutetime_to_nanoseconds(now - since, &ns);
            tid[i] = thread_tid(t);
            woke[i] = since;
            ms[i] = ns / NSEC_PER_MSEC;
        }
    }
    simple_unlock(&switch_recorder_watch_lock);
    for (unsigned int i = 0; i < SWITCH_RECORDER_WATCHES; i++) {
        uint64_t split_ms[3] = {0};

        for (unsigned int k = 0; k < 3; k++) {
            absolutetime_to_nanoseconds(split[i][k], &split_ms[k]);
            split_ms[k] /= NSEC_PER_MSEC;
        }
        if (tid[i] == 0 ||
            !switch_recorder_hang(tid[i], ms[i], (uint32_t)split_ms[0], (uint32_t)split_ms[1], (uint32_t)split_ms[2])) {
            continue;
        }
        simple_lock(&switch_recorder_watch_lock, LCK_GRP_NULL);
        if (switch_recorder_watch_thread[i] != THREAD_NULL && thread_tid(switch_recorder_watch_thread[i]) == tid[i]) {
            switch_recorder_watch_fired(&switch_recorder_watch_state[i], woke[i]);
        }
        simple_unlock(&switch_recorder_watch_lock);
    }
}

#endif /* SWITCH_RECORDER */
