// SPDX-License-Identifier: BSD-2-Clause-Patent
// The CPU governor's periodic line (SwitchPerfGovernorLine.h).
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include "SwitchPerfGovernorLine.h"
#include "SwitchPerfGovernorState.h"
#include "../SwitchBattery/SwitchBattery.h"  // switch_battery_avg_ma: the draw beside the clocks
#include "../TegraDFLL/TegraDFLL.h"

namespace {
constexpr unsigned kLineTries = 4;

// The tick writes gLine with gLineSeq odd; take() copies it while gLineSeq stays even and unchanged.
uint32_t gLineSeq;
uint32_t gLineTakenSeq;  // the owner's: the last line it took
#if DEVELOPMENT || DEBUG
switchperf::LineTaken gLine;
// The owner's: touch, frame and launch boosts, then the owner wakes they made, at its last line.
uint32_t gBoostsSeen[6];

template <class T> void put(T *to, T value)
{
    __atomic_store_n(to, value, __ATOMIC_RELAXED);
}

template <class T> T get(const T *from)
{
    return __atomic_load_n(from, __ATOMIC_RELAXED);
}

void copyLine(switchperf::LineTaken *to, const switchperf::LineTaken *from)
{
    for (unsigned i = 0; i < SWITCH_GOV_MAX_OPPS; ++i)
        to->residency[i] = get(&from->residency[i]);
    to->span = get(&from->span);
    to->busySum = get(&from->busySum);
    to->ups = get(&from->ups);
    to->downs = get(&from->downs);
    to->jumps = get(&from->jumps);
    to->boosted = get(&from->boosted);
    to->clamped = get(&from->clamped);
    to->windows = get(&from->windows);
    to->busyPeak = get(&from->busyPeak);
    to->wakes = get(&from->wakes);
    to->lo = get(&from->lo);
    to->hi = get(&from->hi);
    to->boot = get(&from->boot);
}

// Boosts of one source since the last line.
uint32_t boostsSince(unsigned source, uint32_t now)
{
    const uint32_t since = now - gBoostsSeen[source];
    gBoostsSeen[source] = now;
    return since;
}
#endif
}  // namespace

void switchperf::lineWindow(LineStats *stats, uint32_t busy)
{
#if DEVELOPMENT || DEBUG
    ++stats->windows;
    stats->busySum += busy;
    if (busy > stats->busyPeak)
        stats->busyPeak = busy;
#else
    (void)stats;
    (void)busy;
#endif
}

void switchperf::lineWake(LineStats *stats)
{
#if DEVELOPMENT || DEBUG
    ++stats->wakes;
#else
    (void)stats;
#endif
}

void switchperf::linePublish(LineStats *stats, switch_gov *gov, uint64_t now)
{
    const uint32_t seq = gLineSeq;
#if DEVELOPMENT || DEBUG
    __atomic_store_n(&gLineSeq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    switch_gov_account(gov, now);
    for (unsigned i = 0; i < SWITCH_GOV_MAX_OPPS; ++i) {
        put(&gLine.residency[i], gov->residency[i]);
        gov->residency[i] = 0;
    }
    put(&gLine.span, now - stats->since);
    put(&gLine.busySum, stats->busySum);
    put(&gLine.ups, gov->ups);
    put(&gLine.downs, gov->downs);
    put(&gLine.jumps, gov->jumps);
    put(&gLine.boosted, gov->boosted);
    put(&gLine.clamped, gov->clamped);
    put(&gLine.windows, stats->windows);
    put(&gLine.busyPeak, stats->busyPeak);
    put(&gLine.wakes, stats->wakes);
    put(&gLine.lo, gov->lo);
    put(&gLine.hi, gov->hi);
    put(&gLine.boot, now < __atomic_load_n(&gBootUntil, __ATOMIC_RELAXED));
    gov->ups = gov->downs = gov->jumps = gov->boosted = gov->clamped = 0;
    *stats = {};
#else
    (void)gov;
#endif
    stats->since = now;
    __atomic_store_n(&gLineSeq, seq + 2, __ATOMIC_RELEASE);
}

bool switchperf::lineTake(LineTaken *taken)
{
    for (unsigned i = 0; i < kLineTries; ++i) {
        const uint32_t seq = __atomic_load_n(&gLineSeq, __ATOMIC_ACQUIRE);
        if (seq == gLineTakenSeq)
            return false;
        if (seq & 1u)
            continue;
#if DEVELOPMENT || DEBUG
        copyLine(taken, &gLine);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
#else
        (void)taken;
#endif
        if (__atomic_load_n(&gLineSeq, __ATOMIC_RELAXED) == seq) {
            gLineTakenSeq = seq;
            return true;
        }
    }
    return false;  // the tick is publishing another: it wakes the owner again once it has
}

void switchperf::lineFormat(const LineTaken &taken, unsigned index, char *line, size_t len)
{
#if !(DEVELOPMENT || DEBUG)
    (void)taken;
    (void)index;
    (void)line;
    (void)len;
#else
    const Config &in = config();
    uint64_t spanUs = 0;
    absolutetime_to_nanoseconds(taken.span, &spanUs);
    spanUs /= 1000;
    size_t n = static_cast<size_t>(snprintf(line, len, "SwitchPerf governor: %llu ms:", spanUs / 1000));
    for (unsigned i = 0; i < in.count && n < len; ++i) {
        if (!taken.residency[i] && (i < taken.lo || i > taken.hi))
            continue;
        const uint64_t pct = taken.span ? taken.residency[i] * 1000 / taken.span : 0;
        n += static_cast<size_t>(snprintf(line + n, len - n, " %u %llu.%llu%%", in.khz[i] / 1000, pct / 10, pct % 10));
    }
    uint32_t launches = 0;
    uint32_t launchWakes = 0;
    for (const Boost &slot : gLaunch) {
        launches += __atomic_load_n(&slot.hints, __ATOMIC_RELAXED);
        launchWakes += __atomic_load_n(&slot.wakes, __ATOMIC_RELAXED);
    }
    const uint32_t touches = boostsSince(0, __atomic_load_n(&gTouch.hints, __ATOMIC_RELAXED));
    const uint32_t frames = boostsSince(1, __atomic_load_n(&gFrame.hints, __ATOMIC_RELAXED));
    launches = boostsSince(2, launches);
    const uint32_t touchWakes = boostsSince(3, __atomic_load_n(&gTouch.wakes, __ATOMIC_RELAXED));
    const uint32_t frameWakes = boostsSince(4, __atomic_load_n(&gFrame.wakes, __ATOMIC_RELAXED));
    launchWakes = boostsSince(5, launchWakes);
    unsigned long target = 0;
    unsigned long measured = 0;
    unsigned vdd = 0;
    tegra_dfll_cpu_status(&target, &measured, &vdd);
    if (n < len) {
        n += static_cast<size_t>(snprintf(
            line + n, len - n,
            "; %u up %u down (%u jumps, %u boosted, %u clamped); busy mean %llu%% peak %u%% over %u windows; boosts "
            "touch %u frame %u launch %u%s; range %u-%u MHz; now %u MHz (DFLL target %lu, measured %lu MHz, VDD_CPU %u "
            "mV)",
            taken.ups, taken.downs, taken.jumps, taken.boosted, taken.clamped,
            taken.windows ? taken.busySum / taken.windows / 10 : 0, taken.busyPeak / 10, taken.windows, touches, frames,
            launches, taken.boot ? ", boot floor" : "", in.khz[taken.lo] / 1000, in.khz[taken.hi] / 1000,
            in.khz[index] / 1000, target / 1000000, measured / 1000000, vdd));
    }
    if (n < len) {
        n += static_cast<size_t>(snprintf(line + n, len - n, "; owner wakes: tick %u, touch %u, frame %u, launch %u",
                                          taken.wakes, touchWakes, frameWakes, launchWakes));
    }
    bool external = false;
    const int ma = switch_battery_avg_ma(&external);
    if (ma != INT32_MIN && n < len)
        snprintf(line + n, len - n, "; battery %d mA%s", ma, external ? " (external power in)" : "");
#endif
}
