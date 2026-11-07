// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchBattery's interface for other drivers (the battery itself is an IOPMPowerSource; see SwitchBattery.cpp).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// The BQ24193's OTG boost for a sink-only partner on the USB-C port (drivers/SwitchUSB/SwitchPD, which also brings up
// v_vdd5v3, the OLED's own VBUS power, before it): on = REG01 CHG_CONFIG 10 (OTG) with BOOST_LIM 1.3 A
// (bq2419x_vbus_enable), which also keeps the charger from charging from our own VBUS; refused while the charger sees
// an input on VBUS (REG08 PG_STAT or VBUS_STAT USB/adapter: the partner sources, we must sink), unless `ourVbus` (the
// PD controller's own source switch is closed: what the charger sees is our v_vdd5v3). off = CHG_CONFIG 01 (charge).
// Absolute, never a restore: a charger left in OTG by an earlier boot is put back to charge at SwitchBattery's start.
// The boost needs the charger's OTG pin high too (datasheet 8.3.1.5); REG08 VBUS_STAT 11 says it runs
// (switch_battery_describe_otg). While it runs the charger's I2C watchdog is off (switchroot ti,no-otg-watchdog): its
// expiry would reset REG01 and end the boost. With charging off by temperature, the same. SwitchBattery is the one
// writer of REG01; off = charge, or off in a battery temperature zone that does not charge. Thread context (I2C1); 0 on
// success, SWITCH_BATTERY_BOOST_VBUS_PRESENT when refused, -1 on an I2C error (logged).
#define SWITCH_BATTERY_BOOST_VBUS_PRESENT (-2)
int switch_battery_set_otg_boost(bool on, bool ourVbus);

// One line for the USB role report (drivers/SwitchUSB/SwitchUSB.cpp), nothing written: REG01 (CHG_CONFIG, BOOST_LIM),
// REG05 (I2C watchdog: a write to the charger starts it; when it expires the registers reset, OTG included), REG08
// (VBUS_STAT, CHRG_STAT, PG), REG09 twice (latched faults, then now: BOOST_FAULT, WATCHDOG_FAULT; what it clears is
// kept for SwitchBattery's next reading) and the gauge's battery current (negative: the battery is discharging, into
// the port too while we source). Thread context (I2C1); 0, or the I2C error (the line says so).
int switch_battery_describe_otg(char *buffer, size_t size);
// The fuel gauge's average current at its last reading, mA (positive = charging; INT32_MIN before a reading), and in
// *external (may be NULL) whether external power was in. Any context.
int switch_battery_avg_ma(bool *external);

// The battery temperature (MAX17050 TEMP, the pack's thermistor) in m C and when it was read (mach absolute time), for
// thermalmonitord's battery sensors (drivers/SwitchPower/SwitchSensors.cpp). Kept current by the gauge's temperature
// alert window (a degree either way of the last reading raises ALRT) besides the other alerts and the slow re-read.
// 0 before the first reading or after a failed one. Any context.
int switch_battery_temperature(int *milli_c, unsigned long long *at);

// System sleep (SwitchSuspend, SC7): before the wake events are armed, the gauge's alert is narrowed to the battery's
// temperature zone edges (SOC and voltage windows off), so its ALRT (PY0, PMC wake 27) wakes the system only for a
// charge-zone change; after the wake, a re-read restores the normal windows. Thread context, I2C up.
void switch_battery_suspend(void);
void switch_battery_resume(void);

// The gauge right before and right after SC7, for SwitchSuspend's drain line, and for the power stats line: QH (the
// coulomb counter's upper word, SWITCH_BATTERY_QH_UAH per count), AvgCurrent and Current in mA (positive = charging),
// VCELL in mV. Thread context, I2C up; 0, or the I2C error.
#define SWITCH_BATTERY_QH_UAH 500u

struct switch_battery_gauge {
    uint16_t qh;
    int avg_ma, now_ma;
    unsigned mv;
};

int switch_battery_gauge_sample(struct switch_battery_gauge *sample);

#ifdef __cplusplus
}
#endif
