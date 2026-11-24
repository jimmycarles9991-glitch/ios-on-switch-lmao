// SPDX-License-Identifier: BSD-2-Clause-Patent
// The freeze heartbeat's part of the panic record (SwitchPanicHeartbeat.c, debug kernels; <switch/heartbeat.h>):
// SwitchPanicSave.c's switch_panic_begin asks whether a panic is the heartbeat's, which is a watchdog panic, and its
// reset report whether the heartbeat's WDT4 ended the last boot. In release both answer no.
#ifndef SWITCH_PANIC_HEARTBEAT_H
#define SWITCH_PANIC_HEARTBEAT_H

#ifdef __cplusplus
extern "C" {
#endif

#if DEVELOPMENT || DEBUG
int switch_panic_heartbeat_watchdog(const char *panic_line);
// For a watchdog reset with the heartbeat beating at the end of the last boot: what the heartbeat saw; else NULL.
const char *switch_panic_heartbeat_story(int watchdog_reset);
#else
#define switch_panic_heartbeat_watchdog(panic_line)  ((void)sizeof(panic_line), 0)
#define switch_panic_heartbeat_story(watchdog_reset) ((void)sizeof(watchdog_reset), (const char *)0)
#endif

#ifdef __cplusplus
}
#endif
#endif
