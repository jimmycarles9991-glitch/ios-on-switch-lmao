// SPDX-License-Identifier: BSD-2-Clause-Patent
/*
 * CPU idle states of the Switch's four Cortex-A57 cores, shared by XNU's idle path (kernel/switch/idle/idle.c, behind
 * the cpu_idle and arm_init_idle_cpu hooks) and the drivers that provide the platform half (ArmCpu) or feed the choice
 * (ArmGIC).
 *
 * C7 is the T210 per-core power-down: PSCI CPU_SUSPEND with power state 0x40000007 (switchroot ATF
 * plat_psci_handlers.c PSTATE_ID_CORE_POWERDN, extended StateID format; the flow controller gates the core once it is
 * in WFI). The core loses all its state and comes back through the PSCI entry and XNU's own reset path (start.s
 * resume_idle_cpu -> arm_init_idle_cpu), the path Apple's cores take out of their power-down idle.
 */
#ifndef SWITCH_IDLE_H
#define SWITCH_IDLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The platform half (drivers/ArmCpu). Both run on the idle core with interrupts masked, and only on the C7 path. */
struct switch_idle_platform {
    /* Arms the core's wake timer `ticks` timebase ticks from now (1..wake_ticks_max) and asks the firmware to power
     * the core down. Returns only when it did not: 0 when an interrupt was already pending, else the PSCI error. The
     * wake timer is disarmed before it returns. */
    int64_t (*power_down)(uint32_t cpu, uint64_t ticks);
    /* On the core back from power-down, before its interrupts are unmasked: the core's banked GIC state that the
     * firmware's warm boot reset is set back, and the wake timer disarmed. */
    void (*resumed)(uint32_t cpu);
    uint64_t wake_ticks_max;
};

/* ArmCpu, once every core runs and the platform is verified: each core uses C7 from its next idle on. Thread context,
 * once. `platform` must stay valid for the life of the system. */
void switch_idle_start(const struct switch_idle_platform *platform);

/* ArmGIC, primary interrupt context, any core: a frame line (display, GPU fences, host1x) fired. */
void switch_idle_note_frame(void);

/* When a frame line last fired (timebase ticks; 0: never), and, for the EMC governor, a function run in primary
 * interrupt context when one fires after none for 100 ms (frames start again). */
uint64_t switch_idle_last_frame(void);
void switch_idle_set_frames_started(void (*started)(void));  // NOLINT(modernize-redundant-void-arg): C too (gpu_exec.c)

/* TegraGM20B and TegraHost1x (NVDEC, VIC, NVENC, NVJPG), any context, before a channel's doorbell: the EMC governor
 * (drivers/SwitchPower/SwitchEMC.cpp) raises the memory clock ahead of the engine's traffic. One atomic load when it
 * is already up; never blocks. */
void switch_emc_gpu_submit(void);

/* Debug kernels: time in each state (timebase ticks), entries, and C7 requests the firmware returned from at once,
 * since boot; frame lines (switch_idle_note_frame) that were the first interrupt the core took back from C7, so paid
 * its exit; and the exits its wake timer ended (switch_idle_c7_exit: count, sum and largest in ticks, and how many came
 * back after the deadline). Returns 0, or -1 for a core that does not exist. */
struct switch_idle_counts {
    uint64_t residency[2], entries[2], skipped;
    uint64_t frame_wakes;
    uint64_t timer_exits, exit_ticks, exit_max, exits_late;
};

int switch_idle_read_counts(uint32_t cpu, struct switch_idle_counts *out);

#ifdef __cplusplus
}
#endif

#endif /* SWITCH_IDLE_H */
