// SPDX-License-Identifier: BSD-2-Clause-Patent
// Two-stage bounded register settle for XUDC endpoint state transitions (EP_RELOAD, EP_STCHG, EP_THREAD_ACTIVE,
// EP_STOPPED): immediate check, brief microsecond poll for normal hardware completion, then a gate-releasing sleep
// fallback up to the millisecond bound so other endpoints' transfer events keep draining.
#pragma once
#include <stdint.h>

namespace xudc {

constexpr uint32_t kFastSettleUs = 20;
constexpr uint32_t kStateChangeMs = 1;

template <typename Read, typename NowNs, typename SleepNs>
bool settleRegister(uint32_t mask, uint32_t value, uint32_t fastUs, uint32_t slowMs, Read read, NowNs nowNs,
                    SleepNs sleepNs, uint32_t *seenOut = nullptr)
{
    uint32_t seen = read() & mask;
    if (seen == value) {
        if (seenOut)
            *seenOut = seen;
        return true;
    }
    const uint64_t startNs = nowNs();
    const uint64_t fastDeadline = startNs + static_cast<uint64_t>(fastUs) * 1000ULL;
    while (nowNs() < fastDeadline) {
        seen = read() & mask;
        if (seen == value) {
            if (seenOut)
                *seenOut = seen;
            return true;
        }
    }
    seen = read() & mask;
    if (seen == value) {
        if (seenOut)
            *seenOut = seen;
        return true;
    }
    if (slowMs > 0) {
        const uint64_t slowDeadline = startNs + static_cast<uint64_t>(slowMs) * 1000000ULL;
        for (;;) {
            const uint64_t cur = nowNs();
            if (cur >= slowDeadline)
                break;
            sleepNs(slowDeadline - cur);
            seen = read() & mask;
            if (seen == value) {
                if (seenOut)
                    *seenOut = seen;
                return true;
            }
        }
        seen = read() & mask;
        if (seen == value) {
            if (seenOut)
                *seenOut = seen;
            return true;
        }
    }
    if (seenOut)
        *seenOut = seen;
    return false;
}

}  // namespace xudc
