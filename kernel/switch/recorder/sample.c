// SPDX-License-Identifier: GPL-2.0-only
/*
 * The flight recorder's stacks (--//kernel:recorder): kperf's timer, armed at boot by kperf_init_early, samples the
 * thread each busy core runs every SWITCH_RECORDER_PERIOD_NS: its thread info, its kernel stack at the timer interrupt
 * and its user stack at its next return to user space, all into the kdebug ring (recorder.c). kperf's timer is a
 * running timer, so an idle core is never woken for a sample. No CPU counters are needed: the timer is the trigger.
 */

#include <switch/recorder.h>

#include <kern/cpu_number.h>
#include <kern/misc_protos.h>
#include <kperf/action.h>
#include <kperf/kperf.h>
#include <os/atomic_private.h>
#include <string.h>

#if SWITCH_RECORDER

#define SWITCH_RECORDER_PERIOD_NS 4000000ull /* of a core's time */
#define SWITCH_RECORDER_USTACK    64         /* user frames a sample walks with copyin, at the thread's AST */
#define SWITCH_RECORDER_KSTACK    32
#define SWITCH_RECORDER_CPUS      8

static bool switch_recorder_kperf_ours;
static uint64_t switch_recorder_cpu_samples[SWITCH_RECORDER_CPUS];

/* kperf_kernel_configure's form: samplers, '@', the period in ns. Apple's kperf= boot-arg wins, depths included. */
bool switch_recorder_kperf(bool found, char *config, size_t size)
{
    if (found && config[0] != '\0') {
        return true;
    }
    snprintf(config, size, "%u@%llu", (unsigned int)(SAMPLER_TH_INFO | SAMPLER_KSTACK | SAMPLER_USTACK),
             SWITCH_RECORDER_PERIOD_NS);
    switch_recorder_kperf_ours = true;
    return true;
}

void switch_recorder_kperf_configured(void)
{
    if (switch_recorder_kperf_ours) {
        (void)kperf_action_set_ucallstack_depth(1, SWITCH_RECORDER_USTACK);
        (void)kperf_action_set_kcallstack_depth(1, SWITCH_RECORDER_KSTACK);
    }
    switch_recorder_start();
}

/* The timer interrupt: this CPU's counter alone, so a plain increment with no atomic read-modify-write. */
void switch_recorder_sampled(void)
{
    const int cpu = cpu_number();

    if (cpu >= 0 && cpu < SWITCH_RECORDER_CPUS) {
        os_atomic_store(&switch_recorder_cpu_samples[cpu], switch_recorder_cpu_samples[cpu] + 1, relaxed);
    }
}

uint64_t switch_recorder_samples(void)
{
    uint64_t n = 0;

    for (unsigned int i = 0; i < SWITCH_RECORDER_CPUS; i++) {
        n += os_atomic_load(&switch_recorder_cpu_samples[i], relaxed);
    }
    return n;
}

#endif /* SWITCH_RECORDER */
