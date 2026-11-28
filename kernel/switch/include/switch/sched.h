// SPDX-License-Identifier: GPL-2.0-only
/*
 * Switch scheduling stats (kernel/switch/sched/stats.c): the events Apple's files report, one call each. Debug kernels only; a
 * release kernel compiles every call to nothing. switch_sched_cpulimit_lift is the exception: its scaling is in both.
 */
#ifndef SWITCH_SCHED_H
#define SWITCH_SCHED_H

#include <stdint.h>
#include <sys/cdefs.h>
#include <kern/kern_types.h>
#include <os/atomic_private.h>
#include <switch/cpulimit.h>

__BEGIN_DECLS

#if DEVELOPMENT || DEBUG

#define SWITCH_WATCH_MASK_SCHED (1u << 0)
#define SWITCH_WATCH_MASK_BUSY  (1u << 1)
#define SWITCH_WATCH_WATCH      (1u << 2)
#define SWITCH_WATCH_LIMIT      (1u << 3)

void switch_sched_named(thread_t thread, const char *name);
void switch_sched_role(task_t task, int role);
void switch_sched_boot_complete(void);

#if (defined(SWITCH_STATS_FRAME) && SWITCH_STATS_FRAME) || (defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED)
/* render.c: Core Animation's display thread from each vsync to its swap (sched/render.h). The display and the GPU driver
 * declare the last three themselves, with weak empty definitions for a release kernel. */
void switch_sched_render_on_core(thread_t thread, uint64_t now);
void switch_sched_render_vsync(uint64_t stamp, uint64_t period);
void switch_sched_render_submitted(void);
bool switch_sched_render_window(uint64_t *vsync, int64_t *woke_us, int64_t *first_us);
void switch_sched_render_swapped(void);
#endif

#if (defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED) || (defined(SWITCH_RECORDER) && SWITCH_RECORDER)
/* busy.c: SpringBoard's and backboardd's main threads between two waits in their run loops (sched/busy.h). */
#define SWITCH_SCHED_BUSY_WATCHING    (1u << 0)
#define SWITCH_SCHED_BUSY_SPRINGBOARD (1u << 1)
extern uint32_t switch_sched_busy_watching;

void switch_sched_busy_exec(thread_t thread);
void switch_sched_busy_departed(thread_t thread, uint64_t now);
void switch_sched_busy_dispatched(thread_t thread, uint64_t now);
void switch_sched_busy_tick(void);
void switch_sched_busy_focal(task_t task, int role, thread_t main);
bool switch_sched_busy_open(thread_t thread, uint64_t *woke);
void switch_sched_busy_split(thread_t thread, uint64_t *cpu, uint64_t *blocked, uint64_t *runnable);

/* busy.c: a thread's user PC or LR (diag/stall.h switch_stall_user_where's place and value), and a waiting thread's
 * wait (sched/busy.h); under the thread's lock or on the thread itself. */
struct switch_busy_wait;
uint8_t switch_sched_user_site(thread_t thread, bool lr, uint64_t *value);
void switch_sched_wait_of(thread_t thread, struct switch_busy_wait *w);
#else
static inline void switch_sched_busy_exec(__unused thread_t thread)
{}

static inline void switch_sched_busy_tick(void)
{}

static inline void switch_sched_busy_focal(__unused task_t task, __unused int role, __unused thread_t main)
{}
#endif

#if defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED

extern uint32_t switch_sched_watching;

void switch_sched_dispatched(thread_t thread, uint64_t now);
void switch_sched_limit_block(thread_t thread, uint64_t until);
void switch_sched_limit_asked(uint8_t percentage, uint64_t interval_ns, bool lifted);
void switch_sched_tick(uint64_t now);

/* cpu.c: each core's and each process's CPU in the report's window (`line`: its LINE-byte buffer), and a process's
 * exit (diag/exit.c), whose CPU since the last report would leave no row otherwise. */
void switch_sched_cpu_report(char *line, uint64_t window_ns);
void switch_sched_cpu_boot_totals(char *line, uint64_t boot_ns);
void switch_sched_cpu_exited(task_t task);

/* watch.c, watch_write.c: committing and touch-sending threads and the focal app's main thread (sched/watch.h);
 * the report lines take the report's buffer and its size. */
extern uint32_t switch_sched_watch_on;

void switch_sched_watch_departed(thread_t thread, uint64_t now);
void switch_sched_watch_dispatched(thread_t thread, uint64_t now);
void switch_sched_watch_commit(uint64_t now);
void switch_sched_watch_delivered(uint64_t report, int to_pid, uint64_t now);
void switch_sched_watch_render_server(thread_t thread);
void switch_sched_watch_focal(task_t task, thread_t thread);
void switch_sched_watch_exiting(thread_t thread);
void switch_sched_watch_display_woke(uint64_t now);
void switch_sched_watch_tick(void);
void switch_sched_watch_report(char *line, size_t size, uint64_t window_ns);

/* commit.c: the commit path's sides (sched/commit.h) and its report line. */
void switch_sched_commit_sent(uint64_t now);
bool switch_sched_commit_server(thread_t thread, uint64_t now, bool departing);
void switch_sched_commit_server_named(uint64_t tid);
void switch_sched_commit_report(char *line, size_t size, uint64_t window_ns);

/* systime.c: user threads' kernel time by what entered the kernel (sched/systime.h). */
void switch_systime_entered(thread_t thread);
void switch_systime_absorb(thread_t thread, task_t task, uint64_t mach);
void switch_systime_returned(thread_t thread);
void switch_sched_systime_report(char *line, size_t size, uint64_t window_ns);
/* callouts.c: thread call functions and timer actions by CPU (<switch/callouts.h>). */
void switch_sched_callout_report(char *line, size_t size, uint64_t window_ns);

/* limit_shadow.c: Apple's blocking CPU limits replayed beside the ones switch_cpulimit_lift lifts (sched/limit_shadow.h). */
extern uint32_t switch_sched_limit_watching;

void switch_sched_limit_shadow(uint8_t percentage, uint64_t interval_ns);
void switch_sched_limit_ended(void);
void switch_sched_limit_dispatched(thread_t thread, uint64_t now);
void switch_sched_limit_departed(thread_t thread, uint64_t now);
void switch_sched_limit_report(char *line, size_t size, uint64_t window_ns);

#else /* SWITCH_STATS_SCHED */

static inline void switch_sched_limit_block(__unused thread_t thread, __unused uint64_t until)
{}

static inline void switch_sched_limit_asked(__unused uint8_t percentage, __unused uint64_t interval_ns,
                                            __unused bool lifted)
{}

static inline void switch_sched_tick(__unused uint64_t now)
{}

static inline void switch_sched_cpu_exited(__unused task_t task)
{}

static inline void switch_sched_watch_commit(__unused uint64_t now)
{}

static inline void switch_sched_watch_delivered(__unused uint64_t report, __unused int to_pid, __unused uint64_t now)
{}

static inline void switch_sched_watch_exiting(__unused thread_t thread)
{}

static inline void switch_sched_watch_display_woke(__unused uint64_t now)
{}

static inline void switch_sched_watch_tick(void)
{}

static inline void switch_systime_entered(__unused thread_t thread)
{}

static inline void switch_systime_absorb(__unused thread_t thread, __unused task_t task, __unused uint64_t mach)
{}

static inline void switch_systime_returned(__unused thread_t thread)
{}

static inline void switch_sched_limit_ended(void)
{}

#endif /* SWITCH_STATS_SCHED */

#if (defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED) || (defined(SWITCH_RECORDER) && SWITCH_RECORDER)
#if defined(_KERN_THREAD_H_) && defined(MACH_KERNEL_PRIVATE)
/* thread_invoke, under the incoming thread's lock: one byte load on struct thread skips unwatched threads. */
static inline void switch_sched_on_core(thread_t thread, uint64_t now)
{
    const uint8_t mask = os_atomic_load(&thread->switch_watch_mask, relaxed);

    if (__improbable(mask != 0)) {
#if defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED
        if (mask & SWITCH_WATCH_MASK_SCHED) {
            switch_sched_dispatched(thread, now);
        }
#endif
        if (mask & SWITCH_WATCH_MASK_BUSY) {
            switch_sched_busy_dispatched(thread, now);
        }
#if defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED
        if (mask & SWITCH_WATCH_WATCH) {
            switch_sched_watch_dispatched(thread, now);
        }
        if (mask & SWITCH_WATCH_LIMIT) {
            switch_sched_limit_dispatched(thread, now);
        }
#endif
    }
}

/* thread_dispatch, under the departing thread's lock, before its wait or requeue is applied. */
static inline void switch_sched_off_core(thread_t thread, uint64_t now)
{
    const uint8_t mask = os_atomic_load(&thread->switch_watch_mask, relaxed);

    if (__improbable(mask != 0)) {
        if (mask & SWITCH_WATCH_MASK_BUSY) {
            switch_sched_busy_departed(thread, now);
        }
#if defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED
        if (mask & SWITCH_WATCH_WATCH) {
            switch_sched_watch_departed(thread, now);
        }
        if (mask & SWITCH_WATCH_LIMIT) {
            switch_sched_limit_departed(thread, now);
        }
#endif
    }
}
#endif /* _KERN_THREAD_H_ */
#else
static inline void switch_sched_on_core(__unused thread_t thread, __unused uint64_t now)
{}

static inline void switch_sched_off_core(__unused thread_t thread, __unused uint64_t now)
{}
#endif

#else /* DEVELOPMENT || DEBUG */

static inline void switch_sched_on_core(__unused thread_t thread, __unused uint64_t now)
{}

static inline void switch_sched_off_core(__unused thread_t thread, __unused uint64_t now)
{}

static inline void switch_sched_busy_exec(__unused thread_t thread)
{}

static inline void switch_sched_busy_tick(void)
{}

static inline void switch_sched_named(__unused thread_t thread, __unused const char *name)
{}

static inline void switch_sched_role(__unused task_t task, __unused int role)
{}

static inline void switch_sched_limit_block(__unused thread_t thread, __unused uint64_t until)
{}

static inline void switch_sched_limit_asked(__unused uint8_t percentage, __unused uint64_t interval_ns,
                                            __unused bool lifted)
{}

static inline void switch_sched_tick(__unused uint64_t now)
{}

static inline void switch_sched_boot_complete(void)
{}

static inline void switch_sched_cpu_exited(__unused task_t task)
{}

static inline void switch_sched_watch_exiting(__unused thread_t thread)
{}

static inline void switch_systime_entered(__unused thread_t thread)
{}

static inline void switch_systime_absorb(__unused thread_t thread, __unused task_t task, __unused uint64_t mach)
{}

static inline void switch_systime_returned(__unused thread_t thread)
{}

static inline void switch_sched_limit_ended(void)
{}

#endif /* DEVELOPMENT || DEBUG */

/* thread_set_cpulimit, on the thread itself: a blocking limit's share scaled to this CPU (<switch/cpulimit.h>); true
 * when none is left to apply. */
static inline bool switch_sched_cpulimit_lift(uint8_t *percentage, uint64_t interval_ns)
{
    const uint8_t asked = *percentage;
    const bool lifted = switch_cpulimit_lift(percentage);

    switch_sched_limit_asked(asked, interval_ns, lifted);
    return lifted;
}

__END_DECLS

#endif /* SWITCH_SCHED_H */
