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

#include "stall.h"

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
#define STALL_MORE    2 /* owners followed past the first, each the one the owner before it waits for */

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
    bool rwlock;             /* a kernel rwlock wait */
    uint32_t rw_writer_ctid; /* that lock's exclusive owner by ctid (0: none, only readers hold it) */
};

/* An owner past the first: whom the one before it waits for, and its own state. */
struct stall_hop {
    struct switch_stall_owner owner;
    thread_t thread; /* +1 until switch_stall_release: the hop's thread, when the chain reached one */
    bool special_reply;
    bool locked;
    bool taken;
    bool on_core;
    struct stall_wait wait;
};

struct stall_snapshot {
    uint64_t taken;
    uint64_t tid;
    struct stall_wait wait;
    uint64_t pc, lr, fp;
    bool special_reply;
    bool locked; /* a lock on the way was held: the owner may be missing */
    struct switch_stall_owner owner;
    thread_t owner_thread; /* +1 between the two steps: the turnstile chain's end, when a thread */
    bool owner_taken;      /* owner_wait holds that thread's state */
    bool owner_on_core;
    struct stall_wait owner_wait;
    struct stall_hop more[STALL_MORE];
    unsigned int nmore;
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
/* Under t's thread lock, at splsched: record kernel rwlock writer ctid when t waits off core on one. */
void switch_stall_take_rwlock(thread_t t, struct stall_wait *w);
/* No thread lock held: the first owner's state under its own lock (no extra hops or rwlock read, for stall_frame.c). */
void switch_stall_take_owner(struct stall_snapshot *ss);
/* No thread lock held: the first owner and up to max_more further owners, reading rwlock writer ctids, one thread lock
 * at a time. Keeps references for switch_stall_release. */
void switch_stall_take_owner_chain(struct stall_snapshot *ss, unsigned int max_more);
/* On a thread call: the references the snapshot holds. */
void switch_stall_release(struct stall_snapshot *ss);
/* No thread lock held: t's state under its own lock and, when it waits, whom for (into next, unless NULL). False: it is
 * exiting. */
bool switch_stall_take_thread(thread_t t, bool *on_core, struct stall_wait *w, struct stall_hop *next,
                              bool read_rwlock);
/* stall_chain.c, under t's lock, t waiting on a kernel rwlock: the lock's exclusive owner by ctid, 0 for none. */
uint32_t switch_stall_rw_writer(thread_t t);
/* On a thread call (blocks while the other cores answer): each other core's thread now. */
void switch_stall_census(struct stall_cpu cpus[STALL_CPUS]);

bool switch_stall_on_core(thread_t t);
bool switch_stall_off_core_waiting(int state);

void switch_stall_put(struct stall_line *l, const char *fmt, ...) __printflike(2, 3);
/* "<wait> on <queue> 0x..., event 0x...; waits for <process>[pid] thread 0x... (<via>); it is on core | blocked: ..." */
void switch_stall_put_wait(struct stall_line *l, const struct stall_snapshot *ss);
/* " <wait> on <queue> 0x..., event 0x...;" */
void switch_stall_put_blocked(struct stall_line *l, const struct stall_wait *w, bool special);
/* "<label> continuation 0x...;" or "<label> 0x... 0x...;" (unslid) */
void switch_stall_put_kernel(struct stall_line *l, const char *label, const struct stall_wait *w);
/* " user pc ... lr ..., frames ..." (copyin: thread context) */
void switch_stall_put_address(struct stall_line *l, task_t task, const char *label, uint64_t pc);
void switch_stall_put_user(struct stall_line *l, const struct stall_snapshot *ss);
void switch_stall_put_census(struct stall_line *l, const struct stall_cpu cpus[STALL_CPUS]);
/* stall_chain.c, on a thread call: the owners past the first and each kernel rwlock's writer, with their state; false
 * when the chain ends at the first owner and no rwlock is in it. */
bool switch_stall_put_chain(struct stall_line *l, const struct stall_snapshot *ss);
/* "RT", "FX", "TS" or "-" for a thread's sched_mode */
const char *switch_stall_mode_name(int mode);

#endif /* SWITCH_DIAG_STALL_SNAPSHOT_H */
