// SPDX-License-Identifier: BSD-2-Clause-Patent
// Debug kernels: where the cryptex devices' completions spend their time (SwitchCryptex.cpp finishRequest), one os_log
// line per 10 s of reads: the completions and the time in the client's completion (APFS, which hashes the sealed
// volume's blocks there), and the cache hits' copy out of the windows: of it, what the request waited for (a window read
// for it), the bytes read straight into requests' pages, the bytes the card read into windows, and of the card's reads
// those the device put in the high class for no client that asked for it. kern.switch_sd carries the same bytes since
// boot (totals).
#pragma once

#if SWITCH_STATS_IO
#include <kern/clock.h>
#include <os/log.h>
#include <libkern/libkern.h>
#include <stdint.h>
#include "../../kits/SwitchTime/SwitchTime.h"

namespace switch_cryptex_stats {
constexpr uint64_t kWindowMs = 10000;

// `raised`: card bytes the device read in the SD driver's high class for no client that asked for it there (a request
// of a lower class, the boot prefetch's read-ahead).
struct Bytes {
    uint64_t waited, direct, card, raised;
};

struct Counters {
    uint64_t completions, clientAbs, clientMaxAbs;
    uint64_t copiedBytes, copyAbs;
    uint64_t windowStartMs;
    Bytes window, total;
    uint64_t copiedTotal;
    // Since boot: tier-0 requests that raised a fill they joined, doSetPriority calls (XNU's busy-page
    // reprioritisation through IOStorageFamily), and fills their issuer raised after queuing them because a raise came
    // between their plan and their queuing.
    uint64_t joinedRaises, setPriorityCalls, lateRaises;
};

inline Counters &counters()
{
    static Counters c;
    return c;
}

inline void count(uint64_t Counters::*field)
{
    __atomic_fetch_add(&(counters().*field), 1, __ATOMIC_RELAXED);
}

inline uint64_t take(uint64_t &counter)
{
    return __atomic_exchange_n(&counter, 0, __ATOMIC_RELAXED);
}

inline void add(uint64_t Bytes::*field, uint64_t bytes)
{
    Counters &c = counters();
    __atomic_fetch_add(&(c.window.*field), bytes, __ATOMIC_RELAXED);
    __atomic_fetch_add(&(c.total.*field), bytes, __ATOMIC_RELAXED);
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
    const uint64_t copyUs = switch_abs_to_us(take(c.copyAbs));
    os_log(
        OS_LOG_DEFAULT,
        "SwitchCryptex: %llu ms: %llu completions, client %llu us (max %llu us); copied %llu KiB in %llu us, %llu KiB "
        "of it waited for; direct %llu KiB; card into windows %llu KiB; raised to high %llu KiB; raises since boot: "
        "joined %llu, doSetPriority %llu, late %llu",
        now - start, completions, clientUs, clientMaxUs, copiedKiB, copyUs, take(c.window.waited) >> 10,
        take(c.window.direct) >> 10, take(c.window.card) >> 10, take(c.window.raised) >> 10,
        __atomic_load_n(&c.joinedRaises, __ATOMIC_RELAXED), __atomic_load_n(&c.setPriorityCalls, __ATOMIC_RELAXED),
        __atomic_load_n(&c.lateRaises, __ATOMIC_RELAXED));
}

// finishRequest: `copyAbs` spent copying `copied` bytes out of the windows (`waited`: the request waited for a window
// read), `clientAbs` in the client's completion.
inline void note(uint64_t copied, bool waited, uint64_t copyAbs, uint64_t clientAbs)
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
        __atomic_fetch_add(&c.copiedTotal, copied, __ATOMIC_RELAXED);
        __atomic_fetch_add(&c.copyAbs, copyAbs, __ATOMIC_RELAXED);
        if (waited)
            add(&Bytes::waited, copied);
    }
    report(switch_abs_to_ns(mach_absolute_time()) / NSEC_PER_MSEC);
}

// kern.switch_sd's cryptex tokens: bytes since boot copied out of windows, of them waited for, read straight into
// requests' pages, and read by the card into windows.
inline size_t totals(char *out, size_t size)
{
    const Counters &c = counters();
    const int n = snprintf(
        out, size, " cryptex_copied=%llu cryptex_waited=%llu cryptex_direct=%llu cryptex_card=%llu cryptex_raised=%llu",
        __atomic_load_n(&c.copiedTotal, __ATOMIC_RELAXED), __atomic_load_n(&c.total.waited, __ATOMIC_RELAXED),
        __atomic_load_n(&c.total.direct, __ATOMIC_RELAXED), __atomic_load_n(&c.total.card, __ATOMIC_RELAXED),
        __atomic_load_n(&c.total.raised, __ATOMIC_RELAXED));
    return n < 0 ? 0 : (static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1);
}
}  // namespace switch_cryptex_stats
#endif
