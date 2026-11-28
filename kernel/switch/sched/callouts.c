// SPDX-License-Identifier: GPL-2.0-only
/*
 * Thread call functions by group, and timer actions, by CPU (callouts.h), debug kernels: one line with each 10 s
 * scheduling report. A hook runs with interrupts masked from its read of the thread's on-core time to its row update, which also
 * keeps it on the CPU whose table it writes, so each table has one writer.
 */

#include <kern/misc_protos.h>
#include <switch/callouts.h>
#include <switch/sched.h>

#include "callouts.h"

#include <kern/clock.h>
#include <kern/cpu_number.h>
#include <kern/recount.h>
#include <machine/machine_routines.h>
#include <mach/vm_param.h>
#include <os/log.h>
#include <pexpert/arm64/board_config.h>

#if (DEVELOPMENT || DEBUG) && defined(SWITCH_STATS_SCHED) && SWITCH_STATS_SCHED

static struct switch_callout_table callout_tables[MAX_CPUS] __attribute__((aligned(64)));
static struct switch_callout_sum callout_now, callout_seen, callout_window;

uint64_t switch_callout_begin(void)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const uint64_t t = recount_current_thread_time_mach();

    ml_set_interrupts_enabled(enabled);
    return t;
}

void switch_callout_end(const void *function, const char *group, uint64_t begin)
{
    const boolean_t enabled = ml_set_interrupts_enabled(FALSE);
    const uint64_t t = recount_current_thread_time_mach();

    switch_callout_add(&callout_tables[cpu_number()], (uint64_t)(uintptr_t)function, (uint64_t)(uintptr_t)group,
                       t - MIN(t, begin));
    ml_set_interrupts_enabled(enabled);
}

static uint64_t unslide(uint64_t address)
{
    return (uint64_t)VM_KERNEL_UNSLIDE((uintptr_t)address);
}

static const char *group_name(uint64_t group)
{
    return group != 0 ? (const char *)(uintptr_t)group : "timer actions";
}

void switch_sched_callout_report(char *line, size_t size, uint64_t window_ns)
{
    mach_timebase_info_data_t tb;
    size_t at = 0;

    clock_timebase_info(&tb);
    __builtin_memset(&callout_now, 0, sizeof(callout_now));
    for (unsigned int cpu = 0; cpu < MAX_CPUS; cpu++) {
        switch_callout_sum_table(&callout_now, &callout_tables[cpu]);
    }
    switch_callout_window(&callout_now, &callout_seen, &callout_window);
    at = switch_callout_put(size, 0,
                            snprintf(line, size, "SwitchSched callouts (%llu ms; on-core ms (calls), unslid): ",
                                     window_ns / NSEC_PER_MSEC));
    switch_callout_format(line + at, size - at, &callout_window, group_name, unslide, tb.numer, tb.denom);
    os_log(OS_LOG_DEFAULT, "%s", line);
}

#endif /* DEVELOPMENT || DEBUG */
