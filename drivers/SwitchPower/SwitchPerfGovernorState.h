// SPDX-License-Identifier: BSD-2-Clause-Patent
// The CPU governor's state, shared by SwitchPerfGovernor.cpp (the scheduler's callouts, the hints, the owner's take)
// and SwitchPerfGovernorLimits.cpp (the thread side). Each part has one writer, or one lock its writers share that no
// callout and no interrupt filter takes:
//   * the table, built by start() and swapped whole (gConfig[gGeneration & 1]);
//   * each CPU's load slot (SwitchCpuLoad.h), written by that CPU's context-switch callout;
//   * each boost's deadline: the touch filter's, the display gate's, and exec's in the slot of the CPU it ran on with
//     interrupts masked;
//   * the tick's stepping state and its published decision: XNU runs deadline_passed on one CPU at a time;
//   * the thermal floor and caps, thermalmonitord's ceiling and the boot window: the thread side under gThreadLock,
//     which publishes them to the callouts as one limits word and one deadline;
//   * the index the DFLL runs: the owner's work loop.
#ifndef SWITCH_PERF_GOVERNOR_STATE_H
#define SWITCH_PERF_GOVERNOR_STATE_H

#include <IOKit/IOLib.h>
#include <stdint.h>
#include "SwitchCpuLoad.h"
#include "SwitchGovernor.h"
#include "SwitchPerfGovernor.h"

namespace switchperf {

constexpr unsigned kCpus = kSignalCpus;

struct Config {
    uint32_t khz[SWITCH_GOV_MAX_OPPS];
    uint32_t mv[SWITCH_GOV_MAX_OPPS];
    unsigned count, start, cpus;
    switch_gov_tunables tunables;
    uint64_t window, logWindow, touchHold, frameHold, launchHold;
};

extern Config gConfig[2];
extern uint32_t gGeneration;  // start() only: the config in force is gConfig[gGeneration & 1]
extern bool gRunning;         // start() and stop() only
extern Signal gSignal;
extern void *gCtx;

extern switch_cpu_load gLoad[kCpus];

// A boost's deadline, one writer each: the touch filter, the display gate, exec in the slot of the CPU it ran on.
struct Boost {
    uint64_t until;
#if DEVELOPMENT || DEBUG
    uint32_t hints, wakes;  // since boot, by the same writer
#endif
};

extern Boost gTouch, gFrame, gLaunch[kCpus];

extern IOLock *gThreadLock;
extern uint32_t gLimits;     // lo | hi << 8
extern uint64_t gBootUntil;  // the boot window's end, 0 once it closed
extern bool gBootDone;       // read from any context; written under gThreadLock

extern uint32_t gApplied, gAppliedKhz;

extern uint32_t gDecided;  // the tick's index | gGeneration << 8

#if DEVELOPMENT || DEBUG
extern uint32_t gBootHints;  // under gThreadLock
#endif

inline const Config &config()
{
    return gConfig[__atomic_load_n(&gGeneration, __ATOMIC_ACQUIRE) & 1];
}

inline uint32_t packLimits(unsigned lo, unsigned hi)
{
    return lo | hi << 8;
}

inline unsigned limitsLo(uint32_t limits)
{
    return limits & 0xffu;
}

inline unsigned limitsHi(uint32_t limits)
{
    return limits >> 8 & 0xffu;
}

// The latest boost deadline of any source.
uint64_t boostUntil();

// What the owner applies now: the tick's decision, the top while a boost runs, inside the limits published last.
unsigned indexNow(uint64_t now);

// Wakes the owner from thread context (interrupts masked around it, so this CPU's source has one producer).
void signalHere();

// The thread side (SwitchPerfGovernorLimits.cpp). start(): the boot window, the limits.
void startLimits(IOLock *lock, uint64_t now);
// The owner: the boot window's fallback passed (true once per boot: that take logs it).
bool closeBootWindow(uint64_t now);
// System Boot Complete.
void bootDone();
// The highest index the published limits allow.
unsigned ceiling();

}  // namespace switchperf

#endif
