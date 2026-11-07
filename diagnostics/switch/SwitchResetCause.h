// SPDX-License-Identifier: BSD-2-Clause-Patent
// Why the last boot ended, from the PMC's reset status and SwitchPower's thermal shutdown record, as SwitchPanicSave.c
// logs and files it at every boot. Plain C for the host test.
//
// PMC_RST_STATUS (APBDEV_PMC + 0x1b4) bits 2:0 on T210 (L4T pmc.c tegra_pmc_get_system_reset_reason, legacy layout):
// 0 power-on (a cold start, also after a MAX77620 power-off), 1 the SoC watchdog, 2 SOC_THERM's THERMTRIP, 3 a software
// main reset (PMC_CNTRL MAIN_RST: Restart, a panic's reset, hekate), 4 LP0/SC7 exit, 5 AOTAG.
#ifndef SWITCH_RESET_CAUSE_H
#define SWITCH_RESET_CAUSE_H

#include <stddef.h>
#include <stdint.h>

enum switch_reset_cause {
    SWITCH_RESET_POWER_ON = 0,
    SWITCH_RESET_WATCHDOG = 1,
    SWITCH_RESET_THERMTRIP = 2,
    SWITCH_RESET_SOFTWARE = 3,
    SWITCH_RESET_LP0 = 4,
    SWITCH_RESET_AOTAG = 5,
    SWITCH_RESET_UNKNOWN = 6,
};

// The sensor a software thermal shutdown acted on (SwitchThermalRecord.h).
enum switch_thermal_sensor {
    SWITCH_THERMAL_NONE = 0,
    SWITCH_THERMAL_SOC_DIODE = 1,
    SWITCH_THERMAL_PLLX = 2,
};

static inline enum switch_reset_cause switch_reset_cause(uint32_t pmc_rst_status)
{
    const uint32_t source = pmc_rst_status & 7u;
    return source <= SWITCH_RESET_AOTAG ? (enum switch_reset_cause)source : SWITCH_RESET_UNKNOWN;
}

static inline const char *switch_reset_cause_name(enum switch_reset_cause cause)
{
    static const char *const names[] = {
        "power-on", "SoC watchdog", "thermal trip (THERMTRIP)", "software reset", "SC7 exit", "AOTAG", "unknown"};
    return names[cause <= SWITCH_RESET_UNKNOWN ? cause : SWITCH_RESET_UNKNOWN];
}

static inline const char *switch_thermal_sensor_name(enum switch_thermal_sensor sensor)
{
    switch (sensor) {
    case SWITCH_THERMAL_SOC_DIODE:
        return "SoC diode (TMP451)";
    case SWITCH_THERMAL_PLLX:
        return "PLLX (SOC_THERM)";
    default:
        return "none";
    }
}

// Whether the last boot's end is filed as a report: a hang the watchdog reset, a hardware thermal trip, or a software
// thermal shutdown, unless a saved panic already tells the story.
static inline int switch_reset_files_report(enum switch_reset_cause cause, int panic_saved,
                                            enum switch_thermal_sensor thermal)
{
    if (panic_saved) {
        return 0;
    }
    return thermal != SWITCH_THERMAL_NONE || cause == SWITCH_RESET_WATCHDOG || cause == SWITCH_RESET_THERMTRIP;
}

#endif
