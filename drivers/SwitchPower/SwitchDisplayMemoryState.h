// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchDisplayMemoryRegisters.h"

namespace switch_display {
class MemoryState {
public:
    switch_display_memory_head heads[2] = {};
    bool owned[2] = {};

    bool same(const switch_display_memory_head &request) const
    {
        if (request.head > 1 || !owned[request.head])
            return false;
        const auto &old = heads[request.head];
        if (old.pixel_clock_hz != request.pixel_clock_hz || old.active_width != request.active_width ||
            old.active_height != request.active_height || old.total_width != request.total_width ||
            old.total_height != request.total_height || old.peak_bytes_per_second != request.peak_bytes_per_second ||
            old.bandwidth_envelope != request.bandwidth_envelope)
            return false;
        for (unsigned i = 0; i != 3; ++i) {
            const auto &a = old.windows[i], &b = request.windows[i];
            if (a.active != b.active || a.peak_bytes_per_second != b.peak_bytes_per_second ||
                a.required_buffering_bytes != b.required_buffering_bytes ||
                a.latency_buffering_bytes != b.latency_buffering_bytes)
                return false;
        }
        return true;
    }

    bool calculate(uint32_t dramKhz, MemoryPlan out[2]) const
    {
        for (unsigned head = 0; head != 2; ++head)
            if (owned[head] && !memoryPlan(heads[head], dramKhz, out[head]))
                return false;
        return true;
    }

    bool transition(uint32_t beforeKhz, uint32_t afterKhz, MemoryPlan out[2]) const
    {
        MemoryPlan before[2], after[2];
        if (!calculate(beforeKhz, before) || !calculate(afterKhz, after))
            return false;
        for (unsigned head = 0; head != 2; ++head)
            if (owned[head] && !memoryTransition(before[head], after[head], out[head]))
                return false;
        return true;
    }

    unsigned floor(const uint32_t *rates, unsigned count, unsigned first, uint32_t currentKhz) const
    {
        for (unsigned row = first; row < count; ++row) {
            MemoryPlan plans[2];
            if (transition(currentKhz, rates[row], plans))
                return row;
        }
        return count;
    }

    template <class Registers> bool program(Registers &regs, uint32_t beforeKhz, uint32_t afterKhz) const
    {
        if (!owned[0] && !owned[1])
            return true;
        MemoryPlan plans[2];
        return transition(beforeKhz, afterKhz, plans) && memoryProgram(regs, plans, owned);
    }
};
}  // namespace switch_display
