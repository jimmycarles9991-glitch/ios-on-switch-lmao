// SPDX-License-Identifier: BSD-2-Clause-Patent
// SOC_THERM's LVL0 thresholds as a window around a group's temperature (SwitchSocTherm.cpp; L4T soctherm.c
// tegra_thermctl_set_trips and tegra210-soctherm.c's T210 masks). Plain C for the host test.
//
// THERMCTL_LEVEL0_GROUP_<group>: UP threshold bits 26:18 and DN threshold bits 17:9 in 0.5 C steps (TEGRA210_THRESH_GRAIN),
// EN bit 8, the throttle fields below it. THERMCTL_INTR_*: two bits per group (down, up): PLLX ("TSENSE") 1:0, CPU 9:8,
// GPU 17:16. The group temperatures are SENSOR_TEMP1 (CPU 31:16, GPU 15:0) and SENSOR_TEMP2 (PLLX 15:0), in the tsensors'
// readback format (SwitchSensors.h switch_soctherm_millic).
#ifndef SWITCH_SOCTHERM_WINDOW_H
#define SWITCH_SOCTHERM_WINDOW_H

#include <stdint.h>
#include "SwitchThermalLimits.h"

#define SWITCH_SOCTHERM_THERMCTL_INTR_STATUS  0x84u
#define SWITCH_SOCTHERM_THERMCTL_INTR_EN      0x88u
#define SWITCH_SOCTHERM_THERMCTL_INTR_DISABLE 0x8cu
#define SWITCH_SOCTHERM_SENSOR_TEMP1          0x1c8u
#define SWITCH_SOCTHERM_SENSOR_TEMP2          0x1ccu

#define SWITCH_SOCTHERM_GRAIN_MILLIC  500
#define SWITCH_SOCTHERM_STEP_MILLIC   1000
#define SWITCH_SOCTHERM_LVL0_UP_SHIFT 18u
#define SWITCH_SOCTHERM_LVL0_DN_SHIFT 9u
#define SWITCH_SOCTHERM_LVL0_THRESH   0x1ffu
#define SWITCH_SOCTHERM_LVL0_EN       (1u << 8)

struct switch_soctherm_group_regs {
    uint32_t lvl0, temp, temp_shift, intr_shift;
};

/* Indexed by enum switch_soctherm_group (SwitchSocTherm.h): CPU, GPU, PLLX. */
static const struct switch_soctherm_group_regs switch_soctherm_groups[] = {
    {0x0u, SWITCH_SOCTHERM_SENSOR_TEMP1, 16u, 8u},
    {0x4u, SWITCH_SOCTHERM_SENSOR_TEMP1, 0u, 16u},
    {0xcu, SWITCH_SOCTHERM_SENSOR_TEMP2, 0u, 0u},
};

static inline uint32_t switch_soctherm_intr_bits(const struct switch_soctherm_group_regs *g)
{
    return 3u << g->intr_shift;
}

static inline uint32_t switch_soctherm_thresh(int32_t millic)
{
    if (millic <= 0) {
        return 0;
    }
    const uint32_t steps = (uint32_t)millic / SWITCH_SOCTHERM_GRAIN_MILLIC;
    return steps > SWITCH_SOCTHERM_LVL0_THRESH ? SWITCH_SOCTHERM_LVL0_THRESH : steps;
}

/* The group's LVL0 word with its window one step either side of `millic`, enabled; `lvl0` keeps its other fields. */
static inline uint32_t switch_soctherm_lvl0_window(uint32_t lvl0, int32_t millic)
{
    const uint32_t mask = (SWITCH_SOCTHERM_LVL0_THRESH << SWITCH_SOCTHERM_LVL0_UP_SHIFT) |
                          (SWITCH_SOCTHERM_LVL0_THRESH << SWITCH_SOCTHERM_LVL0_DN_SHIFT) | SWITCH_SOCTHERM_LVL0_EN;
    const uint32_t up = switch_soctherm_thresh(millic + SWITCH_SOCTHERM_STEP_MILLIC);
    const uint32_t dn = switch_soctherm_thresh(millic - SWITCH_SOCTHERM_STEP_MILLIC);
    return (lvl0 & ~mask) | up << SWITCH_SOCTHERM_LVL0_UP_SHIFT | dn << SWITCH_SOCTHERM_LVL0_DN_SHIFT |
           SWITCH_SOCTHERM_LVL0_EN;
}

/* THERMCTL_THERMTRIP_CTL: past a group's threshold SOC_THERM asserts THERMTRIP, and with PMC_SENSOR_CTRL ENABLE_RST
 * the PMC resets the SoC; the temperatures are the last resort of SwitchThermalLimits.h. The MEM group (T210B01 has no
 * memory sensors) stays off. Thresholds in 0.5 C steps: TSENSE (PLLX) 8:0, CPU 17:9, GPU/MEM 26:18; enables TSENSE 27,
 * CPU 28, GPU 29; ANY_EN (31) off, as L4T leaves it. */
#define SWITCH_SOCTHERM_THERMTRIP_CTL     0x80u
#define SWITCH_PMC_SENSOR_CTRL            0x1b0u
#define SWITCH_PMC_SENSOR_CTRL_ENABLE_RST (1u << 1)

static inline uint32_t switch_soctherm_thermtrip_ctl(uint32_t ctl)
{
    const uint32_t thresh = SWITCH_SOCTHERM_LVL0_THRESH;
    ctl &= ~((thresh << 0) | (thresh << 9) | (thresh << 18) | (0xfu << 27) | (1u << 31));
    return ctl | switch_soctherm_thresh(SWITCH_THERMAL_THERMTRIP_PLLX_MILLIC) << 0 |
           switch_soctherm_thresh(SWITCH_THERMAL_THERMTRIP_CPU_MILLIC) << 9 |
           switch_soctherm_thresh(SWITCH_THERMAL_THERMTRIP_GPU_MILLIC) << 18 | (1u << 27) | (1u << 28) | (1u << 29);
}

#endif
