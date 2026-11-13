// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SWITCH_DISPLAY_PRIORITY_H
#define SWITCH_DISPLAY_PRIORITY_H

#include <stdint.h>

namespace SwitchDisplayPriority {
constexpr uint32_t kPriority = 0x403, kTimer = 0x404;
// Linux v6.12 tegra_crtc_atomic_enable uses these for Tegra210's non-NVDisplay path.
constexpr uint32_t kThresholds = 0x00202020, kDelays = 0x00010101;

template <class Registers> void initialize(Registers &regs)
{
    constexpr uint32_t stateAccess = 0x040, stateControl = 0x041;
    const uint32_t access = regs[stateAccess];
    regs[stateAccess] = access & ~4u;
    regs[kPriority] = kThresholds;
    regs[kTimer] = kDelays;
    regs[stateControl] = 1u << 8;
    regs[stateControl] = 1u;
    regs[stateAccess] = access;
}
}  // namespace SwitchDisplayPriority
#endif
