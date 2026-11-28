// SPDX-License-Identifier: GPL-2.0-only
/*
 * User threads' kernel time by what entered the kernel (systime.h), debug kernels: one line with each 10 s scheduling
 * report (stats.c), and one on what the PMU costs and whether its overflow interrupts arrive. Each CPU's counts have one writer, that CPU: sleh_synchronous counts an entry before it enables
 * interrupts, and recount absorbs a slice with interrupts masked. The report reads every CPU's counts from its own
 * thread, so a count it reads may miss the slice being added at that moment; the next window has it.
 */

#include <kern/misc_protos.h>
#include <switch/sched.h>

#include "systime.h"

#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/arm/thread_status.h>
#include <machine/machine_routines.h>
#include <os/atomic_private.h>
#include <os/log.h>
#include <pexpert/arm64/board_config.h>
#include <arm/cpu_data_internal.h>
#include <arm64/switch_smp.h>
#include <switch/idle.h>
#include <switch/pmuv3.h>

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED

struct systime_cpu {
    struct switch_systime counts;
    uint64_t returns; /* to EL0, from a trap or an interrupt */
} __attribute__((aligned(64)));

static struct systime_cpu systime_cpu[MAX_CPUS];
static struct switch_systime systime_seen;
static uint64_t returns_seen;

static enum switch_systime_bucket bucket_of(thread_t thread)
{
    const uint16_t code = thread->machine.exception_trace_code;
    int64_t svc = 0;

    if ((code & SWITCH_SYSTIME_TRACE_EC) == SWITCH_SYSTIME_EC_SVC_64 && thread->machine.upcb != NULL) {
        svc = (int64_t)(int)get_saved_state_svc_number(thread->machine.upcb);
    }
    return switch_systime_trace_bucket(code, svc);
}

static void add(uint64_t *count, uint64_t n)
{
    os_atomic_store(count, os_atomic_load(count, relaxed) + n, relaxed);
}

void switch_systime_entered(thread_t thread)
{
    const enum switch_systime_bucket bucket = bucket_of(thread);

    add(&systime_cpu[cpu_number()].counts.entries[bucket], 1);
    thread->machine.switch_systime_trap = (uint8_t)(bucket + 1);
}

void switch_systime_absorb(thread_t thread, task_t task, uint64_t mach)
{
    const uint8_t trap = thread->machine.switch_systime_trap;

    if (task == kernel_task || mach == 0) {
        return;
    }
    add(&systime_cpu[cpu_number()].counts.time[trap ? trap - 1 : SWITCH_SYSTIME_OUTSIDE], mach);
}

void switch_systime_returned(thread_t thread)
{
    thread->machine.switch_systime_trap = 0;
    add(&systime_cpu[cpu_number()].returns, 1);
}

/*
 * What precise user/kernel PMC accounting costs (CONFIG_PERVASIVE_CPI): recount reads the cycle counter and
 * INST_RETIRED at every user/kernel transition beside the timestamp. The two reads timed against a timestamp read,
 * interrupts masked, in ticks per 64 transitions.
 */
static uint64_t pmc_read_ticks(void)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    uint64_t sink = 0;
    const uint64_t t0 = mach_absolute_time();
    for (unsigned int i = 0; i < 64; i++) {
        sink += switch_pmuv3_read_pmccntr() + switch_pmuv3_read_evcntr(0);
    }
    const uint64_t t1 = mach_absolute_time();
    for (unsigned int i = 0; i < 64; i++) {
        sink += mach_absolute_time();
    }
    const uint64_t t2 = mach_absolute_time();
    ml_set_interrupts_enabled(enabled);
    __asm__ volatile("" ::"r"(sink));
    return (t1 - t0) > (t2 - t1) ? (t1 - t0) - (t2 - t1) : 0;
}

/*
 * Each CPU's PMIs (cpu_stat.pmi_cnt_wake, both trees' PMI handlers) beside its C7 entries: cpc_hw_configure re-bases
 * the idle counters at every C7 exit (INST_RETIRED to 2^31), so a PMI needs 2^31 instructions on a core between two
 * C7s. A core with no C7 in a busy window and no PMI says the overflow interrupt does not arrive. Then the C7 PMU
 * saves' cost and the precise reads'.
 */
static void report_pmu(char *line, size_t size, uint64_t window_ns, uint64_t transitions)
{
    static uint64_t pmis_seen[MAX_CPUS], saves_seen[SWITCH_SMP_CORE_COUNT], save_ticks_seen[SWITCH_SMP_CORE_COUNT];
    static uint64_t cached_read_ticks;
    static bool read_ticks_measured;
    const unsigned int cpus = MIN((unsigned int)ml_get_max_cpu_number() + 1, (unsigned int)MAX_CPUS);
    uint64_t saves = 0, save_ticks = 0, ns = 0;
    if (!read_ticks_measured) {
        cached_read_ticks = pmc_read_ticks();
        read_ticks_measured = true;
    }
    const uint64_t read_ticks = cached_read_ticks;
    size_t at = switch_systime_put(
        size, 0, snprintf(line, size, "SwitchSched pmu (%llu ms): PMIs (C7 entries)", window_ns / NSEC_PER_MSEC));

    for (unsigned int cpu = 0; cpu < cpus; cpu++) {
        const cpu_data_t *cdp = cpu_datap((int)cpu);
        const uint64_t pmis = cdp != NULL ? os_atomic_load(&cdp->cpu_stat.pmi_cnt_wake, relaxed) : 0;
        struct switch_idle_counts c = {0};
        uint64_t core_saves = 0;

        if (cpu < SWITCH_SMP_CORE_COUNT && switch_idle_read_counts(cpu, &c) == 0) {
            core_saves = c.pmu_saves - MIN(c.pmu_saves, saves_seen[cpu]);
            saves += core_saves;
            save_ticks += c.pmu_save_ticks - MIN(c.pmu_save_ticks, save_ticks_seen[cpu]);
            saves_seen[cpu] = c.pmu_saves;
            save_ticks_seen[cpu] = c.pmu_save_ticks;
        }
        at = switch_systime_put(
            size, at,
            snprintf(line + at, size - at, " cpu%u %llu (%llu)", cpu, pmis - MIN(pmis, pmis_seen[cpu]), core_saves));
        pmis_seen[cpu] = pmis;
    }
    absolutetime_to_nanoseconds(save_ticks, &ns);
    at = switch_systime_put(size, at,
                            snprintf(line + at, size - at, "; C7 PMU save %llu ns each", saves ? ns / saves : 0));
    absolutetime_to_nanoseconds(read_ticks, &ns);
    (void)snprintf(
        line + at, size - at,
        "; precise transition reads %llu ns each beyond the timestamp, %llu user/kernel transitions (%llu us)", ns / 64,
        transitions, ns * transitions / 64 / NSEC_PER_USEC);
    os_log(OS_LOG_DEFAULT, "%s", line);
}

void switch_sched_systime_report(char *line, size_t size, uint64_t window_ns)
{
    struct switch_systime now = {{0}};
    struct switch_systime window = {{0}};
    mach_timebase_info_data_t tb;
    size_t at = 0;

    for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++) {
        for (unsigned int b = 0; b < SWITCH_SYSTIME_BUCKETS; b++) {
            now.time[b] += os_atomic_load(&systime_cpu[cpu].counts.time[b], relaxed);
            now.entries[b] += os_atomic_load(&systime_cpu[cpu].counts.entries[b], relaxed);
        }
    }
    for (unsigned int b = 0; b < SWITCH_SYSTIME_BUCKETS; b++) {
        window.time[b] = now.time[b] - MIN(now.time[b], systime_seen.time[b]);
        window.entries[b] = now.entries[b] - MIN(now.entries[b], systime_seen.entries[b]);
    }
    systime_seen = now;
    clock_timebase_info(&tb);
    at = switch_systime_put(size, 0,
                            snprintf(line, size,
                                     "SwitchSched system (%llu ms; user threads' kernel time by what entered the "
                                     "kernel, with entries): ",
                                     window_ns / NSEC_PER_MSEC));
    switch_systime_format(line + at, size - at, &window, tb.numer, tb.denom);
    os_log(OS_LOG_DEFAULT, "%s", line);
    uint64_t returns = 0;
    for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++) {
        returns += os_atomic_load(&systime_cpu[cpu].returns, relaxed);
    }
    const uint64_t window_returns = returns - MIN(returns, returns_seen);
    returns_seen = returns;
    report_pmu(line, size, window_ns, 2 * window_returns);
}

#endif /* DEVELOPMENT || DEBUG */
