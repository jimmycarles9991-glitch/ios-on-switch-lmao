// SPDX-License-Identifier: BSD-2-Clause-Patent
// The PMU monotonic clock iPadOS reads through sysctl (SwitchMonotonicClock.cpp).
#ifndef SWITCH_MONOTONIC_CLOCK_H
#define SWITCH_MONOTONIC_CLOCK_H

#include <IOKit/IOReturn.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Registers the sysctls if the Tegra RTC counts; false (and nothing registered) if it does not. Once, before XNU's
// clock_initialize_calendar(), with the MAX77620 RTC's boot reading (UTC seconds and its mach_absolute_time(); seconds
// below 0 when there is none).
bool switch_monotonic_clock_start(int64_t rtc_secs, uint64_t rtc_stamp);

// Microseconds of the monotonic clock and the mach_absolute_time() read with them; false before a successful start.
bool switch_monotonic_clock_read(uint64_t *usec, uint64_t *mach_time);

// The calendar (SwitchRTC's UTC) minus the monotonic clock, in microseconds; false before a successful start or without
// the RTC service.
bool switch_monotonic_clock_offset(int64_t *usec);

// SwitchRTC's calendar time (SwitchPMIC.cpp); kIOReturnNotReady without the RTC service.
IOReturn switch_rtc_utc_usec(uint64_t *usec);

#ifdef __cplusplus
}
#endif

#endif
