// SPDX-License-Identifier: GPL-2.0-only
/*
 * The freeze heartbeat (kernel/switch/diag/heartbeat.c, debug kernels): a kernel thread restarts the SoC's WDT4, and a
 * freeze ends in a "SwitchHeartbeat:" panic naming its class, or in WDT4's warm reset when no core takes its interrupt.
 * NativeSwitchWatchdog arms it when watchdogd connects and hands it WDT4's interrupt (COP_WD, SPI 124).
 * Holds stack: the power trail's watch holds it from a power step to the wake (shut down, restart, system sleep), the
 * kernel debugger for as long as it has the CPUs. Hold and release run in any context and never block.
 */
#ifndef SWITCH_HEARTBEAT_H
#define SWITCH_HEARTBEAT_H

#include <stdbool.h>
#include <sys/cdefs.h>

#define SWITCH_HEARTBEAT_INTID         (32u + 124u)
#define SWITCH_HEARTBEAT_HOLD_POWER    (1u << 0)
#define SWITCH_HEARTBEAT_HOLD_DEBUGGER (1u << 1)

__BEGIN_DECLS

#if DEVELOPMENT || DEBUG
/* Thread context, once: arms only a timer block found idle. What it did, for the caller's one line. */
const char *switch_heartbeat_arm(bool *armed);
/* Primary interrupt context: panics, or clears WDT4's interrupt and returns. */
void switch_heartbeat_interrupt(void);
void switch_heartbeat_hold(unsigned why);
void switch_heartbeat_release(unsigned why);
#else
static inline void switch_heartbeat_hold(__unused unsigned why)
{}

static inline void switch_heartbeat_release(__unused unsigned why)
{}
#endif

__END_DECLS

#endif
