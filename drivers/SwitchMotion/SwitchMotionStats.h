// SPDX-License-Identifier: BSD-2-Clause-Patent
// The motion driver's line (SwitchMotion.cpp, debug kernels): every 10 s of converting, what it converted for whom, what
// woke it and what it posted, from its interrupt action (no timer of its own). Plain data and text, so the host test
// runs it; the including file declares snprintf. Times are mach ticks.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct SwitchMotionStats {
    uint64_t start = 0, last = 0;  // the window's first interrupt and the latest; 0: none yet
    uint32_t interrupts = 0, empty = 0, posted[2] = {};
    uint32_t drains = 0, records = 0, overflows = 0;  // the driver's own counts at the window's start
};

// What the driver converts and for whom, as the line finds it.
struct SwitchMotionState {
    uint32_t milliHz;
    bool on[2];
    uint32_t intervalUS[2], batchUS[2];  // the clients' report and batch intervals, accelerometer then gyroscope
    bool detecting, lit, fifo;
    unsigned watermark;
    uint32_t drains, records, overflows;
};

inline void switchMotionRestart(SwitchMotionStats &s, uint64_t now, const SwitchMotionState &state)
{
    s = {};
    s.start = s.last = now;
    s.drains = state.drains;
    s.records = state.records;
    s.overflows = state.overflows;
}

// An interrupt at `now`, after its action. True when the window has run `window` ticks: its line is due, and the
// caller logs it and restarts. A gap of `gap` or more since the last interrupt (converting stopped and started again)
// restarts the window first.
inline bool switchMotionInterrupt(SwitchMotionStats &s, uint64_t now, uint64_t window, uint64_t gap,
                                  const SwitchMotionState &state)
{
    if (s.start == 0 || now - s.last >= gap)
        switchMotionRestart(s, now, state);
    s.last = now;
    ++s.interrupts;
    return now - s.start >= window;
}

inline size_t switchMotionLine(char *out, size_t size, const SwitchMotionStats &s, const SwitchMotionState &state,
                               uint64_t ms)
{
    if (size == 0)
        return 0;
    const int n = snprintf(
        out, size,
        "%llu ms at %u mHz: accel %d (client %u us, batch %u us), gyro %d (client %u us, batch %u us), detector %d, "
        "panel lit %d, FIFO %d (watermark %u); %u interrupts (%llu/s), %u empty, posted %u accel %u gyro, %u drains "
        "of %u records, %u overflows",
        static_cast<unsigned long long>(ms), state.milliHz, state.on[0], state.intervalUS[0], state.batchUS[0],
        state.on[1], state.intervalUS[1], state.batchUS[1], state.detecting, state.lit, state.fifo, state.watermark,
        s.interrupts, static_cast<unsigned long long>(ms ? uint64_t{s.interrupts} * 1000 / ms : 0), s.empty,
        s.posted[0], s.posted[1], state.drains - s.drains, state.records - s.records, state.overflows - s.overflows);
    if (n < 0)
        return 0;
    return static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
}
