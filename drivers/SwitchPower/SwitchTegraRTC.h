// SPDX-License-Identifier: BSD-2-Clause-Patent
// The Tegra RTC at the start of the PMC page (Linux rtc-tegra.c). Reading MILLI_SECONDS latches SECONDS into
// SHADOW_SECONDS (TRM 11.2.4-11.2.5), so one reading is those two reads back to back with no other MILLI_SECONDS read
// in between, from anywhere. Every reading goes through here, by one of two callers that cannot overlap:
// SwitchMonotonicClock under its latch lock, and system sleep's quiesce and activate, which run alone on the boot CPU
// with interrupts off.
#ifndef SWITCH_TEGRA_RTC_H
#define SWITCH_TEGRA_RTC_H

#include <stdint.h>
#include "../../kits/SwitchMMIO/SwitchMMIO.h"

struct switch_tegra_rtc_reading {
    uint32_t seconds, millis;
};

static inline struct switch_tegra_rtc_reading switch_tegra_rtc_read(const volatile uint8_t *pmc_page)
{
    const uint32_t millis = switch_mmio_read32(pmc_page, 0x010);
    const struct switch_tegra_rtc_reading reading = {switch_mmio_read32(pmc_page, 0x00c), millis};
    return reading;
}

#endif
