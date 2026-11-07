// SPDX-License-Identifier: BSD-2-Clause-Patent
// The panic screen and the persistent panic record (SwitchPanicSave.c). Hooks, in the order they run:
//   arm_init.c, after switch_diag_init       switch_panic_save_init
//   pe_init.c check_for_panic_log (no /pram)   switch_panic_save_xnu_buffer
//   SwitchDiag.c switch_diag_panic         switch_panic_begin     (panic entry, other CPUs still running)
//   debug.c handle_debugger_trap               switch_panic_halted    (other CPUs halted)
//   debug.c, after SavePanicInfo               switch_panic_logged    (XNU's panic log written; may not return)
//   SwitchPMIC.cpp's start                     switch_panic_save_thermal
//   kern_exec.c, once Data is mounted          switch_panic_save_pending, _pending_stackshot, switch_panic_save_done
#ifndef SWITCH_PANIC_SAVE_H
#define SWITCH_PANIC_SAVE_H
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// SwitchPMIC.cpp at its start, thread context: the last boot's software thermal shutdown record (SwitchResetCause.h's
// switch_thermal_sensor, 0 for none, and its temperature), filed with the PMC's reset cause by
// switch_panic_save_pending. A no-op in kernels without the panic record.
void switch_panic_save_thermal(int sensor, int millic);
#ifdef __cplusplus
}
#endif

#if defined(SWITCH_XNU_DIAGNOSTICS) && SWITCH_XNU_DIAGNOSTICS
void switch_panic_save_init(void);
int switch_panic_save_xnu_buffer(uintptr_t *base, unsigned int *bytes);
int switch_panic_begin(const char *format, va_list *args, unsigned long caller, int take_screen);
void switch_panic_halted(void);
void switch_panic_logged(void);
const char *switch_panic_save_pending(uint32_t *bytes, char *path, size_t path_bytes);
const void *switch_panic_save_pending_stackshot(uint32_t *bytes);
void switch_panic_save_done(int error, const char *path);
// The SoC watchdog (TMR5/WDT0, a warm reset `seconds` from now; 0 off): SwitchPowerTrail.h switch_power_trail_watch.
void switch_panic_save_watchdog(unsigned seconds);
#else
#define switch_panic_save_init()                              ((void)0)
#define switch_panic_save_xnu_buffer(base, bytes)             0
#define switch_panic_begin(format, args, caller, take_screen) 0
#define switch_panic_halted()                                 ((void)0)
#define switch_panic_logged()                                 ((void)0)
#define switch_panic_save_pending(bytes, path, path_bytes)    ((const char *)0)
#define switch_panic_save_pending_stackshot(bytes)            ((const void *)0)
#define switch_panic_save_done(error, path)                   ((void)0)
#define switch_panic_save_watchdog(seconds)                   ((void)0)
#endif
#endif
