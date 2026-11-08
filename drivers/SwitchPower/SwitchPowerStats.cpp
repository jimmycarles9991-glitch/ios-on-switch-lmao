// SPDX-License-Identifier: BSD-2-Clause-Patent
// The power stats line (debug kernels only, os_log): every 10 s, each core's time in C7, in WFI and busy over the
// window, with its C7 entries, the requests the firmware returned from at once and the frame lines that found it in C7;
// the C7 exits wake timers ended (mean, largest since boot, and those back after their deadline); the EMC's rate changes
// and time at each rate, then the battery's draw.
#include "SwitchPowerStats.h"

#if DEVELOPMENT || DEBUG
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <os/log.h>
#include "../../kits/SwitchIdle/switch_idle.h"
#include "../SwitchBattery/SwitchBattery.h"
#include "SwitchEMC.h"

namespace {
constexpr uint32_t kCores = 4;
constexpr uint64_t kWindowTicks = UINT64_C(19200000) * 10;

uint64_t gLast;
switch_idle_counts gSeen[kCores];

unsigned permille(uint64_t part, uint64_t whole)
{
    return whole ? static_cast<unsigned>(part * 1000 / whole) : 0;
}

uint64_t ticksToUs(uint64_t ticks)
{
    return ticks * 5 / 96;
}

size_t appendIdle(char *line, size_t size, size_t n, uint64_t window)
{
    uint64_t exits = 0;
    uint64_t exitTicks = 0;
    uint64_t exitMax = 0;
    uint64_t late = 0;
    for (uint32_t cpu = 0; cpu < kCores && n < size; ++cpu) {
        switch_idle_counts now;
        if (switch_idle_read_counts(cpu, &now))
            continue;
        const switch_idle_counts &was = gSeen[cpu];
        const uint64_t c7 = now.residency[1] - was.residency[1];
        const uint64_t wfi = now.residency[0] - was.residency[0];
        const unsigned c7Pm = permille(c7, window);
        const unsigned wfiPm = permille(wfi, window);
        const unsigned busyPm = c7Pm + wfiPm < 1000 ? 1000 - c7Pm - wfiPm : 0;
        n += static_cast<size_t>(snprintf(
            line + n, size - n, "%scpu%u c7 %u.%u%% (%llu, %llu skipped, %llu frame wakes) wfi %u.%u%% busy %u.%u%%",
            cpu ? ", " : "", cpu, c7Pm / 10, c7Pm % 10, now.entries[1] - was.entries[1], now.skipped - was.skipped,
            now.frame_wakes - was.frame_wakes, wfiPm / 10, wfiPm % 10, busyPm / 10, busyPm % 10));
        exits += now.timer_exits - was.timer_exits;
        exitTicks += now.exit_ticks - was.exit_ticks;
        late += now.exits_late - was.exits_late;
        exitMax = now.exit_max > exitMax ? now.exit_max : exitMax;
        gSeen[cpu] = now;
    }
    if (n < size) {
        n += static_cast<size_t>(snprintf(line + n, size - n,
                                          "; c7 timer exits %llu, mean %llu us, boot max %llu us, %llu late", exits,
                                          exits ? ticksToUs(exitTicks / exits) : 0, ticksToUs(exitMax), late));
    }
    return n;
}

size_t appendBattery(char *line, size_t size, size_t n)
{
    switch_battery_gauge gauge = {};
    if (n >= size || switch_battery_gauge_sample(&gauge))
        return n;
    const long mw = static_cast<long>(gauge.avg_ma) * static_cast<long>(gauge.mv) / 1000;
    return n + static_cast<size_t>(
                   snprintf(line + n, size - n, "; battery %d mA x %u mV = %ld mW", gauge.avg_ma, gauge.mv, mw));
}
}  // namespace

void switch_power_stats_tick()
{
    const uint64_t now = mach_absolute_time();
    if (gLast && now - gLast < kWindowTicks)
        return;
    const uint64_t window = gLast ? now - gLast : 0;
    char line[768];
    size_t n = static_cast<size_t>(snprintf(line, sizeof(line), "SwitchPower: %llu ms: ", window / 19200));
    n = appendIdle(line, sizeof(line), n, window);
    if (n + 2 < sizeof(line)) {
        n += static_cast<size_t>(snprintf(line + n, sizeof(line) - n, "; "));
        n += switch_emc_stats(line + n, sizeof(line) - n);
    }
    appendBattery(line, sizeof(line), n);
    if (gLast)
        os_log(OS_LOG_DEFAULT, "%s", line);
    gLast = now;
}
#endif
