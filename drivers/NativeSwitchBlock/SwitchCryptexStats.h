// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: where the cryptex devices' completions spend their time (SwitchCryptex.cpp finishRequest), one os_log
// line per 10 s of reads: the completions and the time in the client's completion (APFS, which hashes the sealed
// volume's blocks there), and the cache hits' copy out of the windows.
#pragma once

#if DEVELOPMENT || DEBUG
#include <kern/clock.h>
#include <os/log.h>
#include <stdint.h>
#include "../../kits/SwitchTime/SwitchTime.h"

namespace switch_cryptex_stats {
constexpr uint64_t kWindowMs = 10000;

struct Counters {
    uint64_t completions, clientAbs, clientMaxAbs;
    uint64_t copiedBytes, copyAbs;
    uint64_t windowStartMs;
};

inline Counters &counters()
{
    static Counters c;
    return c;
}

inline uint64_t take(uint64_t &counter)
{
    return __atomic_exchange_n(&counter, 0, __ATOMIC_RELAXED);
}

inline void report(uint64_t now)
{
    Counters &c = counters();
    uint64_t start = __atomic_load_n(&c.windowStartMs, __ATOMIC_RELAXED);
    if (!start) {
        __atomic_compare_exchange_n(&c.windowStartMs, &start, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        return;
    }
    if (now - start < kWindowMs ||
        !__atomic_compare_exchange_n(&c.windowStartMs, &start, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
        return;
    const uint64_t completions = take(c.completions);
    const uint64_t clientUs = switch_abs_to_us(take(c.clientAbs));
    const uint64_t clientMaxUs = switch_abs_to_us(take(c.clientMaxAbs));
    const uint64_t copiedKiB = take(c.copiedBytes) >> 10;
    os_log(OS_LOG_DEFAULT,
           "SwitchCryptex: %llu ms: %llu completions, client %llu us (max %llu us); copied %llu KiB in %llu us",
           now - start, completions, clientUs, clientMaxUs, copiedKiB, switch_abs_to_us(take(c.copyAbs)));
}

// finishRequest: `copyAbs` spent copying `copied` bytes out of the windows, `clientAbs` in the client's completion.
inline void note(uint64_t copied, uint64_t copyAbs, uint64_t clientAbs)
{
    Counters &c = counters();
    __atomic_fetch_add(&c.completions, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&c.clientAbs, clientAbs, __ATOMIC_RELAXED);
    uint64_t seen = __atomic_load_n(&c.clientMaxAbs, __ATOMIC_RELAXED);
    while (clientAbs > seen &&
           !__atomic_compare_exchange_n(&c.clientMaxAbs, &seen, clientAbs, true, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    }
    if (copied) {
        __atomic_fetch_add(&c.copiedBytes, copied, __ATOMIC_RELAXED);
        __atomic_fetch_add(&c.copyAbs, copyAbs, __ATOMIC_RELAXED);
    }
    report(switch_abs_to_ns(mach_absolute_time()) / NSEC_PER_MSEC);
}
}  // namespace switch_cryptex_stats
#endif
