// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include "SwitchDisplayMemory.h"

namespace switch_display {
struct MemoryPlan {
    uint32_t ptsa_rate = 0;
    uint32_t allowance[3] = {};
    uint32_t minimum[3] = {};
};

constexpr uint32_t kMemoryDvfsNs = 2000;

inline uint64_t memoryMin(uint64_t a, uint64_t b)
{
    return a < b ? a : b;
}

inline uint64_t memoryMax(uint64_t a, uint64_t b)
{
    return a > b ? a : b;
}

inline uint32_t memoryMBps(uint64_t bytes)
{
    return uint32_t(bytes / 1000000 + 1);
}

inline uint32_t displayPtsa(uint32_t mbps)
{
    const uint64_t fraction = ((176ULL * mbps * 1100 / 400000) / 10) * 10 / 4;
    return uint32_t(memoryMin((fraction * 4096 + 9999) / 10000, 4095));
}

inline uint64_t memoryBlankBandwidth(uint64_t storage, uint32_t pixelClockHz, uint64_t blankPixels)
{
    const uint64_t denominator = blankPixels * 1000000;
    return (storage * pixelClockHz + denominator - 1) / denominator;
}

// With at most three windows and 1.1 catchup, spool/(own bandwidth * DVFS time) is below 3/5.
// For three windows the bound reduces to 55*x*x - 229*x + 240 > 0 (negative discriminant).
inline uint64_t memorySpoolEnvelope(uint64_t bandwidthMBps, uint32_t dvfsNs)
{
    return dvfsNs ? (bandwidthMBps * dvfsNs * 3 + 4999) / 5000 + 2 : 0;
}

// T210 (major revision 1) or T210B01 (2) with LPDDR4 (EMC_FBIO_CFG5 DRAM_TYPE 1): L4T's T21x latency allowance and
// bandwidth manager take both chips alike and key only on the DRAM type.
inline bool memoryProfile(uint32_t hidrev, uint32_t fbio)
{
    const uint32_t major = (hidrev >> 4) & 0xf;
    return ((hidrev >> 8) & 0xff) == 0x21 && (major == 1 || major == 2) && (fbio & 3) == 1;
}

// T210B01's BPMP rate is the root DRAM clock; the LA equations use half of it.
// Coefficients and the 2us software DVFS allowance follow NVIDIA L4T r32.7.4's T21x display/MC policy.
inline bool memoryPlan(const switch_display_memory_head &head, uint32_t dramKhz, MemoryPlan &out,
                       uint32_t dvfsNs = kMemoryDvfsNs)
{
    if (head.head > 1 || !head.pixel_clock_hz || !head.active_width || !head.active_height ||
        head.active_width > head.total_width || head.active_height > head.total_height || head.total_width > 16384 ||
        head.total_height > 16384 || head.peak_bytes_per_second > 64000000000ULL || head.bandwidth_envelope > 1 ||
        dvfsNs > 1000000)
        return false;
    const uint64_t blank =
        uint64_t(head.total_width) * head.total_height - uint64_t(head.active_width) * head.active_height;
    const uint32_t mhz = dramKhz / 2000;
    if (!blank || mhz < 12 || mhz > 1600)
        return false;
    uint64_t bandwidth[3] = {}, total = 0;
    unsigned active = 0;
    for (unsigned i = 0; i != 3; ++i) {
        const auto &window = head.windows[i];
        if (!window.active)
            continue;
        if (!window.peak_bytes_per_second || window.peak_bytes_per_second > head.peak_bytes_per_second ||
            window.required_buffering_bytes > 1u << 24 || window.latency_buffering_bytes > 1u << 24)
            return false;
        bandwidth[i] = memoryMBps(window.peak_bytes_per_second);
        total += bandwidth[i];
        ++active;
    }
    MemoryPlan result;
    result.ptsa_rate = active ? displayPtsa(memoryMBps(head.peak_bytes_per_second)) : 0;
    constexpr uint32_t lineBuffer[2][3] = {{151552, 112640, 112640}, {112640, 112640, 112640}};
    constexpr uint32_t mccif[2][3] = {{6144, 6144, 11520}, {11520, 6144, 6144}};
    const uint64_t row =
        memoryMin(memoryMin(4096000, 16000 * memoryMin(mhz + 50, 400)), uint64_t(10 * mhz - 54) * 16 * 500);
    const uint64_t drain = row * 1000 / (uint64_t(mhz) * 16 * 500) + 54000 / mhz;
    const uint64_t interruption = memoryMax(2362000 / mhz, drain + 70000 / mhz);
    for (unsigned i = 0; i != 3; ++i) {
        if (!bandwidth[i]) {
            result.allowance[i] = 255;
            continue;
        }
        const auto &window = head.windows[i];
        const uint64_t bw = bandwidth[i];
        const uint64_t storage = lineBuffer[head.head][i] + mccif[head.head][i];
        const uint64_t available = uint64_t(window.latency_buffering_bytes) * 1000;
        const uint64_t disruption = bw * interruption + 500;
        const uint64_t low =
            window.required_buffering_bytes + (disruption > available ? (disruption - available) / 1000 : 0);
        const uint64_t blankDenominator = blank * 1000000;
        const uint64_t blankBandwidth = (storage * head.pixel_clock_hz + blankDenominator - 1) / blankDenominator;
        const uint64_t fill = 1100 * memoryMax(total, blankBandwidth);
        const uint64_t share = fill / active;
        uint64_t spool = 0;
        if (bw * 1000 > share && active > 1) {
            const uint64_t others = total - bw;
            const uint64_t fillOthers = fill * (active - 1) / active;
            if (fillOthers <= others * 1000)
                return false;
            const uint64_t duration = uint64_t(dvfsNs) * others * 1000 / (fillOthers - others * 1000);
            spool = (bw * 1000 - share) * duration / 1000000;
        }
        if (head.bandwidth_envelope)
            spool = memoryMax(spool, memorySpoolEnvelope(bw, dvfsNs));
        const uint64_t threshold = low + bw * dvfsNs / 1000 + spool;
        if (threshold >= storage)
            return false;
        const uint64_t effective = threshold < lineBuffer[head.head][i] ? mccif[head.head][i] : storage - threshold;
        const uint64_t upper = (effective * 1000 / bw) * 1000 / 1100;
        const uint64_t overhead = 158000 / mhz;
        if (upper < overhead)
            return false;
        result.allowance[i] = uint32_t(memoryMin(upper - overhead, 7650) / 30);
        result.minimum[i] = uint32_t((drain + 29) / 30);
        if (result.allowance[i] < result.minimum[i])
            return false;
    }
    out = result;
    return true;
}

inline bool memoryTransition(const MemoryPlan &before, const MemoryPlan &after, MemoryPlan &joined)
{
    joined = after;
    for (unsigned i = 0; i != 3; ++i) {
        joined.allowance[i] = uint32_t(memoryMin(before.allowance[i], after.allowance[i]));
        joined.minimum[i] = uint32_t(memoryMax(before.minimum[i], after.minimum[i]));
        if (joined.allowance[i] < joined.minimum[i])
            return false;
    }
    return true;
}
}  // namespace switch_display
