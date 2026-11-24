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

extern uint32_t switch_sched_watching;

void switch_sched_dispatched(thread_t thread, uint64_t now);
void switch_sched_named(thread_t thread, const char *name);
void switch_sched_role(task_t task, int role);
void switch_sched_limit_block(thread_t thread, uint64_t until);
void switch_sched_limit_asked(uint8_t percentage, uint64_t interval_ns, bool lifted);
void switch_sched_tick(uint64_t now);
void switch_sched_boot_complete(void);

/* render.c: Core Animation's display thread from each vsync to its swap (sched/render.h). The display and the GPU driver
 * declare the last three themselves, with weak empty definitions for a release kernel. */
void switch_sched_render_on_core(thread_t thread, uint64_t now);
void switch_sched_render_vsync(uint64_t stamp, uint64_t period);
void switch_sched_render_submitted(void);
void switch_sched_render_swapped(void);

/* cpu.c: each core's and each process's CPU in the report's window (`line`: its LINE-byte buffer), and a process's
 * exit (diag/exit.c), whose CPU since the last report would leave no row otherwise. */
void switch_sched_cpu_report(char *line, uint64_t window_ns);
void switch_sched_cpu_exited(task_t task);

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

/* busy.c: a thread's user PC or LR (diag/stall.h switch_stall_user_where's place and value), and a waiting thread's
 * wait (sched/busy.h); under the thread's lock or on the thread itself. */
struct switch_busy_wait;
uint8_t switch_sched_user_site(thread_t thread, bool lr, uint64_t *value);
void switch_sched_wait_of(thread_t thread, struct switch_busy_wait *w);

/* watch.c, watch_write.c: committing and touch-sending threads and the focal app's main thread (sched/watch.h);
 * the report lines take the report's buffer and its size. */
extern uint32_t switch_sched_watch_on;

void switch_sched_watch_departed(thread_t thread, uint64_t now);
void switch_sched_watch_dispatched(thread_t thread, uint64_t now);
void switch_sched_watch_commit(uint64_t now);
void switch_sched_watch_delivered(uint64_t report, int to_pid, uint64_t now);
void switch_sched_watch_render_server(uint64_t tid);
void switch_sched_watch_focal(task_t task, uint64_t tid);
void switch_sched_watch_display_woke(uint64_t now);
void switch_sched_watch_tick(void);
void switch_sched_watch_report(char *line, size_t size, uint64_t window_ns);

/* commit.c: the commit path's sides (sched/commit.h) and its report line. */
void switch_sched_commit_sent(uint64_t now);
bool switch_sched_commit_server(thread_t thread, uint64_t now, bool departing);
void switch_sched_commit_server_named(uint64_t tid);
void switch_sched_commit_report(char *line, size_t size, uint64_t window_ns);

/* thread_invoke, under the incoming thread's lock: the scheduler's hot path, so three loads while nothing is watched. */
static inline void switch_sched_on_core(thread_t thread, uint64_t now)
{
    if (__improbable(os_atomic_load(&switch_sched_watching, relaxed) != 0)) {
        switch_sched_dispatched(thread, now);
    }
    if (__improbable(os_atomic_load(&switch_sched_busy_watching, relaxed) != 0)) {
        switch_sched_busy_dispatched(thread, now);
    }
    if (__improbable(os_atomic_load(&switch_sched_watch_on, relaxed) != 0)) {
        switch_sched_watch_dispatched(thread, now);
    }
}

/* thread_dispatch, under the departing thread's lock, before its wait or requeue is applied. */
static inline void switch_sched_off_core(thread_t thread, uint64_t now)
{
    if (__improbable(os_atomic_load(&switch_sched_busy_watching, relaxed) != 0)) {
        switch_sched_busy_departed(thread, now);
    }
    if (__improbable(os_atomic_load(&switch_sched_watch_on, relaxed) != 0)) {
        switch_sched_watch_departed(thread, now);
    }
}

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
