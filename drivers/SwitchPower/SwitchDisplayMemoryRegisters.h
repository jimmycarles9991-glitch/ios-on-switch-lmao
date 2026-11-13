// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchDisplayMemoryMath.h"

namespace switch_display {
template <class Registers> bool memoryProgram(Registers &regs, const MemoryPlan plans[2], const bool owned[2])
{
    for (unsigned head = 0; head != 2; ++head)
        for (unsigned window = 0; owned[head] && window != 3; ++window)
            if (plans[head].allowance[window] > 255)
                return false;
    constexpr uint32_t dis[2] = {0x41c, 0x428};
    uint32_t rates[2];
    for (unsigned head = 0; head != 2; ++head)
        rates[head] = owned[head] ? plans[head].ptsa_rate : uint32_t(regs[dis[head] / 4]) & 0xfff;
    // Preserve firmware's camera grants and budget all of them as isochronous: this is an upper bound without
    // camera software state. The display owner never modifies camera, CPU, FTOP or global grant configuration.
    const uint32_t camera =
        (uint32_t(regs[0x434 / 4]) & 0xfff) + (uint32_t(regs[0x494 / 4]) & 0xfff) + (uint32_t(regs[0x4a0 / 4]) & 0xfff);
    uint32_t ring =
        (rates[0] + rates[1] + camera) / 2 + (uint32_t(regs[0x44c / 4]) & 0xfff) + (uint32_t(regs[0x50c / 4]) & 0xfff);
    if (ring > 0xfff || rates[0] > 0xfff || rates[1] > 0xfff)
        return false;
    if (!ring)
        ring = 1;
    regs[0x480 / 4] = 59;
    regs[0x484 / 4] = 31;
    regs[0x47c / 4] = ring;
    constexpr uint32_t la[2][3] = {{0x2e8, 0x2e8, 0x2ec}, {0x2f4, 0x2f4, 0x2f8}};
    constexpr uint32_t scaled[2][3] = {{0x690, 0x698, 0x6a0}, {0x694, 0x69c, 0x6a4}};
    for (unsigned head = 0; head != 2; ++head) {
        if (!owned[head])
            continue;
        regs[(dis[head] + 4) / 4] = 59;
        regs[(dis[head] + 8) / 4] = 31;
        regs[dis[head] / 4] = rates[head];
        for (unsigned window = 0; window != 3; ++window) {
            const uint32_t shift = window == 1 ? 16 : 0;
            const uint32_t value = plans[head].allowance[window];
            regs[la[head][window] / 4] = (uint32_t(regs[la[head][window] / 4]) & ~(0xffu << shift)) | (value << shift);
            regs[scaled[head][window] / 4] =
                (uint32_t(regs[scaled[head][window] / 4]) & ~0x00ff00ffu) | value | (value << 16);
        }
    }
    return true;
}
}  // namespace switch_display
