// SPDX-License-Identifier: BSD-2-Clause-Patent
// What HW Test's display.x1.landscape_video reads of TegraVIC (debug kernels only): each IOSurfaceAccelerator request
// shape once (who asks, the transform, the formats and sizes, whether it carries events or a callback), the jobs and
// recorded quarter turns by who asked, and TegraVIC's CPU time; and the run's kernel-test requests
// (SwitchLandscapeRunABI.h).
#pragma once
#include <stdint.h>
#include "TegraVIC.h"

#if TEGRA_VIC_STATS
#include <IOKit/IOUserClient.h>
#include "TegraVICRequest.h"
#include "TegraVICSurface.h"

// Who asked for a job: Core Animation's display server (backboardd, the route under test), the HW Test process that
// last made a landscape-run request, anyone else (SWITCH_DISPLAY_LANDSCAPE_ORIGIN_*).
struct SwitchVICTurnCounters {
    uint32_t jobs[3];        // submitted to VIC
    uint32_t turns[3];       // quarter turns recorded instead of copied
    uint32_t eventTurns[3];  // quarter turns whose request carries events
    uint64_t ticks;          // mach_absolute_time in requests and on the engine thread
};

void switch_vic_turn_request_seen(const TegraVICRequest &request, const TegraVICSurfaceDesc &source,
                                  const TegraVICSurfaceDesc &destination, int pid);
// In the requesting process's context.
uint8_t switch_vic_turn_origin(int pid);
void switch_vic_turn_count(uint8_t origin, bool submitted, bool turned, bool eventTurn, uint64_t ticks);
extern "C" void switch_vic_turn_counters(SwitchVICTurnCounters *out);
IOReturn switch_vic_landscape_call(const void *client, const IOExternalMethodArguments *a);
#endif
