// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apple's blocking CPU limits, replayed beside the ones this port lifts (limit_shadow.h), debug kernels: one line with
 * each 10 s scheduling report naming, for each process name and limit, how often it would have blocked and for how
 * long.
 *
 * thread_set_cpulimit runs on the thread itself: a lifted limit claims or updates the thread's slot, any other call
 * (removing the limit, or one Apple's ledger applies) ends it, and so does thread_terminate_self. Each of those runs
 * with preemption off, so the thread stays on its CPU and no dispatch hook of its own runs meanwhile.
 */

#include <kern/misc_protos.h>
#include <switch/sched.h>
#include <switch/xnu.h>

#include <kern/clock.h>
#include <kern/cpu_data.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <os/log.h>
#include <pexpert/arm64/board_config.h>

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED

#define SWITCH_LIMIT_CPUS MAX_CPUS
#include "limit_shadow.h"

uint32_t switch_sched_limit_watching;
static struct switch_limit_table limits;

/* thread_set_cpulimit, on the thread itself, for a limit switch_cpulimit_lift lifted. */
void switch_sched_limit_shadow(uint8_t percentage, uint64_t interval_ns)
{
    thread_t thread = current_thread();
    struct switch_limit_key key = {
        .percentage = percentage,
        .interval_ms = (uint32_t)(interval_ns / NSEC_PER_MSEC),
    };

    switch_task_name(get_threadtask(thread), key.name, sizeof(key.name));
    nanoseconds_to_absolutetime(interval_ns, &key.interval);
    disable_preemption();
    switch_limit_requested(&limits, cpu_number(), thread->thread_id, &key, mach_absolute_time());
    os_atomic_or(&thread->switch_watch_mask, SWITCH_WATCH_LIMIT, relaxed);
    if (os_atomic_load(&limits.watching, relaxed) != 0) {
        os_atomic_store(&switch_sched_limit_watching, 1, relaxed);
    }
    enable_preemption();
}

/* thread_set_cpulimit for anything but a lifted limit, and thread_terminate_self: on the thread itself. */
void switch_sched_limit_ended(void)
{
    thread_t thread = current_thread();

    if ((os_atomic_load(&thread->switch_watch_mask, relaxed) & SWITCH_WATCH_LIMIT) == 0) {
        return;
    }
    disable_preemption();
    os_atomic_and(&thread->switch_watch_mask, (uint8_t)~SWITCH_WATCH_LIMIT, relaxed);
    switch_limit_ended(&limits, cpu_number(), thread->thread_id, mach_absolute_time());
    enable_preemption();
}

void switch_sched_limit_dispatched(thread_t thread, uint64_t now)
{
    switch_limit_dispatched(&limits, thread->thread_id, now);
}

void switch_sched_limit_departed(thread_t thread, uint64_t now)
{
    switch_limit_departed(&limits, cpu_number(), thread->thread_id, now);
}

void switch_sched_limit_report(char *line, size_t size, uint64_t window_ns)
{
    size_t at = 0;
    unsigned int watched = 0;

    if (os_atomic_load(&switch_sched_limit_watching, relaxed) == 0) {
        return;
    }
    at = (size_t)snprintf(line, size,
                          "SwitchSched CPU limits lifted (%llu ms; what Apple's limit would have done: blocks, ms "
                          "run past the budget):",
                          window_ns / NSEC_PER_MSEC);
    for (uint32_t i = 0; i < SWITCH_LIMIT_ROWS && at < size - 1; i++) {
        struct switch_limit_counts window;
        unsigned int threads = 0;
        uint64_t over_ns = 0;

        if (!switch_limit_window(&limits, i, &window, &threads)) {
            continue;
        }
        watched += threads;
        if (window.blocks == 0) {
            continue;
        }
        const struct switch_limit_key *key = &limits.rows[i].key;
        absolutetime_to_nanoseconds(window.over, &over_ns);
        const int n = snprintf(line + at, size - at, " %s %u%%/%u (%u threads now): %llu, %llu;", key->name,
                               key->percentage, key->interval_ms, threads, window.blocks, over_ns / NSEC_PER_MSEC);
        at = n < 0 ? at : MIN(at + (size_t)n, size - 1);
    }
    (void)snprintf(line + at, size - at, " %u threads watched, %u threads and %u process limits not", watched,
                   os_atomic_load(&limits.slots_dropped, relaxed), os_atomic_load(&limits.rows_dropped, relaxed));
    os_log(OS_LOG_DEFAULT, "%s", line);
}

#endif /* DEVELOPMENT || DEBUG */
