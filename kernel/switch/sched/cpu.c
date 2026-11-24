// SPDX-License-Identifier: GPL-2.0-only
/*
 * Who used the CPU (cpu.h), debug kernels: two lines with each 10 s scheduling report (stats.c). "SwitchSched cores"
 * closes each core's books: its threads' time, and its idle thread's split into asleep, interrupts taken while idle and
 * the rest, with its sleeps. "SwitchSched tasks" names the processes that used the most user + system time, the sum over
 * every process, and what the processes that exited in the window used. Both lines carry the report's own cost.
 */

#include <kern/misc_protos.h>
#include <switch/idle.h>
#include <switch/sched.h>
#include <switch/xnu.h>

#include "cpu.h"

#include <kern/clock.h>
#include <kern/counter.h>
#include <kern/processor.h>
#include <kern/recount.h>
#include <kern/simple_lock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <os/log.h>
#include <string.h>

#if DEVELOPMENT || DEBUG

#define LINE              900u /* under the 1 KiB os_log record */
#define BOOT_TASK_LOG_MAX 128u

#if XNU_12377_121_6
extern int kernel_sysctlbyname(const char *name, void *oldp, size_t *oldlenp, void *newp, size_t newlen);
#endif

/* Exits on any core and the report both take a process's counts from the same rows. */
static SIMPLE_LOCK_DECLARE(tasks_lock, 0);
static struct switch_cpu_tasks cpu_tasks;
static struct switch_cpu_core cores_seen[SWITCH_CPU_CORES];

static uint64_t task_key(task_t task)
{
    const uint64_t uid = get_task_uniqueid(task);

    /* kernel_task and a task without a proc: keyed by pid instead, never 0 (the empty row) */
    return uid == 0 || uid == UINT64_MAX ? (1ull << 63) | (uint32_t)task_pid(task) : uid;
}

static void boot_task_exited(task_t task, const char *name, const struct recount_times_mach *times)
{
    static uint32_t logged;
    uint64_t now_ns = 0, user_ns = 0, sys_ns = 0;

    if (os_atomic_load(&logged, relaxed) >= BOOT_TASK_LOG_MAX ||
        os_atomic_inc_orig(&logged, relaxed) >= BOOT_TASK_LOG_MAX) {
        return;
    }
    absolutetime_to_nanoseconds(mach_absolute_time(), &now_ns);
    absolutetime_to_nanoseconds(times->rtm_user, &user_ns);
    absolutetime_to_nanoseconds(times->rtm_system, &sys_ns);
    os_log(OS_LOG_DEFAULT,
           "SwitchBootTask: %s[%d] exited at %llu.%03llu s of uptime: CPU %llu ms user + %llu ms "
           "system, %llu page-ins",
           name, task_pid(task), now_ns / NSEC_PER_SEC, now_ns % NSEC_PER_SEC / NSEC_PER_MSEC, user_ns / NSEC_PER_MSEC,
           sys_ns / NSEC_PER_MSEC, counter_load(&task->pageins));
}

void switch_sched_cpu_exited(task_t task)
{
    if (task == TASK_NULL) {
        return;
    }
    const struct recount_times_mach times = recount_task_times(task);
    char name[SWITCH_CPU_NAME];

    switch_task_name(task, name, sizeof(name));
    simple_lock(&tasks_lock, LCK_GRP_NULL);
    switch_cpu_exited(&cpu_tasks, task_key(task), task_pid(task), name, times.rtm_user, times.rtm_system);
    simple_unlock(&tasks_lock);
    if ((os_atomic_load(&switch_sched_busy_watching, relaxed) & SWITCH_SCHED_BUSY_SPRINGBOARD) == 0) {
        boot_task_exited(task, name, &times);
    }
}

static uint32_t walk_tasks(struct switch_cpu_task *top, uint64_t *all_user, uint64_t *all_sys)
{
    uint32_t processes = 0;
    task_t task = TASK_NULL;

    lck_mtx_lock(&tasks_threads_lock);
    queue_iterate(&tasks, task, task_t, tasks)
    {
        const struct recount_times_mach times = recount_task_times(task);
        uint64_t du = 0, ds = 0;
        int at = 0;

        simple_lock(&tasks_lock, LCK_GRP_NULL);
        switch_cpu_take(&cpu_tasks, task_key(task), times.rtm_user, times.rtm_system, &du, &ds);
        simple_unlock(&tasks_lock);
        processes++;
        *all_user += du;
        *all_sys += ds;
        at = switch_cpu_rank(top, du + ds);
        if (at >= 0) {
            struct switch_cpu_task t = {.pid = task_pid(task), .user = du, .sys = ds};

            switch_task_name(task, t.name, sizeof(t.name));
            switch_cpu_place(top, at, &t);
        }
    }
    lck_mtx_unlock(&tasks_threads_lock);
    return processes;
}

static uint32_t take_cores(struct switch_cpu_core *window)
{
    const uint32_t count = MIN((uint32_t)ml_get_max_cpu_number() + 1, SWITCH_CPU_CORES);

    for (uint32_t c = 0; c < count; c++) {
        const processor_t p = processor_array[c];
        struct recount_usage usage = {};
        struct switch_idle_counts idle = {0};
        struct switch_cpu_core now = {0};
        uint64_t idle_time = 0;

        if (p == PROCESSOR_NULL) {
            continue;
        }
        recount_processor_usage(&p->pr_recount, &usage, &idle_time);
        const struct recount_times_mach t = recount_usage_times_mach(&usage);
        now.threads = t.rtm_user + t.rtm_system;
        now.irq = p->pr_recount.rpr_interrupt_duration_mach;
        now.idle_irq = p->idle_thread != THREAD_NULL ? p->idle_thread->th_recount.rth_interrupt_duration_mach : 0;
        if (switch_idle_read_counts(c, &idle) == 0) {
            now.asleep = idle.residency[0] + idle.residency[1];
            now.sleeps = idle.entries[0] + idle.entries[1];
        }
        window[c] = (struct switch_cpu_core){
            now.threads - MIN(now.threads, cores_seen[c].threads),
            now.irq - MIN(now.irq, cores_seen[c].irq),
            now.idle_irq - MIN(now.idle_irq, cores_seen[c].idle_irq),
            now.asleep - MIN(now.asleep, cores_seen[c].asleep),
            now.sleeps - MIN(now.sleeps, cores_seen[c].sleeps),
        };
        cores_seen[c] = now;
    }
    return count;
}

/* kern.a64x's trap and page counts since the last report (iOS 27's translator; none on 17). */
static size_t append_a64x(char *line, size_t at)
{
#if XNU_12377_121_6
    static uint64_t a64x_seen[3];
    static const char *const names[3] = {"kern.a64x.traps", "kern.a64x.traps_unhandled", "kern.a64x.text_pages"};
    uint64_t now[3] = {0};

    for (unsigned int i = 0; i < 3; i++) {
        size_t len = sizeof(now[i]);

        if (kernel_sysctlbyname(names[i], &now[i], &len, NULL, 0) != 0) {
            return at;
        }
    }
    at = switch_cpu_put(LINE, at,
                        snprintf(line + at, LINE - at, "; a64x traps %llu, unhandled %llu, text pages %llu",
                                 now[0] - MIN(now[0], a64x_seen[0]), now[1] - MIN(now[1], a64x_seen[1]),
                                 now[2] - MIN(now[2], a64x_seen[2])));
    memcpy(a64x_seen, now, sizeof(now));
#else
    (void)line;
#endif
    return at;
}

void switch_sched_cpu_report(char *line, uint64_t window_ns)
{
    static bool primed;
    struct switch_cpu_core cores[SWITCH_CPU_CORES] = {{0}};
    struct switch_cpu_task top[SWITCH_CPU_TOP] = {{0}};
    struct switch_cpu_exits exits;
    mach_timebase_info_data_t tb;
    uint64_t window = 0, all_user = 0, all_sys = 0, cost_ns = 0;
    const uint64_t start = mach_absolute_time();
    uint32_t processes = 0, count = 0;
    size_t at = 0;

    clock_timebase_info(&tb);
    nanoseconds_to_absolutetime(window_ns, &window);
    count = take_cores(cores);
    processes = walk_tasks(top, &all_user, &all_sys);
    simple_lock(&tasks_lock, LCK_GRP_NULL);
    exits = switch_cpu_end_window(&cpu_tasks);
    simple_unlock(&tasks_lock);
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &cost_ns);
    if (!primed) {
        primed = true;
        os_log(OS_LOG_DEFAULT, "SwitchSched cores: first window, counts taken (%llu us)", cost_ns / NSEC_PER_USEC);
        return;
    }
    at = (size_t)snprintf(line, LINE,
                          "SwitchSched cores (%llu ms; ms: a core's threads, its idle thread split; %llu us "
                          "to take): ",
                          window_ns / NSEC_PER_MSEC, cost_ns / NSEC_PER_USEC);
    at = MIN(at, LINE - 1);
    at += switch_cpu_cores_format(line + at, LINE - at, cores, count, window, tb.numer, tb.denom);
    at = append_a64x(line, at);
    os_log(OS_LOG_DEFAULT, "%s", line);

    at = (size_t)snprintf(line, LINE,
                          "SwitchSched tasks (%llu ms; ms user+system, top %u by both): ", window_ns / NSEC_PER_MSEC,
                          SWITCH_CPU_TOP);
    at = MIN(at, LINE - 1);
    switch_cpu_tasks_format(line + at, LINE - at, top, processes, all_user, all_sys, &exits, tb.numer, tb.denom);
    os_log(OS_LOG_DEFAULT, "%s", line);
}

#endif /* DEVELOPMENT || DEBUG */
