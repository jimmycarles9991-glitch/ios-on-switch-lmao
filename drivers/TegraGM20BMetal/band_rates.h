// SPDX-License-Identifier: MIT
// What a state's draws (or a kernel's dispatches) cost the GPU, learned from GPU time stamps of its own large draws, which the
// band decision (draw_bands.h, dispatch_bands.h) reads in place of the instruction-count estimate once it has one.
// The static estimate prices a draw as pixels x program instructions, so a draw bound by memory or the ROP (a 4x resolve:
// 642 instructions, priced 3 ms, measured 11.5-12 ms) or a looped program is priced far short and kept whole.
//
// A unit is a pixel of the draw's box times its sample passes and instances (a thread of a dispatch). Only a draw alone in
// its pass (a dispatch alone in its encoder) of at least kMeasureUnits units is a candidate: its stamps go where the pass
// already drains the tiled cache and waits (draw_bands.cpp), so a measured draw costs what an unmeasured one does. A
// state's first kWarmSamples candidates, then one in kRemeasureEvery, are measured, counted where they are encoded whatever
// later teaches them; the next command buffer's begin teaches every committed measured draw the GPU has run
// (Device::learnCompleted). Picks, stamp slots and learning are the encoding thread's alone (the device encodes one command
// buffer at a time), so the pick count is a load and a store; the rate stays atomic for the band decision's relaxed reads.
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <utility>

namespace gm20b {

constexpr uint64_t kMeasureUnits = 64 * 1024;
constexpr uint32_t kWarmSamples = 3;
constexpr uint32_t kRemeasureEvery = 32;
constexpr uint32_t kMeasuredPerCommandBuffer = 16;
constexpr uint32_t kRateWeight = 4;  // a new sample moves a warm rate by a quarter of the difference

struct BandRate {
    std::atomic<uint32_t> psPerUnit{0};  // a unit's share of a draw's time, its pixels' (0: not measured yet)
    // what each band of a banded draw repeats: its vertices and the drain at its end (0: not measured yet)
    std::atomic<uint32_t> nsPerBand{0};
    std::atomic<uint32_t> samples{0};
    std::atomic<uint32_t> picks{0};
    std::atomic<uint32_t> firstBands{0};
    std::atomic<bool> separated{false};
    // learn()'s fit, written by the learning thread alone: moving averages of bands per unit (x) and picoseconds per unit
    // (y) of its samples, a quarter a sample, and of x x x and x x y. Atomic for a copy made on another thread.
    std::atomic<double> x{0}, y{0}, xx{0}, xy{0};

    BandRate() = default;

    BandRate(const BandRate &o)
    {
        *this = o;
    }

    BandRate &operator=(const BandRate &o)
    {
        psPerUnit.store(o.psPerUnit.load(std::memory_order_relaxed), std::memory_order_relaxed);
        nsPerBand.store(o.nsPerBand.load(std::memory_order_relaxed), std::memory_order_relaxed);
        samples.store(o.samples.load(std::memory_order_relaxed), std::memory_order_relaxed);
        picks.store(o.picks.load(std::memory_order_relaxed), std::memory_order_relaxed);
        firstBands.store(o.firstBands.load(std::memory_order_relaxed), std::memory_order_relaxed);
        separated.store(o.separated.load(std::memory_order_relaxed), std::memory_order_relaxed);
        for (auto [to, from] : {std::pair{&x, &o.x}, {&y, &o.y}, {&xx, &o.xx}, {&xy, &o.xy}})
            to->store(from->load(std::memory_order_relaxed), std::memory_order_relaxed);
        return *this;
    }
};

inline uint64_t unitsNs(uint64_t units, uint64_t ps)
{
    return units > UINT64_MAX / ps ? UINT64_MAX / 1000 : units * ps / 1000;
}

// The work's GPU time: `units` at the learned rate, else the static estimate
inline uint64_t estimatedNs(const BandRate &r, uint64_t units, uint64_t staticNs)
{
    const uint64_t ps = r.psPerUnit.load(std::memory_order_relaxed);
    return ps ? unitsNs(units, ps) : staticNs;
}

// Whether any of its draws was measured, whole or banded
inline bool measuredOnce(const BandRate &r)
{
    return r.psPerUnit.load(std::memory_order_relaxed) || r.nsPerBand.load(std::memory_order_relaxed);
}

inline bool picked(uint32_t pick)
{
    return pick < kWarmSamples || (pick - kWarmSamples) % kRemeasureEvery == kRemeasureEvery - 1;
}

// Whether a candidate of `units` is one to measure; counts it where it is large enough to be one
inline bool measure(BandRate &r, uint64_t units)
{
    if (units < kMeasureUnits)
        return false;
    const uint32_t pick = r.picks.load(std::memory_order_relaxed);
    r.picks.store(pick + 1, std::memory_order_relaxed);
    return picked(pick);
}

// Whether the state's next candidate would be measured, without counting it (a dispatch's start stamp goes out before its
// encoder knows the dispatch is alone)
inline bool wouldMeasure(const BandRate &r, uint64_t units)
{
    return units >= kMeasureUnits && picked(r.picks.load(std::memory_order_relaxed));
}

inline void noteProbeBands(BandRate &r, uint32_t bands)
{
    uint32_t expected = 0;
    r.firstBands.compare_exchange_strong(expected, std::max<uint32_t>(bands, 1u), std::memory_order_relaxed);
}

// Before learn() has seen nonzero spread in bands/unit, perturb a repeated measured band count so the fit separates
// per-band WFI/vertex overhead from psPerUnit instead of locking at spread == 0.
inline uint32_t probeBands(const BandRate &r, uint64_t units, uint32_t bands, bool allowWhole = true)
{
    if (bands < 2 || units < kMeasureUnits || r.separated.load(std::memory_order_relaxed))
        return bands;
    if (!picked(r.picks.load(std::memory_order_relaxed)))
        return bands;
    const uint32_t first = r.firstBands.load(std::memory_order_relaxed);
    if (!first)
        return bands;
    const uint32_t margin = first / 5;
    if (bands < first - margin || bands > first + margin)
        return bands;
    if (bands >= 8)
        return (bands * 3) / 4;
    if (bands >= 4)
        return bands / 2;
    if (bands == 3)
        return 2;
    return allowWhole ? 1u : 3u;
}

// A measured draw: `ns` of GPU time over `units`, drawn in `bands` (1: whole). Each band repeats the draw's vertices and
// drains at its end, so a draw takes units x a pixel rate plus bands x a band's cost: booked as pixel cost, the repeat
// priced the state's next draw longer and banded it more (25 Nov: VectorKit's ground draws of 48-208 instructions took
// 8-17 ms in Maps at 1.0-2.9 ms a band, their pixels 0.06-0.8 ms). The two come apart by a line fitted through the
// samples in picoseconds per unit against bands per unit, each sample a quarter of the averages; samples of one band
// count leave the band's cost as it was and move the pixel rate alone. A quarter either way: one sample up by the time
// other channels ran between its stamps (the same draw read 6.0 and 25.3 ms in one second) sets nothing outright.
inline void learn(BandRate &r, uint64_t ns, uint64_t units, uint32_t bands = 1)
{
    if (!units || !ns)
        return;
    const uint32_t b = std::max<uint32_t>(bands, 1u);
    noteProbeBands(r, b);
    const double x = double(b) / double(units), y = double(ns) * 1000.0 / double(units);
    const bool first = r.samples.load(std::memory_order_relaxed) == 0;
    const double keep = first ? 0.0 : double(kRateWeight - 1) / kRateWeight, take = 1.0 - keep;
    const auto average = [keep, take](std::atomic<double> &m, double sample) {
        const double v = m.load(std::memory_order_relaxed) * keep + sample * take;
        m.store(v, std::memory_order_relaxed);
        return v;
    };
    const double mx = average(r.x, x), my = average(r.y, y), mxx = average(r.xx, x * x), mxy = average(r.xy, x * y);
    const double spread = mxx - mx * mx;
    double band = double(r.nsPerBand.load(std::memory_order_relaxed)) * 1000.0;  // picoseconds a band
    if (spread > 1e-6 * mxx) {
        const double slope = (mxy - mx * my) / spread;
        const double maxBand = mx > 0.0 ? my / mx : 0.0;
        band = std::clamp(slope, 0.0, maxBand);
        r.separated.store(true, std::memory_order_relaxed);
    }
    const double pixel = my - band * mx;
    r.psPerUnit.store(uint32_t(std::clamp(pixel, 1.0, double(UINT32_MAX))), std::memory_order_relaxed);
    r.nsPerBand.store(uint32_t(std::min(band / 1000.0, double(UINT32_MAX))), std::memory_order_relaxed);
    r.samples.fetch_add(1, std::memory_order_relaxed);
}

// A measured draw's record, kept by its command buffer until completion: the rate it teaches, its stamp slot and payload,
// and for a debug build's long draw line its state's key, program instructions and bands
struct DrawTiming {
    BandRate *rate = nullptr;
    uint64_t units = 0;
    uint32_t slot = 0, payload = 0;
    uint64_t key = 0;
    uint32_t instructions = 0, bands = 1;
};

constexpr uint64_t kLongDrawNs = 5'000'000;

}  // namespace gm20b
