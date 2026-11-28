// SPDX-License-Identifier: BSD-2-Clause-Patent
// The DEVELOPMENT line for host1x's command channels, one per 5 s window with a submission. Per channel (NVDEC, VIC,
// NVENC, NVJPG, the display's DC): gathered, jobs host1x read from the arena where their engine wrote them; copied,
// one-off streams copied into it (tegra_host1x_submit_on); arena_high, the most of the arena the jobs in flight held;
// room_waits, submits that slept until earlier jobs' fences gave room back; clocked, the host1x clocks each channel's
// tick counter counted, and stalled, those it spent waiting on a syncpoint (TRM 14.4.4, p644), in microseconds. Stalled
// is counted once per wait however many queued jobs sit behind it, so it is a share of clocked, never subtracted from
// a job's submit-to-done time. Kept under the channels' lock.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

struct SwitchHost1xChannelCounters {
    uint64_t ticks, stalled;
};

struct SwitchHost1xGatherStats {
    static constexpr uint64_t kWindowUs = 5000000;
    static constexpr uint32_t kChannels = 7;
    uint32_t gathered[kChannels], copied[kChannels], highWords[kChannels], roomWaits[kChannels];
    uint64_t windowStart;
    SwitchHost1xChannelCounters base[kChannels];  // the counters at the last line
    uint32_t clockKHz;

    // Channels programmed: every counter starts again from zero, counting at `khz`.
    void restarted(uint32_t khz)
    {
        for (SwitchHost1xChannelCounters &b : base)
            b = {};
        clockKHz = khz;
    }

    void submitted(uint32_t channel, bool copy, uint32_t heldWords)
    {
        __atomic_add_fetch(&(copy ? copied : gathered)[channel], 1u, __ATOMIC_RELAXED);
        uint32_t cur = __atomic_load_n(&highWords[channel], __ATOMIC_RELAXED);
        while (heldWords > cur && !__atomic_compare_exchange_n(&highWords[channel], &cur, heldWords, true,
                                                               __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
    }

    void waited(uint32_t channel)
    {
        __atomic_add_fetch(&roomWaits[channel], 1u, __ATOMIC_RELAXED);
    }

    bool maybeDue(uint64_t nowUs) const
    {
        const uint64_t start = __atomic_load_n(&windowStart, __ATOMIC_RELAXED);
        return !start || nowUs - start >= kWindowUs;
    }

    bool due(uint64_t nowUs)
    {
        if (!windowStart)
            __atomic_store_n(&windowStart, nowUs, __ATOMIC_RELAXED);
        return nowUs - windowStart >= kWindowUs;
    }

    // The line once the window has run its 5 s, and the counts start again; false otherwise. `counters`: each
    // channel's, read once the line is due, or null while host1x is stopped (the line then says counters=off).
    bool line(uint64_t nowUs, const SwitchHost1xChannelCounters *counters, char *out, size_t size)
    {
        if (!due(nowUs))
            return false;
        uint32_t g[kChannels], c[kChannels], kb[kChannels], w[kChannels];
        for (uint32_t i = 0; i < kChannels; ++i) {
            g[i] = __atomic_exchange_n(&gathered[i], 0u, __ATOMIC_RELAXED);
            c[i] = __atomic_exchange_n(&copied[i], 0u, __ATOMIC_RELAXED);
            kb[i] = (__atomic_exchange_n(&highWords[i], 0u, __ATOMIC_RELAXED) * 4 + 1023) / 1024;
            w[i] = __atomic_exchange_n(&roomWaits[i], 0u, __ATOMIC_RELAXED);
        }
        const int n =
            snprintf(out, size,
                     "TegraHost1x: %llus NVDEC/VIC/NVENC/NVJPG/DC gathered=%u/%u/%u/%u/%u copied=%u/%u/%u/%u/%u "
                     "arena_high=%u/%u/%u/%u/%uKB room_waits=%u/%u/%u/%u/%u",
                     ull((nowUs - windowStart) / 1000000), g[0], g[1], g[2], g[3], g[4], c[0], c[1], c[2], c[3], c[4],
                     kb[0], kb[1], kb[2], kb[3], kb[4], w[0], w[1], w[2], w[3], w[4]);
        const size_t at = n < 0 ? 0 : static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size;
        if (counters && clockKHz) {
            uint64_t clocked[kChannels], stalled[kChannels];
            for (uint32_t i = 0; i < kChannels; ++i) {
                clocked[i] = since(counters[i].ticks, base[i].ticks) * 1000 / clockKHz;
                stalled[i] = since(counters[i].stalled, base[i].stalled) * 1000 / clockKHz;
                base[i] = counters[i];
            }
            snprintf(out + at, size - at, " clocked=%llu/%llu/%llu/%llu/%lluus stalled=%llu/%llu/%llu/%llu/%lluus",
                     ull(clocked[0]), ull(clocked[1]), ull(clocked[2]), ull(clocked[3]), ull(clocked[4]),
                     ull(stalled[0]), ull(stalled[1]), ull(stalled[2]), ull(stalled[3]), ull(stalled[4]));
        } else {
            snprintf(out + at, size - at, " counters=off");
        }
        __atomic_store_n(&windowStart, nowUs, __ATOMIC_RELAXED);
        return true;
    }

private:
    // A counter below its base was zeroed by a restart the stats did not see: it counted from zero since.
    static uint64_t since(uint64_t now, uint64_t then)
    {
        return now >= then ? now - then : now;
    }

    static unsigned long long ull(uint64_t value)
    {
        return static_cast<unsigned long long>(value);
    }
};
