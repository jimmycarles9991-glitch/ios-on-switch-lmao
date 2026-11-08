// SPDX-License-Identifier: BSD-2-Clause-Patent
// Where kern.monotonicclock starts at boot (SwitchMonotonicClock.cpp). Plain C, so drivers/SwitchPower/test runs it.
//
// J120's PMU clock counts from the battery's first connection and never goes back across a reboot. timed relies on
// that: it keeps the clock with the UTC it last trusted in /var/db/timed/com.apple.timed.plist and, at its next start,
// throws that pair away when the clock reads below it ("Current RTC value older than cache. RTC reset likely"). With
// no network it then sets the time from a filesystem timestamp, minutes or days in the past, and every time-keyed
// scheduler (xpc activities, widget timelines) starts over. The Tegra RTC counts from the SoC's last power-up, so on its
// own it reads below the previous boot's value after every cold boot. The MAX77620 RTC keeps counting while the
// console is off, from the same 32 kHz crystal: anchored to it, the clock reads seconds since the MAX77620's 2000
// epoch, carries on from the previous boot's reading, and the calendar offset stays the epoch, never zero (timed also
// throws away its pair on a zero offset).
#ifndef SWITCH_MONOTONIC_BASE_H
#define SWITCH_MONOTONIC_BASE_H

#include <stdint.h>

#define SWITCH_RTC_EPOCH_SECS         946684800LL /* 2000-01-01 UTC: the MAX77620's year 0 */
#define SWITCH_MONOTONIC_USEC_PER_SEC 1000000ULL

/*
 * Added to every Tegra RTC reading. rtc_secs: the MAX77620's UTC seconds (negative when it was not read); tegra_usec
 * and mach_usec: the Tegra RTC and mach time at the clock's first latch; since_rtc_usec: mach time from the MAX77620
 * read to that latch. Without the MAX77620, as before: nothing, or enough to keep the clock a second above mach time,
 * which XNU subtracts from it unsigned.
 */
static inline uint64_t switch_monotonic_base_usec(int64_t rtc_secs, uint64_t tegra_usec, uint64_t since_rtc_usec,
                                                  uint64_t mach_usec)
{
    if (rtc_secs > SWITCH_RTC_EPOCH_SECS) {
        // NOLINTBEGIN(modernize-avoid-c-style-cast): C (kern_exec.c) reads this header too
        const uint64_t anchor =
            (uint64_t)(rtc_secs - SWITCH_RTC_EPOCH_SECS) * SWITCH_MONOTONIC_USEC_PER_SEC + since_rtc_usec;
        // NOLINTEND(modernize-avoid-c-style-cast)

        if (anchor > tegra_usec && anchor > mach_usec) {
            return anchor - tegra_usec;
        }
    }
    return tegra_usec < mach_usec ? mach_usec - tegra_usec + SWITCH_MONOTONIC_USEC_PER_SEC : 0;
}

#endif /* SWITCH_MONOTONIC_BASE_H */
