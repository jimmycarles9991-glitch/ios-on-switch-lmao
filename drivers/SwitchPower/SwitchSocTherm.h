// SPDX-License-Identifier: BSD-2-Clause-Patent
// SOC_THERM's die temperatures for the thermal consumers (SwitchThermal's fan curve and DFLL limits, an
// AppleDieTempController stand-in), from one reader (SwitchSensors.cpp). Readings are the hardware's own conversion at the
// moment of the call; changes arrive as SOC_THERM's LVL0 threshold interrupts (THERMCTL_INTR, INTID 80), never by
// polling: each group's thresholds sit one degree either side of its last reported temperature, so every listener hears
// each degree the group moves.
#ifndef SWITCH_SOCTHERM_H
#define SWITCH_SOCTHERM_H

#ifdef __cplusplus
extern "C" {
#endif

// T210B01's thermal groups as SOC_THERM keeps them: CPU (the hottest of the four core sensors), GPU, PLLX (mid-die).
enum switch_soctherm_group {
    SWITCH_SOCTHERM_CPU = 0,
    SWITCH_SOCTHERM_GPU = 1,
    SWITCH_SOCTHERM_PLLX = 2,
    SWITCH_SOCTHERM_GROUPS = 3,
};

// The group's temperature now in m C: the hardware's current conversion, never older than what the last callback for
// the group carried, so a listener may read on its own schedule without racing the events. Any context. 0, or -1 while
// SOC_THERM is not up (before its bring-up, across SC7).
int switch_soctherm_read(enum switch_soctherm_group group, int *millic);

// Called on SOC_THERM's work loop when a group's temperature has moved a degree from the last one reported, with the new
// reading; once per group right after registering, and again after SC7. Must not block for long.
typedef void (*switch_soctherm_moved)(void *ctx, enum switch_soctherm_group group, int millic);

// At most four listeners, for the life of the system. Thread context. 0, or -1 when the table is full.
int switch_soctherm_listen(switch_soctherm_moved moved, void *ctx);

// SwitchSensors.cpp, thread context: SOC_THERM is up (`regs` mapped) and its windows are armed around the readings, or
// it is going down (nullptr: SC7 resets it, reads fail until the wake's bring-up arms it again).
void switch_soctherm_armed(volatile unsigned char *regs);

#ifdef __cplusplus
}
#endif

#endif
