// SPDX-License-Identifier: BSD-2-Clause-Patent
// The CPU governor's periodic line (SwitchPerfGovernorLine.cpp). The tick accumulates its windows and, every
// kLogWindowUs, publishes them with the governor's residency and steps under a sequence count only it writes; the
// owner's take() copies the published set, which also tells it a line is due, and makes the line. Release builds
// have no line.
#ifndef SWITCH_PERF_GOVERNOR_LINE_H
#define SWITCH_PERF_GOVERNOR_LINE_H

#include <stddef.h>
#include <stdint.h>
#include "SwitchGovernor.h"

namespace switchperf {

// One window's busy shares, in permille of one CPU: the decision's (all CPUs summed, capped at one), the busiest CPU's,
// all CPUs summed (up to 1000 per CPU), and the utility and background threads' part of that sum.
struct WindowBusy {
    uint32_t decision, peak, all, efficiency;
};

// The tick's windows since its last line.
struct LineStats {
    uint64_t since;
#if DEVELOPMENT || DEBUG
    uint64_t busySum, peakSum, allSum, efficiencySum;
    uint32_t windows, busyPeak, wakes;
    uint32_t topOneCore, topEfficiency;  // top-point windows one CPU alone made; that utility and background work made
#endif
};

// A published line, as take() copies it.
struct LineTaken {
#if DEVELOPMENT || DEBUG
    uint64_t span, busySum, peakSum, allSum, efficiencySum;
    uint64_t residency[SWITCH_GOV_MAX_OPPS];
    uint32_t ups, downs, jumps, boosted, clamped, windows, busyPeak, wakes, topOneCore, topEfficiency;
    unsigned lo, hi;
    bool boot;
#endif
};

// The tick: one window's busy shares, against the share that jumps to the top point; the tick woke the owner.
void lineWindow(LineStats *stats, const WindowBusy &busy, uint32_t upPermille);
void lineWake(LineStats *stats);
// The tick: the line is due; its windows and `gov`'s accounting published and started again.
void linePublish(LineStats *stats, switch_gov *gov, uint64_t now);
// The owner: whether a line was published since the last take, copied into `taken`.
bool lineTake(LineTaken *taken);
// The owner: the line, with the index it applies now.
void lineFormat(const LineTaken &taken, unsigned index, char *line, size_t len);

}  // namespace switchperf

#endif
