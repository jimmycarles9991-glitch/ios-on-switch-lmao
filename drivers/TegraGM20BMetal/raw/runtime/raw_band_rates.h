// SPDX-License-Identifier: MIT
// What a state's draws (or a kernel's dispatches) cost the GPU, learned from GPU time stamps of its own large draws, which the
// band decision (raw_draw_bands.h, raw_dispatch_bands.h) reads in place of the instruction-count estimate once it has one.
// The static estimate prices a draw as pixels x program instructions, so a draw bound by memory or the ROP (a 4x resolve:
// 642 instructions, priced 3 ms, measured 11.5-12 ms) or a looped program is priced far short and kept whole.
//
// A unit is a pixel of the draw's box times its sample passes and instances (a thread of a dispatch). Only a draw alone in
// its pass (a dispatch alone in its encoder) of at least kMeasureUnits units is a candidate: its stamps go where the pass
// already drains the tiled cache and waits (raw_draw_bands.cpp), so a measured draw costs what an unmeasured one does. A
// state's first kWarmSamples candidates, then one in kRemeasureEvery, are measured, counted where they are encoded whatever
// later teaches them; any completion teaches every committed measured draw the GPU has run (Device::learnCompleted). The
// table is per state, its reads and its update relaxed atomics: a racing update loses a sample, never a rate.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>

namespace raw {

constexpr uint64_t kMeasureUnits = 64 * 1024;
constexpr uint32_t kWarmSamples = 3;
constexpr uint32_t kRemeasureEvery = 32;
constexpr uint32_t kMeasuredPerCommandBuffer = 16;
constexpr uint32_t kRateWeight = 4;  // a new sample moves a warm rate by a quarter of the difference

struct BandRate {
    std::atomic<uint32_t> psPerUnit{0};  // 0: not measured yet
    std::atomic<uint32_t> samples{0};
    std::atomic<uint32_t> picks{0};

    BandRate() = default;

    BandRate(const BandRate &o)
    {
        *this = o;
    }

    BandRate &operator=(const BandRate &o)
    {
        psPerUnit.store(o.psPerUnit.load(std::memory_order_relaxed), std::memory_order_relaxed);
        samples.store(o.samples.load(std::memory_order_relaxed), std::memory_order_relaxed);
        picks.store(o.picks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
};

// The work's GPU time: `units` at the learned rate, else the static estimate
inline uint64_t estimatedNs(const BandRate &r, uint64_t units, uint64_t staticNs)
{
    const uint64_t ps = r.psPerUnit.load(std::memory_order_relaxed);
    if (!ps)
        return staticNs;
    return units > UINT64_MAX / ps ? UINT64_MAX / 1000 : units * ps / 1000;
}

inline bool picked(uint32_t pick)
{
    return pick < kWarmSamples || (pick - kWarmSamples) % kRemeasureEvery == kRemeasureEvery - 1;
}

// Whether a candidate of `units` is one to measure; counts it
inline bool measure(BandRate &r, uint64_t units)
{
    return units >= kMeasureUnits && picked(r.picks.fetch_add(1, std::memory_order_relaxed));
}

// Whether the state's next candidate would be measured, without counting it (a dispatch's start stamp goes out before its
// encoder knows the dispatch is alone)
inline bool wouldMeasure(const BandRate &r, uint64_t units)
{
    return units >= kMeasureUnits && picked(r.picks.load(std::memory_order_relaxed));
}

// A measured draw: `ns` of GPU time over `units`
inline void learn(BandRate &r, uint64_t ns, uint64_t units)
{
    if (!units || !ns)
        return;
    const uint64_t sample = std::clamp<uint64_t>(ns * 1000 / units, 1, UINT32_MAX);
    const uint64_t old = r.psPerUnit.load(std::memory_order_relaxed);
    const uint64_t rate = old ? (old * (kRateWeight - 1) + sample) / kRateWeight : sample;
    r.psPerUnit.store(uint32_t(std::max<uint64_t>(rate, 1)), std::memory_order_relaxed);
    r.samples.fetch_add(1, std::memory_order_relaxed);
}

// A measured draw's record, kept by its command buffer until completion: the rate it teaches, its stamp slot and payload,
// and for a debug build's long draw line its state's key and program instructions
struct DrawTiming {
    BandRate *rate = nullptr;
    uint64_t units = 0;
    uint32_t slot = 0, payload = 0;
    uint64_t key = 0;
    uint32_t instructions = 0;
};

constexpr uint64_t kLongDrawNs = 5'000'000;

}  // namespace raw
