// SPDX-License-Identifier: GPL-2.0-only
/*
 * CPU idle states (kernel/switch/idle/idle.c, kits/SwitchIdle): osfmk/arm64/cpu.c's cpu_idle idles through
 * switch_idle_wfi instead of a bare WFI, and osfmk/arm/arm_init.c's arm_init_idle_cpu calls switch_idle_resumed on a
 * core back from power-down, before cpu_idle_exit.
 */
#ifndef SWITCH_IDLE_KERNEL_H
#define SWITCH_IDLE_KERNEL_H

#include <sys/cdefs.h>

#include "../../../../kits/SwitchIdle/switch_idle.h"

struct cpu_data;

__BEGIN_DECLS

/* Interrupts masked, the idle thread's context saved (machine_idle's Idle_context). Returns after WFI, or after a C7
 * request the firmware returned from; a core that powered down comes back through switch_idle_resumed instead. */
void switch_idle_wfi(struct cpu_data *cdp);

/* arm_init_idle_cpu, after fiq_context_init: the entry vectors left, the platform's resume, the core's timer armed
 * again (its compare value did not survive power-down). */
void switch_idle_resumed(struct cpu_data *cdp);

__END_DECLS

#endif /* SWITCH_IDLE_KERNEL_H */
