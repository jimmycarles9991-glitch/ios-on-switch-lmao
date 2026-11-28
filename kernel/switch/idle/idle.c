// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU idle states (<switch/idle.h>, kits/SwitchIdle). Each idle period either waits in WFI or, once ArmCpu has started
 * the platform half, powers the core down (C7) when kits/SwitchIdle/switch_idle_governor.h says the period is long
 * enough. A powered-down core returns through XNU's own deep-idle path: the PSCI entry, start.s resume_idle_cpu
 * (cpu_machine_idle_init made it the core's reset handler) and arm_init_idle_cpu, which calls switch_idle_resumed and
 * then cpu_idle_exit(TRUE) and Idle_load_context, back into machine_idle's caller. Nothing on the idle stack survives
 * or needs to: Idle_context saved the idle thread's registers in its machine state.
 */

#include <switch/idle.h>

#include <arm/cpu_data_internal.h>
#include <arm/rtclock.h>
#include <arm64/switch_smp.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <os/atomic_private.h>
#include <os/log.h>
#if XNU_12377_121_6 && CONFIG_CPU_COUNTERS
#include <kern/cpc.h>
#include <machine/machine_cpc.h>
#endif

#include "../../../kits/SwitchIdle/switch_idle_governor.h"

struct idle_core {
    uint64_t predicted;
    uint64_t entered;
    uint64_t wake_at;
    uint64_t lead;
    uint64_t enter_now;
    /* C7 requests the firmware refused and the last one's PSCI error. Only this core's idle path writes them (the count
     * last, release); idle_report reads them and keeps its own count of those it has logged (idle_logged). */
    uint64_t refused;
    int64_t refused_rc;
    uint32_t state;
    bool c7_seen;
#if DEVELOPMENT || DEBUG
    struct switch_idle_counts counts;
    bool from_c7; /* back from C7 and no idle entry since: the next frame line paid the exit */
#endif
} __attribute__((aligned(64)));

static struct idle_core idle_cores[SWITCH_SMP_CORE_COUNT];
static const struct switch_idle_platform *idle_platform;
static bool idle_c7;
static uint64_t idle_frame_at;
static void (*idle_frames_started)(void);
static thread_call_t idle_report_call;
static uint64_t idle_logged[SWITCH_SMP_CORE_COUNT];

static void idle_report(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    for (uint32_t cpu = 0; cpu < SWITCH_SMP_CORE_COUNT; cpu++) {
        const struct idle_core *core = &idle_cores[cpu];
        const uint64_t refused = os_atomic_load(&core->refused, acquire);
        if (refused != idle_logged[cpu]) {
            os_log_error(OS_LOG_DEFAULT,
                         "SwitchIdle: cpu%u: PSCI CPU_SUSPEND to C7 returned %lld, so that idle ran without C7 (%llu "
                         "refused since the last line); the next idle that chooses C7 asks again",
                         cpu, os_atomic_load(&core->refused_rc, relaxed), refused - idle_logged[cpu]);
            idle_logged[cpu] = refused;
        }
    }
}

/* The idle thread, interrupts masked: the line is logged from a thread call. */
static void idle_refused(struct idle_core *core, int64_t rc)
{
    os_atomic_store(&core->refused_rc, rc, relaxed);
    os_atomic_store(&core->refused, core->refused + 1, release);
    thread_call_enter(idle_report_call);
}

/* 12377's CPC_CPU_IDLE leaves the PMU counting, as WFI keeps its state; power-down loses it. The counts since the
 * last read are folded in and PMIs masked first. A core that powered down comes back through cpu_idle_exit(TRUE)'s
 * CPC_CPU_ACTIVE_COLD; one the firmware returned has its counters restarted here, since CPC_CPU_ACTIVE_WARM does
 * nothing. 10063's mt_cpu_idle already stops the counters on every idle. */
static void idle_pmu_save(struct cpu_data *cdp)
{
#if XNU_12377_121_6 && CONFIG_CPU_COUNTERS
    cpc_cpu_transition(CPC_CPU_OFFLINE, cdp);
#else
    (void)cdp;
#endif
}

static void idle_pmu_returned(struct cpu_data *cdp)
{
#if XNU_12377_121_6 && CONFIG_CPU_COUNTERS
    cpc_cpu_transition(CPC_CPU_ACTIVE_COLD, cdp);
#else
    (void)cdp;
#endif
}

static void idle_exit(struct idle_core *core, uint64_t now)
{
    const uint64_t idle = now - core->entered;
    core->predicted = switch_idle_learn(core->predicted, idle, switch_idle_timer_bound(now, core->wake_at));
#if DEVELOPMENT || DEBUG
    core->counts.residency[core->state] += idle;
    core->counts.entries[core->state]++;
#endif
}

void switch_idle_note_now(struct cpu_data *cdp, uint64_t now)
{
    const uint32_t cpu = (uint32_t)cdp->cpu_number;
    if (cpu < SWITCH_SMP_CORE_COUNT) {
        idle_cores[cpu].enter_now = now;
    }
}

void switch_idle_wfi(struct cpu_data *cdp)
{
    const uint32_t cpu = (uint32_t)cdp->cpu_number;
    if (__improbable(cpu >= SWITCH_SMP_CORE_COUNT)) {
        __builtin_arm_wfi();
        return;
    }
    struct idle_core *core = &idle_cores[cpu];
    const uint64_t now = core->enter_now ? core->enter_now : mach_absolute_time();
    core->enter_now = 0;
    const uint64_t deadline = cdp->rtcPop;
#if DEVELOPMENT || DEBUG
    core->from_c7 = false;
#endif
    core->entered = now;
    bool c7 = core->c7_seen;
    if (__improbable(!c7) && os_atomic_load(&idle_c7, relaxed)) {
        c7 = os_atomic_load(&idle_c7, acquire);
        core->c7_seen = c7;
    }
    core->state = c7 ? switch_idle_choose(core->predicted, now, deadline, os_atomic_load(&idle_frame_at, relaxed))
                     : SWITCH_IDLE_WFI;
    if (core->state == SWITCH_IDLE_C7) {
        core->wake_at = switch_idle_c7_wake_at(deadline, core->lead);
#if DEVELOPMENT || DEBUG
        const uint64_t pmu_from = mach_absolute_time();
        idle_pmu_save(cdp);
        core->counts.pmu_save_ticks += mach_absolute_time() - pmu_from;
        core->counts.pmu_saves++;
#else
        idle_pmu_save(cdp);
#endif
        const int64_t rc =
            idle_platform->power_down(cpu, switch_idle_timer_ticks(now, core->wake_at, idle_platform->wake_ticks_max));
        idle_pmu_returned(cdp);
        if (rc != 0) {
            idle_refused(core, rc);
        }
#if DEVELOPMENT || DEBUG
        else {
            core->counts.skipped++;
        }
#endif
    } else {
        core->wake_at = deadline;
        __builtin_arm_wfi();
    }
    idle_exit(core, mach_absolute_time());
}

void switch_idle_resumed(struct cpu_data *cdp)
{
    const uint32_t cpu = (uint32_t)cdp->cpu_number;
    switch_secondary_vectors_leave();
    idle_platform->resumed(cpu);
    ClearIdlePop(FALSE);
    struct idle_core *core = &idle_cores[cpu];
    const uint64_t now = mach_absolute_time();
    const uint64_t exit_latency = switch_idle_c7_exit(now, core->wake_at);
#if DEVELOPMENT || DEBUG
    if (exit_latency != 0) {
        core->counts.timer_exits++;
        core->counts.exit_ticks += exit_latency;
        if (exit_latency > core->counts.exit_max) {
            core->counts.exit_max = exit_latency;
        }
        if (exit_latency > core->lead) {
            core->counts.exits_late++;
        }
    }
    core->from_c7 = true;
#endif
    core->lead = switch_idle_c7_lead(core->lead, exit_latency);
    idle_exit(core, now);
}

void switch_idle_start(const struct switch_idle_platform *platform)
{
    idle_report_call =
        thread_call_allocate_with_options(idle_report, NULL, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
    idle_platform = platform;
    for (uint32_t cpu = 0; cpu < SWITCH_SMP_CORE_COUNT; cpu++) {
        idle_cores[cpu].lead = switch_idle_c7_lead(0, 0);
    }
    os_atomic_store(&idle_c7, true, release);
}

void switch_idle_note_frame(void)
{
#if DEVELOPMENT || DEBUG
    const uint32_t cpu = (uint32_t)getCpuDatap()->cpu_number;
    if (cpu < SWITCH_SMP_CORE_COUNT && idle_cores[cpu].from_c7) {
        idle_cores[cpu].from_c7 = false;
        idle_cores[cpu].counts.frame_wakes++;
    }
#endif
    /* Only the frame core (ArmGIC frameTarget) writes the stamp. While the frame lines move to it (a core coming
     * online, SC7) two cores may both read an old stamp and both report the start, which started() takes twice. */
    const uint64_t now = mach_absolute_time();
    const uint64_t before = os_atomic_load(&idle_frame_at, relaxed);
    os_atomic_store(&idle_frame_at, now, relaxed);
    if (now - before >= SWITCH_IDLE_FRAME_WINDOW) {
        void (*started)(void) = os_atomic_load(&idle_frames_started, acquire);
        if (started) {
            started();
        }
    }
}

uint64_t switch_idle_last_frame(void)
{
    return os_atomic_load(&idle_frame_at, relaxed);
}

void switch_idle_set_frames_started(void (*started)(void))
{
    os_atomic_store(&idle_frames_started, started, release);
}

int switch_idle_read_counts(uint32_t cpu, struct switch_idle_counts *out)
{
#if DEVELOPMENT || DEBUG
    if (cpu >= SWITCH_SMP_CORE_COUNT) {
        return -1;
    }
    *out = idle_cores[cpu].counts;
    return 0;
#else
    (void)cpu;
    (void)out;
    return -1;
#endif
}
