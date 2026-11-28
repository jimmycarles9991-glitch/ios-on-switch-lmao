// SPDX-License-Identifier: GPL-2.0-only
#include <kern/clock.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kdp/kdp_callout.h>
#include <arm/cpu_data_internal.h>
#include <machine/machine_routines.h>
#include <os/log.h>

#include <switch/diag.h>
#include <switch/heartbeat.h>

#include "SwitchPowerTrail.h"
#include "heartbeat.h"

#if DEVELOPMENT || DEBUG

static volatile uint32_t *switch_heartbeat_tmr;
static thread_t switch_heartbeat_thread;
static bool switch_heartbeat_on;
static bool switch_heartbeat_forget;
static bool switch_heartbeat_told;
static uint32_t switch_heartbeat_holds;
static uint64_t switch_heartbeat_last_beat;
static unsigned switch_heartbeat_rearms;
static struct switch_heartbeat_core switch_heartbeat_cores[SWITCH_HEARTBEAT_CORES];
static char switch_heartbeat_cores_line[192];
static char switch_heartbeat_sched_line[512];
static char switch_heartbeat_arm_line[224];

static uint64_t switch_heartbeat_now(void)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}

static uint32_t switch_heartbeat_read(uint32_t offset)
{
    return switch_heartbeat_tmr[offset / 4];
}

static void switch_heartbeat_write(uint32_t offset, uint32_t value)
{
    switch_heartbeat_tmr[offset / 4] = value;
}

static void switch_heartbeat_drain(void)
{
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool switch_heartbeat_running(void)
{
    return __atomic_load_n(&switch_heartbeat_on, __ATOMIC_ACQUIRE) &&
           !__atomic_load_n(&switch_heartbeat_holds, __ATOMIC_ACQUIRE) && !panic_active();
}

static bool switch_heartbeat_programmed(void)
{
    return (switch_heartbeat_read(SWITCH_HEARTBEAT_WDT4_STS) & SWITCH_HEARTBEAT_STS_ENABLED) &&
           switch_heartbeat_read(SWITCH_HEARTBEAT_WDT4_CFG) == switch_heartbeat_wdt_config() &&
           switch_heartbeat_read(SWITCH_HEARTBEAT_TMR6_PTV) == switch_heartbeat_ptv();
}

/* CONFIG ignores writes while the counter is enabled. */
static bool switch_heartbeat_program(void)
{
    switch_heartbeat_write(SWITCH_HEARTBEAT_WDT4_UNLOCK, SWITCH_HEARTBEAT_UNLOCK);
    switch_heartbeat_write(SWITCH_HEARTBEAT_WDT4_CMD, SWITCH_HEARTBEAT_CMD_DISABLE);
    switch_heartbeat_write(SWITCH_HEARTBEAT_TMR6_PTV, switch_heartbeat_ptv());
    switch_heartbeat_write(SWITCH_HEARTBEAT_WDT4_CFG, switch_heartbeat_wdt_config());
    switch_heartbeat_write(SWITCH_HEARTBEAT_WDT4_CMD, SWITCH_HEARTBEAT_CMD_START);
    switch_heartbeat_drain();
    return switch_heartbeat_programmed();
}

/* Only START clears the interrupt status, and a command write clears the unlock: START, then unlock and disable. */
static void switch_heartbeat_stop(void)
{
    switch_heartbeat_write(SWITCH_HEARTBEAT_WDT4_CMD, SWITCH_HEARTBEAT_CMD_START);
    switch_heartbeat_write(SWITCH_HEARTBEAT_WDT4_UNLOCK, SWITCH_HEARTBEAT_UNLOCK);
    switch_heartbeat_write(SWITCH_HEARTBEAT_WDT4_CMD, SWITCH_HEARTBEAT_CMD_DISABLE);
    switch_heartbeat_write(SWITCH_HEARTBEAT_TMR6_PTV, 0);
    switch_heartbeat_drain();
    switch_power_trail_beat(0);
}

static void switch_heartbeat_restart(uint64_t now)
{
    if (switch_heartbeat_programmed()) {
        switch_heartbeat_write(SWITCH_HEARTBEAT_WDT4_CMD, SWITCH_HEARTBEAT_CMD_START);
        switch_heartbeat_drain();
    } else {
        const uint32_t status = switch_heartbeat_read(SWITCH_HEARTBEAT_WDT4_STS);
        const uint32_t config = switch_heartbeat_read(SWITCH_HEARTBEAT_WDT4_CFG);
        const uint32_t ptv = switch_heartbeat_read(SWITCH_HEARTBEAT_TMR6_PTV);
        ++switch_heartbeat_rearms;
        (void)switch_heartbeat_program();
        if (!__atomic_exchange_n(&switch_heartbeat_told, true, __ATOMIC_RELAXED)) {
            os_log(OS_LOG_DEFAULT,
                   "SwitchHeartbeat: a beat found WDT4 not running (status %#x, config %#x, TMR6 PTV %#x) and armed it "
                   "again; later ones are counted in a heartbeat panic",
                   status, config, ptv);
        }
    }
    __atomic_store_n(&switch_heartbeat_last_beat, now, __ATOMIC_RELEASE);
    switch_power_trail_beat(now);
}

static void switch_heartbeat_forget_cores(struct switch_heartbeat_core *cores)
{
    for (unsigned cpu = 0; cpu < SWITCH_HEARTBEAT_CORES; ++cpu) {
        cores[cpu].running = false;
    }
}

static unsigned switch_heartbeat_sample_cores(struct switch_heartbeat_core *cores, uint64_t now)
{
    unsigned count = 0;
    for (processor_t processor = processor_list; processor != NULL; processor = processor->processor_list) {
        const int cpu = processor->cpu_id;
        const cpu_data_t *cdp = cpu >= 0 && cpu < (int)SWITCH_HEARTBEAT_CORES ? cpu_datap(cpu) : NULL;
        if (cdp == NULL) {
            continue;
        }
        const processor_state_t state = processor->state;
        const bool running = state == PROCESSOR_RUNNING || state == PROCESSOR_DISPATCHING;
        switch_heartbeat_sample(&cores[cpu], running, __atomic_load_n(&cdp->cpu_stat.irq_ex_cnt, __ATOMIC_RELAXED),
                                now);
        if ((unsigned)cpu >= count) {
            count = (unsigned)cpu + 1;
        }
    }
    return count;
}

static void switch_heartbeat_describe_cores(const struct switch_heartbeat_core *cores, unsigned count, uint64_t now)
{
    char *out = switch_heartbeat_cores_line;
    const size_t size = sizeof(switch_heartbeat_cores_line);
    int used = 0;
    for (unsigned cpu = 0; cpu < count && used >= 0 && (size_t)used < size; ++cpu) {
        const struct switch_heartbeat_core *core = &cores[cpu];
        const char *sep = cpu ? ", " : "";
        if (switch_heartbeat_core_kind(core, now) == SWITCH_HEARTBEAT_CORE_MASKED) {
            const uint64_t quiet = now - core->quiet_since;
            used += snprintf(out + used, size - (size_t)used, "%scpu%u masked %llu.%llu s", sep, cpu,
                             quiet / NSEC_PER_SEC, quiet / (NSEC_PER_SEC / 10) % 10);
        } else {
            used += snprintf(out + used, size - (size_t)used, "%scpu%u %s", sep, cpu, core->running ? "alive" : "idle");
        }
    }
}

static void __attribute__((noreturn)) switch_heartbeat_panic(enum switch_heartbeat_class class,
                                                             const struct switch_heartbeat_core *cores, unsigned count,
                                                             uint64_t now)
{
    const uint64_t last = __atomic_load_n(&switch_heartbeat_last_beat, __ATOMIC_ACQUIRE);
    const uint64_t late = last <= now ? now - last : 0;
    switch_heartbeat_describe_cores(cores, count, now);
    switch_sched_describe(switch_heartbeat_sched_line, sizeof(switch_heartbeat_sched_line), switch_heartbeat_thread);
    if (class == SWITCH_HEARTBEAT_MASKED) {
        panic("SwitchHeartbeat: class a, a running core took no interrupt for %u.%02u s: %s; last beat %llu.%llu s "
              "ago; WDT4 rearmed %u times | heartbeat %s",
              SWITCH_HEARTBEAT_LATE_MS / 1000, SWITCH_HEARTBEAT_LATE_MS % 1000 / 10, switch_heartbeat_cores_line,
              late / NSEC_PER_SEC, late / (NSEC_PER_SEC / 10) % 10, switch_heartbeat_rearms,
              switch_heartbeat_sched_line);
    }
    panic("SwitchHeartbeat: class b, interrupts alive but no kernel thread scheduled for %llu.%llu s: %s; WDT4 "
          "rearmed %u times | heartbeat %s",
          late / NSEC_PER_SEC, late / (NSEC_PER_SEC / 10) % 10, switch_heartbeat_cores_line, switch_heartbeat_rearms,
          switch_heartbeat_sched_line);
}

static void switch_heartbeat_beat(void)
{
    if (!switch_heartbeat_running()) {
        return;
    }
    if (__atomic_exchange_n(&switch_heartbeat_forget, false, __ATOMIC_ACQ_REL)) {
        switch_heartbeat_forget_cores(switch_heartbeat_cores);
    }
    const uint64_t now = switch_heartbeat_now();
    const unsigned count = switch_heartbeat_sample_cores(switch_heartbeat_cores, now);
    if (switch_heartbeat_class(switch_heartbeat_cores, count, now, now) == SWITCH_HEARTBEAT_MASKED &&
        switch_heartbeat_running()) {
        switch_heartbeat_panic(SWITCH_HEARTBEAT_MASKED, switch_heartbeat_cores, count, now);
    }
    switch_heartbeat_restart(now);
    if (!switch_heartbeat_running()) {
        switch_heartbeat_stop();
    }
}

static void __dead2 switch_heartbeat_main(void *arg __unused, wait_result_t result __unused)
{
    uint64_t interval = 0;
    uint64_t leeway = 0;
    clock_interval_to_absolutetime_interval(SWITCH_HEARTBEAT_BEAT_S, NSEC_PER_SEC, &interval);
    clock_interval_to_absolutetime_interval(SWITCH_HEARTBEAT_LEEWAY_S, NSEC_PER_SEC, &leeway);
    for (;;) {
        switch_heartbeat_beat();
        assert_wait_deadline_with_leeway((event_t)&switch_heartbeat_thread, THREAD_UNINT, TIMEOUT_URGENCY_LEEWAY,
                                         mach_absolute_time() + interval, leeway);
        thread_block(THREAD_CONTINUE_NULL);
    }
}

void switch_heartbeat_hold(unsigned why)
{
    __atomic_fetch_or(&switch_heartbeat_holds, why, __ATOMIC_ACQ_REL);
    if (__atomic_load_n(&switch_heartbeat_on, __ATOMIC_ACQUIRE)) {
        switch_heartbeat_stop();
    }
}

/* The timer block may have lost power meanwhile (SC7). Only the beat thread writes the cores' samples: they span the
 * hold, so it starts them again, told before the hold ends. */
void switch_heartbeat_release(unsigned why)
{
    const uint32_t holds = __atomic_load_n(&switch_heartbeat_holds, __ATOMIC_RELAXED);
    if (!(holds & why)) {
        return;
    }
    const bool on = __atomic_load_n(&switch_heartbeat_on, __ATOMIC_ACQUIRE);
    if (on && !(holds & ~why)) {
        __atomic_store_n(&switch_heartbeat_forget, true, __ATOMIC_RELEASE);
        const uint64_t now = switch_heartbeat_now();
        __atomic_store_n(&switch_heartbeat_last_beat, now, __ATOMIC_RELEASE);
        (void)switch_heartbeat_program();
        switch_power_trail_beat(now);
    }
    __atomic_fetch_and(&switch_heartbeat_holds, ~why, __ATOMIC_ACQ_REL);
    if (on && !switch_heartbeat_running()) {
        switch_heartbeat_stop();
    }
}

static void switch_heartbeat_debugger(void *arg __unused, kdp_event_t event)
{
    if (event == KDP_EVENT_ENTER) {
        switch_heartbeat_hold(SWITCH_HEARTBEAT_HOLD_DEBUGGER);
    } else if (event == KDP_EVENT_EXIT) {
        switch_heartbeat_release(SWITCH_HEARTBEAT_HOLD_DEBUGGER);
    }
}

/* Judged on a copy: the beat thread may be sampling at the same time. */
void switch_heartbeat_interrupt(void)
{
    if (!__atomic_load_n(&switch_heartbeat_on, __ATOMIC_ACQUIRE) ||
        !(switch_heartbeat_read(SWITCH_HEARTBEAT_WDT4_STS) & SWITCH_HEARTBEAT_STS_INT)) {
        return;
    }
    if (!switch_heartbeat_running()) {
        switch_heartbeat_stop();
        return;
    }
    struct switch_heartbeat_core cores[SWITCH_HEARTBEAT_CORES];
    for (unsigned cpu = 0; cpu < SWITCH_HEARTBEAT_CORES; ++cpu) {
        cores[cpu] = switch_heartbeat_cores[cpu];
    }
    if (__atomic_load_n(&switch_heartbeat_forget, __ATOMIC_ACQUIRE)) {
        switch_heartbeat_forget_cores(cores);
    }
    const uint64_t now = switch_heartbeat_now();
    const unsigned count = switch_heartbeat_sample_cores(cores, now);
    const enum switch_heartbeat_class class =
        switch_heartbeat_class(cores, count, now, __atomic_load_n(&switch_heartbeat_last_beat, __ATOMIC_ACQUIRE));
    if (class == SWITCH_HEARTBEAT_HEALTHY) {
        switch_heartbeat_write(SWITCH_HEARTBEAT_WDT4_CMD, SWITCH_HEARTBEAT_CMD_START);
        switch_heartbeat_drain();
        return;
    }
    switch_power_trail_beat_interrupt(now);
    switch_heartbeat_panic(class, cores, count, now);
}

static const char *switch_heartbeat_not_idle(void)
{
    const uint32_t tmr6 = switch_heartbeat_read(SWITCH_HEARTBEAT_TMR6_PTV);
    const uint32_t status = switch_heartbeat_read(SWITCH_HEARTBEAT_WDT4_STS);
    const uint32_t config = switch_heartbeat_read(SWITCH_HEARTBEAT_WDT4_CFG);
    const uint32_t shared = switch_heartbeat_read(SWITCH_HEARTBEAT_SHARED_STATUS);
    const uint32_t source = switch_heartbeat_ptv_offset(config & 0xfu);
    const uint32_t source_ptv = source == UINT32_MAX ? 0 : switch_heartbeat_read(source);
    const enum switch_heartbeat_hardware hardware = switch_heartbeat_hardware(tmr6, status, source_ptv, shared);
    if (hardware == SWITCH_HEARTBEAT_HW_BUSY) {
        snprintf(switch_heartbeat_arm_line, sizeof(switch_heartbeat_arm_line),
                 "heartbeat not armed: the timer block is not idle (TMR6 PTV %#x, WDT4 status %#x config %#x, its "
                 "TMR%u PTV %#x, watchdog interrupts %#x)",
                 tmr6, status, config, config & 0xfu, source_ptv, shared & SWITCH_HEARTBEAT_SHARED_WDTS);
        return switch_heartbeat_arm_line;
    }
    return hardware == SWITCH_HEARTBEAT_HW_DORMANT ? ", found enabled on a stopped timer" : "";
}

const char *switch_heartbeat_arm(bool *armed)
{
    *armed = false;
    if (__atomic_load_n(&switch_heartbeat_on, __ATOMIC_ACQUIRE)) {
        *armed = true;
        return "heartbeat armed";
    }
    switch_heartbeat_tmr = (volatile uint32_t *)ml_io_map((vm_offset_t)SWITCH_HEARTBEAT_TMR_PA, 0x1000);
    if (switch_heartbeat_tmr == NULL) {
        return "heartbeat not armed: the timer block is not mapped";
    }
    const char *found = switch_heartbeat_not_idle();
    if (found == switch_heartbeat_arm_line) {
        return found;
    }
    if (!switch_heartbeat_program()) {
        snprintf(switch_heartbeat_arm_line, sizeof(switch_heartbeat_arm_line),
                 "heartbeat not armed: WDT4 does not start (status %#x, config %#x, TMR6 PTV %#x)",
                 switch_heartbeat_read(SWITCH_HEARTBEAT_WDT4_STS), switch_heartbeat_read(SWITCH_HEARTBEAT_WDT4_CFG),
                 switch_heartbeat_read(SWITCH_HEARTBEAT_TMR6_PTV));
        switch_heartbeat_stop();
        return switch_heartbeat_arm_line;
    }
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start_priority(switch_heartbeat_main, NULL, BASEPRI_KERNEL, &thread) != KERN_SUCCESS) {
        switch_heartbeat_stop();
        return "heartbeat not armed: no thread";
    }
    thread_set_thread_name(thread, "switch_heartbeat");
    switch_heartbeat_thread = thread;
    kdp_register_callout(switch_heartbeat_debugger, NULL);
    __atomic_store_n(&switch_heartbeat_last_beat, switch_heartbeat_now(), __ATOMIC_RELEASE);
    __atomic_store_n(&switch_heartbeat_on, true, __ATOMIC_RELEASE);
    if (__atomic_load_n(&switch_heartbeat_holds, __ATOMIC_ACQUIRE)) {
        switch_heartbeat_stop();
    }
    *armed = true;
    snprintf(switch_heartbeat_arm_line, sizeof(switch_heartbeat_arm_line),
             "heartbeat armed (WDT4%s: its interrupt %u.%02u to %u s and its warm reset %u.%02u to %u s after the last "
             "beat)",
             found, SWITCH_HEARTBEAT_LATE_MS / 1000, SWITCH_HEARTBEAT_LATE_MS % 1000 / 10,
             SWITCH_HEARTBEAT_STALL_MS / 1000, (SWITCH_HEARTBEAT_RESET_MS - SWITCH_HEARTBEAT_TICK_US / 1000) / 1000,
             (SWITCH_HEARTBEAT_RESET_MS - SWITCH_HEARTBEAT_TICK_US / 1000) % 1000 / 10,
             SWITCH_HEARTBEAT_RESET_MS / 1000);
    return switch_heartbeat_arm_line;
}

#endif /* DEVELOPMENT || DEBUG */
