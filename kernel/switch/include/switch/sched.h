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

/* thread_invoke, under the incoming thread's lock: the scheduler's hot path, so one load while nothing is watched. */
static inline void switch_sched_on_core(thread_t thread, uint64_t now)
{
    if (__improbable(os_atomic_load(&switch_sched_watching, relaxed) != 0)) {
        switch_sched_dispatched(thread, now);
    }
}

#else /* DEVELOPMENT || DEBUG */

static inline void switch_sched_on_core(__unused thread_t thread, __unused uint64_t now)
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
