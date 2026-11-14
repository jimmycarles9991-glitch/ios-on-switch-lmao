// SPDX-License-Identifier: BSD-2-Clause-Patent
// The CPU governor's kernel side (SwitchPerfGovernor.cpp) as SwitchThermal drives it. SwitchThermal owns the DFLL and
// the thermal state; this side owns the scheduler's perfcontrol callouts, the load accounting, the hints and the
// decisions (SwitchGovernor.h). A decision that changes the clock (or a periodic log line that is due) calls the
// owner's `signal` from whatever context made it (the scheduler's callout, an interrupt filter, a thread), with
// interrupts masked and the CPU it runs on; the owner then calls take() on its work loop, sets the DFLL to that point
// and reports it with applied().
#ifndef SWITCH_PERF_GOVERNOR_H
#define SWITCH_PERF_GOVERNOR_H

#include <stddef.h>
#include <stdint.h>

namespace switchperf {

// One wake source per CPU, signalled only by code running on that CPU with interrupts masked, so each source has one
// producer at a time (IOInterruptEventSource::interruptOccurred counts with a plain increment).
constexpr unsigned kSignalCpus = 4;  // the Switch's A57 cores (SWITCH_SMP_CORE_COUNT)
using Signal = void (*)(void *ctx, unsigned cpu);

// The boot window's end if System Boot Complete never comes, in seconds of uptime (r8a's slow boot had it at 74 s).
constexpr unsigned kBootFallbackSeconds = 120;

// Starts governing over `count` points (kHz ascending, with their CVB mV), the clock now at `startIndex`, with the boot
// window in force until SWITCH_PERF_HINT_BOOT_DONE (System Boot Complete) or kBootFallbackSeconds of uptime: the CPU at
// its ceiling, the GPU's idle clock held off. A boot window that has ended does not open again on a restart. Registers
// the perfcontrol callouts and arms the first window. False (with `why`) if the table is empty or the governor already
// runs. Work loop only.
bool start(const uint32_t *khz, const uint32_t *mv, unsigned count, unsigned startIndex, Signal signal, void *ctx,
           char *why, size_t len);

// Stops: callouts unregistered, no signal after it returns, hints ignored. Idempotent. Work loop only.
void stop();

bool running();

// Work loop, after a signal: the index the clock should run at now. `line` receives the periodic os_log line when one
// is due ("" otherwise), and the accounting restarts.
unsigned take(char *line, size_t len);

// What the DFLL now runs (kHz of that index), for hints, kprof and the next decision's raise test.
void applied(unsigned index);

// Thermal limits in mV (the DFLL's floor and cap now in force): the lowest point is the highest row at or below the
// floor, the highest the highest row at or below the cap. Applied at once; signals if the clock must move.
void limits(unsigned floorMv, unsigned capMv);

uint32_t khzAt(unsigned index);

// The context-switch callout's last parameter: xnu-12377 passes a timeout the callout may set, xnu-10063 nothing.
#if XNU_12377_121_6
typedef uint64_t *CswTimeout;
#else
typedef void *CswTimeout;
#endif

}  // namespace switchperf

#endif
