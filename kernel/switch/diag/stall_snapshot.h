// SPDX-License-Identifier: GPL-2.0-only
/*
 * One thread's scheduling state at a stall, for the UI main-thread reports (stall.c) and the display's frame stalls
 * (stall_frame.c), and its text (stall_snapshot.c). Debug kernels only.
 *
 * Taken in two steps: switch_stall_take under the thread's own lock (its wait, the kernel half of its stack, its user
 * registers and who it waits for), then switch_stall_take_owner with no thread lock held, under the owner's: whether
 * that thread is on a core and, off one, its own wait and kernel frames. The text is written later on a thread call
 * (switch_stall_put), since reading a user stack can fault.
 */
#ifndef SWITCH_DIAG_STALL_SNAPSHOT_H
#define SWITCH_DIAG_STALL_SNAPSHOT_H

#include <kern/kern_types.h>
#include <kern/task.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

#define STALL_KFRAMES 6
#define STALL_UFRAMES 8
#define STALL_LINE    900 /* under the 1 KiB os_log record */
#define STALL_CPUS    4
#define STALL_NAME    17

struct stall_owner {
    int pid;
    uint64_t tid; /* 0: a process, not a thread */
    const char *via;
};

/* A thread's wait as the scheduler holds it. */
struct stall_wait {
    int state;
    int sched_pri;
    unsigned int hint;
    int wq_type;
    uintptr_t wq_object;
    uint64_t event;
    uintptr_t continuation;
    uintptr_t kframes[STALL_KFRAMES];
    unsigned int nkframes;
};

struct stall_snapshot {
    uint64_t taken;
    uint64_t tid;
    struct stall_wait wait;
    uint64_t pc, lr, fp;
    bool special_reply;
    bool locked; /* a lock on the way was held: the owner may be missing */
    struct stall_owner owner;
    thread_t owner_thread; /* +1 between the two steps: the turnstile chain's end, when a thread */
    bool owner_taken;      /* owner_wait holds that thread's state */
    bool owner_on_core;
    struct stall_wait owner_wait;
    task_t task; /* +1 */
};

/* What each other core was running when a report found a thread runnable and not run. */
struct stall_cpu {
    bool seen;
    bool idle;
    int pid;
    uint64_t tid;
    int sched_pri;
    int mode;
    char process[STALL_NAME];
    char thread[STALL_NAME];
};

struct stall_line {
    char text[STALL_LINE];
    size_t at;
};

/* Under t's thread lock, at splsched. Takes ss->task +1 and, when the wait ends at a thread, ss->owner_thread +1. */
void switch_stall_take(thread_t t, struct stall_snapshot *ss);
/* No thread lock held: the owner's state under its own lock. Keeps the reference for switch_stall_release. */
void switch_stall_take_owner(struct stall_snapshot *ss);
/* On a thread call: the references the snapshot holds. */
void switch_stall_release(struct stall_snapshot *ss);
/* On a thread call (blocks while the other cores answer): each other core's thread now. */
void switch_stall_census(struct stall_cpu cpus[STALL_CPUS]);

bool switch_stall_on_core(thread_t t);
bool switch_stall_off_core_waiting(int state);

void switch_stall_put(struct stall_line *l, const char *fmt, ...) __printflike(2, 3);
/* "<wait> on <queue> 0x..., event 0x...; waits for <process>[pid] thread 0x... (<via>); it is on core | blocked: ..." */
void switch_stall_put_wait(struct stall_line *l, const struct stall_snapshot *ss);
/* "<label> continuation 0x...;" or "<label> 0x... 0x...;" (unslid) */
void switch_stall_put_kernel(struct stall_line *l, const char *label, const struct stall_wait *w);
/* " user pc ... lr ..., frames ..." (copyin: thread context) */
void switch_stall_put_user(struct stall_line *l, const struct stall_snapshot *ss);
void switch_stall_put_census(struct stall_line *l, const struct stall_cpu cpus[STALL_CPUS]);

#endif /* SWITCH_DIAG_STALL_SNAPSHOT_H */
