// SPDX-License-Identifier: MIT
// The GPU priority of the process's Metal command queues, combined: the GPU driver keeps one per connection, and a process
// has one connection (TegraGM20BABI.h method 18), so the process's is the most urgent any live queue asked for, for the
// foreground and for a background GPU role each. Metal's values, 0 the most urgent: Core Animation's window server
// renderer asks for 0, its other contexts 1, 2 and 4 (CA::OGL::new_metal_context).
#pragma once
#include <cstdint>

namespace switch_metal {
class QueuePriorities {
public:
    static constexpr uint32_t kMax = 5, kDefault = 2, kDefaultBackground = 4;

    void add(uint32_t priority, uint32_t background)
    {
        foreground_[priority]++;
        background_[background]++;
    }

    void remove(uint32_t priority, uint32_t background)
    {
        foreground_[priority]--;
        background_[background]--;
    }

    uint32_t priority() const
    {
        return mostUrgent(foreground_, kDefault);
    }

    uint32_t backgroundPriority() const
    {
        return mostUrgent(background_, kDefaultBackground);
    }

private:
    static uint32_t mostUrgent(const uint32_t *queues, uint32_t none)
    {
        for (uint32_t p = 0; p <= kMax; p++) {
            if (queues[p])
                return p;
        }
        return none;
    }

    uint32_t foreground_[kMax + 1] = {}, background_[kMax + 1] = {};
};
}  // namespace switch_metal
