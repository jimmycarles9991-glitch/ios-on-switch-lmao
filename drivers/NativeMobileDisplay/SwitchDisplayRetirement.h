// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace switch_display {
struct RetiredFrame {
    uint64_t sequence = 0;
    uint64_t shownAt = 0;
};

class Retirement {
public:
    void observe(uint64_t sequence, uint64_t shownAt)
    {
        if (!sequence || sequence <= retired)
            return;
        if (sequence > pending.sequence)
            pending.sequence = sequence;
        if (shownAt > pending.shownAt)
            pending.shownAt = shownAt;
    }

    RetiredFrame take(uint64_t oldestPending)
    {
        if (!pending.sequence || (oldestPending && oldestPending <= pending.sequence))
            return {};
        const auto ready = pending;
        retired = ready.sequence;
        pending = {};
        return ready;
    }

private:
    RetiredFrame pending;
    uint64_t retired = 0;
};
}  // namespace switch_display
