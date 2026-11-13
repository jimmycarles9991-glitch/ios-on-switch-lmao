// SPDX-License-Identifier: BSD-2-Clause-Patent
// The DEVELOPMENT line for host1x's command channels, one per 5 s window with a submission. Per channel (NVDEC, VIC,
// NVENC, NVJPG): gathered, jobs host1x read from the arena where their engine wrote them; copied, one-off streams
// copied into it (tegra_host1x_submit_on); arena_high, the most of the arena the jobs in flight held; room_waits,
// submits that slept until earlier jobs' fences gave room back. Kept under the channels' lock.
#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

struct SwitchHost1xGatherStats {
    static constexpr uint64_t kWindowUs = 5000000;
    static constexpr uint32_t kChannels = 4;
    uint32_t gathered[kChannels], copied[kChannels], highWords[kChannels], roomWaits[kChannels];
    uint64_t windowStart;

    void submitted(uint32_t channel, bool copy, uint32_t heldWords)
    {
        (copy ? copied : gathered)[channel]++;
        if (heldWords > highWords[channel])
            highWords[channel] = heldWords;
    }

    void waited(uint32_t channel)
    {
        roomWaits[channel]++;
    }

    // The line once the window has run its 5 s, and the counts start again; false otherwise.
    bool line(uint64_t nowUs, char *out, size_t size)
    {
        if (!windowStart)
            windowStart = nowUs;
        if (nowUs - windowStart < kWindowUs)
            return false;
        uint32_t kb[kChannels];
        for (uint32_t i = 0; i < kChannels; ++i)
            kb[i] = (highWords[i] * 4 + 1023) / 1024;
        snprintf(out, size,
                 "TegraHost1x: %llus NVDEC/VIC/NVENC/NVJPG gathered=%u/%u/%u/%u copied=%u/%u/%u/%u "
                 "arena_high=%u/%u/%u/%uKB room_waits=%u/%u/%u/%u",
                 static_cast<unsigned long long>((nowUs - windowStart) / 1000000), gathered[0], gathered[1],
                 gathered[2], gathered[3], copied[0], copied[1], copied[2], copied[3], kb[0], kb[1], kb[2], kb[3],
                 roomWaits[0], roomWaits[1], roomWaits[2], roomWaits[3]);
        *this = {};
        windowStart = nowUs;
        return true;
    }
};
