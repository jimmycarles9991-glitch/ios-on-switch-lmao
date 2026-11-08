// SPDX-License-Identifier: BSD-2-Clause-Patent
// The software thermal shutdown's record in the MAX77620 RTC's ALARM2 (SwitchPowerMark.cpp), the one store a power-off
// keeps: the next boot reads it, files it (SwitchResetCause.h) and clears it. Every field stays in its RTC range (the
// RTC keeps the old value of a field written out of range) with its compare bit clear, so the alarm never matches:
// HOUR the sensor, MIN whole degrees above 60, SEC tenths, YEAR kMagic, weekday, month and date 1. Debug kernels' durable
// marks use the same registers with another magic and give way to this record for the rest of the boot. Plain C.
#ifndef SWITCH_THERMAL_RECORD_H
#define SWITCH_THERMAL_RECORD_H

#include <stdint.h>
#include "../../diagnostics/switch/SwitchResetCause.h"

#define SWITCH_THERMAL_RECORD_MAGIC  92u
#define SWITCH_THERMAL_RECORD_FIELDS 7u

// Field order as the RTC's: SEC, MIN, HOUR, WEEKDAY, MONTH, YEAR, DATE.
static inline void switch_thermal_record_encode(enum switch_thermal_sensor sensor, int millic,
                                                uint8_t fields[SWITCH_THERMAL_RECORD_FIELDS])
{
    int tenths = millic / 100 - 600;
    if (tenths < 0) {
        tenths = 0;
    }
    if (tenths > 599) {
        tenths = 599;
    }
    fields[0] = static_cast<uint8_t>(tenths % 10);
    fields[1] = static_cast<uint8_t>(tenths / 10);
    fields[2] = static_cast<uint8_t>(sensor);
    fields[3] = 1;
    fields[4] = 1;
    fields[5] = SWITCH_THERMAL_RECORD_MAGIC;
    fields[6] = 1;
}

// The sensor, and *millic, of a record; SWITCH_THERMAL_NONE when ALARM2 holds none.
static inline enum switch_thermal_sensor
switch_thermal_record_decode(const uint8_t fields[SWITCH_THERMAL_RECORD_FIELDS], int *millic)
{
    for (unsigned i = 0; i < SWITCH_THERMAL_RECORD_FIELDS; ++i) {
        if (fields[i] & 0x80u) {
            return SWITCH_THERMAL_NONE;
        }
    }
    if (fields[5] != SWITCH_THERMAL_RECORD_MAGIC || fields[0] > 9 || fields[1] > 59 ||
        (fields[2] != SWITCH_THERMAL_SOC_DIODE && fields[2] != SWITCH_THERMAL_PLLX)) {
        return SWITCH_THERMAL_NONE;
    }
    *millic = 60000 + fields[1] * 1000 + fields[0] * 100;
    return static_cast<enum switch_thermal_sensor>(fields[2]);
}

#endif
