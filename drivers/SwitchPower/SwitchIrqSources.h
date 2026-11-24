// SPDX-License-Identifier: BSD-2-Clause-Patent
// The power stats' interrupt sources (SwitchPowerStats.cpp, debug kernels): the SPIs each INTID's handler took in the
// window (ArmGIC's per-INTID count), the busiest first. Plain data and text, so the host test runs it; the including
// file declares snprintf.
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace switch_power {

constexpr uint32_t kFirstSpi = 32, kIntidLimit = 1020, kTopSources = 6;

struct IrqSource {
    uint32_t intid, count;
};

struct IrqSources {
    uint32_t seen[kIntidLimit];  // each INTID's count since boot at the last window
    IrqSource top[kTopSources];
    uint64_t all;
    uint32_t sources;  // INTIDs that fired in the window
};

inline void startIrqWindow(IrqSources &s)
{
    s.all = 0;
    s.sources = 0;
    for (IrqSource &t : s.top)
        t = {};
}

// The INTID has taken `count` since boot.
inline void noteIrq(IrqSources &s, uint32_t intid, uint32_t count)
{
    const uint32_t n = count - s.seen[intid];
    s.seen[intid] = count;
    if (n == 0)
        return;
    s.all += n;
    ++s.sources;
    for (uint32_t i = 0; i < kTopSources; ++i) {
        if (n > s.top[i].count) {
            for (uint32_t j = kTopSources - 1; j > i; --j)
                s.top[j] = s.top[j - 1];
            s.top[i] = {intid, n};
            return;
        }
    }
}

// "10900 SPIs from 9 INTIDs (1090/s): 119 (spi 87) 8000 (800/s), ..." over a window of `ms`.
inline size_t formatIrqSources(const IrqSources &s, uint64_t ms, char *out, size_t size)
{
    size_t at = 0;
    const auto put = [&](int n) {
        if (n > 0)
            at = at + static_cast<size_t>(n) < size ? at + static_cast<size_t>(n) : size - 1;
    };
    if (size == 0)
        return 0;
    out[0] = '\0';
    put(snprintf(out, size, "%llu SPIs from %u INTIDs (%llu/s)", static_cast<unsigned long long>(s.all), s.sources,
                 static_cast<unsigned long long>(ms ? s.all * 1000 / ms : 0)));
    for (uint32_t i = 0; i < kTopSources && s.top[i].count != 0 && at < size - 1; ++i) {
        const IrqSource &t = s.top[i];
        put(snprintf(out + at, size - at, "%s %u (spi %u) %u (%llu/s)", i ? "," : ":", t.intid, t.intid - kFirstSpi,
                     t.count, static_cast<unsigned long long>(ms ? uint64_t{t.count} * 1000 / ms : 0)));
    }
    return at;
}

}  // namespace switch_power
