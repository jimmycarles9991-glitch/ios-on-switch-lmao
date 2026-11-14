// Host test of drivers/NativeMobileDisplay/VsyncPhase.h: the adaptive vsync phase against a model compositor at 60 Hz. Each frame boundary latches the newest flip that was asked for before it.
// The compositor draws when a vsync reaches it (at V_BLANK with no lead, `lead` before the next boundary with one):
// CPU work, then the swap, then GPU work, then the flip; frames whose flip misses its boundary are shown a frame later or
// replaced by the next one. Scenarios: r8a's quiet windows (2-3 ms responses), the same with rare 8 ms stalls, a client
// at 20 Hz (r8a's clock widget), and a compositor that copies (10 ms responses). Checks: latch wait, frames late or lost
// against no phase, the lead it settles on, the misses it takes, and paced().
#include <gtest/gtest.h>

#include <cinttypes>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "../VsyncPhase.h"

// Every check is a GoogleTest expectation; its message says what was expected.
#define check(ok, what) EXPECT_TRUE(ok) << (what)

struct Model {
    double cpuMin, cpuMax, gpuMin, gpuMax;  // ms
    double stallEvery = 0, stallMs = 0;     // one stall in `stallEvery` frames
    unsigned drawEvery = 1;                 // 3: a client at 20 Hz
};

struct Result {
    double latchMeanMs = 0, swapToScanMeanMs = 0;
    unsigned shown = 0, late = 0, lost = 0, misses = 0, pacedLong = 0, longPresents = 0;
    uint32_t finalLead = 0;
};

static Result run(const Model &m, bool phase, double seconds, uint64_t seed)
{
    constexpr uint64_t T = 16667;  // us
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(0, 1);
    VsyncPhase vp(T);
    Result r;

    struct Flip {
        uint64_t submit, ready, boundary;  // boundary: the one it was drawn for
        bool live;
    };

    std::vector<Flip> pending;
    double latchSum = 0, s2sSum = 0;
    uint64_t lastPresent = 0;
    const uint64_t frames = (uint64_t)(seconds * 1e6 / T);
    for (uint64_t f = 1; f < frames; ++f) {
        const uint64_t boundary = f * T;  // frame end f: latches flips asked for before it
        const uint32_t lead = phase ? vp.lead() : 0;
        // The vsync for this boundary: lead before it, or at the previous boundary (V_BLANK) with none.
        const uint64_t vsync = lead ? boundary - lead : boundary - T;
        vp.vsync(vsync, lead);
        if (f % m.drawEvery == 0) {
            double cpu = m.cpuMin + (m.cpuMax - m.cpuMin) * u(rng), gpu = m.gpuMin + (m.gpuMax - m.gpuMin) * u(rng);
            if (m.stallEvery && u(rng) < 1.0 / m.stallEvery)
                cpu += m.stallMs;
            const uint64_t submit = vsync + (uint64_t)(cpu * 1000), ready = submit + (uint64_t)(gpu * 1000);
            pending.push_back({submit, ready, lead ? boundary : boundary, true});
        }
        // Boundary f latches the newest flip ready before it; older ready ones are replaced (latest frame wins).
        int newest = -1;
        for (int i = 0; i < (int)pending.size(); ++i)
            if (pending[i].live && pending[i].ready < boundary)
                newest = i;
        if (newest >= 0) {
            for (int i = 0; i < newest; ++i)
                if (pending[i].live) {
                    pending[i].live = false;
                    ++r.lost;
                    if (phase)
                        vp.replaced(pending[i].submit, pending[i].ready, boundary);  // NativeLegacyDisplay's stReplaced
                }
            Flip &fl = pending[newest];
            fl.live = false;
            ++r.shown;
            const uint64_t intended = phase && lead ? fl.boundary : fl.boundary;  // the boundary it was drawn for
            if (boundary > intended)
                ++r.late;
            latchSum += (double)(boundary - fl.ready) / 1000;
            s2sSum += (double)(boundary - fl.submit) / 1000;
            const VsyncPhase::Outcome o = vp.present(fl.submit, fl.ready, boundary);
            if (lastPresent && boundary - lastPresent > T * 3 / 2) {
                ++r.longPresents;
                if (o.paced)
                    ++r.pacedLong;
            }
            lastPresent = boundary;
        }
        std::vector<Flip> keep;
        for (const Flip &fl : pending)
            if (fl.live)
                keep.push_back(fl);
        pending.swap(keep);
    }
    r.latchMeanMs = r.shown ? latchSum / r.shown : 0;
    r.swapToScanMeanMs = r.shown ? s2sSum / r.shown : 0;
    r.misses = vp.missCount();
    r.finalLead = vp.lead();
    return r;
}

static void report(const char *name, const Result &off, const Result &on)
{
    std::printf("%-16s off: latch %.1f ms, swap-to-scan %.1f ms, shown %u late %u lost %u | phase: latch %.1f ms, "
                "swap-to-scan %.1f ms, shown %u late %u lost %u, lead %u us, misses %u\n",
                name, off.latchMeanMs, off.swapToScanMeanMs, off.shown, off.late, off.lost, on.latchMeanMs,
                on.swapToScanMeanMs, on.shown, on.late, on.lost, on.finalLead, on.misses);
}

// r8a quiet windows: queue ~20 us, copy (the compositor's GPU render) 0.1-0.7 ms, and its CPU 1.5-2.5 ms.
static const Model kQuiet = {1.5, 2.5, 0.1, 0.7};

TEST(VsyncPhase, Quiet)
{
    const Result off = run(kQuiet, false, 120, 1), on = run(kQuiet, true, 120, 1);
    report("quiet", off, on);
    check(off.latchMeanMs > 12 && off.latchMeanMs < 15, "quiet, no phase: latch ~13 ms as r8a");
    check(on.latchMeanMs < 4.5, "quiet, phase: latch under 4.5 ms (from ~13)");
    check(on.finalLead >= 3000 && on.finalLead <= 5500, "quiet: lead settles at p99 + margin, 3-5.5 ms");
    check(on.misses == 0 && on.late == 0 && on.lost == 0, "quiet: no frame late or lost");
    check(on.shown + 2 >= off.shown, "quiet: as many frames shown");
}

// The same with an 8 ms stall one frame in 500: the phase takes at most one late frame per hold-off and backs off.
TEST(VsyncPhase, Stalls)
{
    const Model stalls = {1.5, 2.5, 0.1, 0.7, 500, 8};
    const Result off = run(stalls, false, 120, 2), on = run(stalls, true, 120, 2);
    report("stalls", off, on);
    check(on.misses <= 1, "stalls: the first stall is the only miss (its response joins the peak)");
    check(on.late + on.lost <= off.late + off.lost + on.misses, "stalls: frames lost only to the misses");
    check(on.latchMeanMs < off.latchMeanMs - 2, "stalls: the phase still saves part of the wait");
    check(on.finalLead >= 10000, "stalls: the lead covers the 10.5 ms stalls");
}

// An 8 ms stall once a minute (rarer than the ~32 s the window peaks remember): the miss floor keeps it to one miss.
TEST(VsyncPhase, RareStalls)
{
    const Model rare = {1.5, 2.5, 0.1, 0.7, 3600, 8};
    const Result off = run(rare, false, 300, 6), on = run(rare, true, 300, 6);
    report("rare stalls", off, on);
    check(on.misses <= 2, "rare stalls: at most two misses in 5 minutes");
    check(on.latchMeanMs < off.latchMeanMs - 2, "rare stalls: the phase still saves part of the wait");
}

// A client at 20 Hz: three frames apart, every swap prompt after its vsync: paced, not a late compositor.
TEST(VsyncPhase, TwentyHzClient)
{
    Model widget = kQuiet;
    widget.drawEvery = 3;
    const Result off = run(widget, false, 90, 3), on = run(widget, true, 90, 3);
    report("20 Hz client", off, on);
    check(off.longPresents > 1000 && off.pacedLong == off.longPresents, "20 Hz: every long present is paced");
    check(on.latchMeanMs < 6 && on.misses == 0 && on.finalLead <= 5500, "20 Hz: the phase works for it too");
}

// A compositor that copies (9-12 ms responses): room for a short saving only; no frame late or lost for it.
TEST(VsyncPhase, CopyingCompositor)
{
    const Model copy = {1.0, 2.0, 8.0, 10.0};
    const Result off = run(copy, false, 60, 4), on = run(copy, true, 60, 4);
    report("copying", off, on);
    check(on.misses == 0 && on.late == 0 && on.lost == 0 && on.shown == off.shown, "copying: no frame late or lost");
    check(on.latchMeanMs <= off.latchMeanMs, "copying: never a longer wait");
}

// A compositor slower than the frame allows (14-16 ms): no phase at all.
TEST(VsyncPhase, SlowCompositor)
{
    const Model slow = {2.0, 3.0, 12.0, 13.0};
    const Result on = run(slow, true, 60, 5);
    check(on.finalLead == 0 && on.misses == 0, "slow: the phase stays off");
}

// V_PULSE2 programmed, routed and never seen is one failure per probe; nothing is held: once the next lead programs it
// again, the next probe runs at once. A V_PULSE2 seen keeps the phase running.
TEST(VsyncPhaseProbe, ANeverSeenVpulse2FailsThatProbeAndTheNextProbeRunsAtOnce)
{
    VsyncPhaseProbe probe;
    for (unsigned round = 0; round < 3; ++round) {
        probe.programmed();
        for (unsigned i = 1; i < VsyncPhaseProbe::kFlips; ++i)
            EXPECT_EQ(probe.flip(true, true, false), VsyncPhaseProbe::kRun);
        EXPECT_EQ(probe.flip(true, true, false), VsyncPhaseProbe::kFailed);
        EXPECT_EQ(probe.flip(false, false, false), VsyncPhaseProbe::kRun);
    }
    VsyncPhaseProbe seen;
    for (unsigned i = 0; i < 100; ++i)
        EXPECT_EQ(seen.flip(true, true, true), VsyncPhaseProbe::kRun);
}

// V_PULSE2 masked (no vsync client routes it) cannot reach the CPU: a flip then is no probe sample and never a failed
// probe, however many there are; it says the precondition is unmet, and a routed probe after it starts from zero.
TEST(VsyncPhaseProbe, AnUnroutedVpulse2IsAPreconditionNotAFailedProbe)
{
    VsyncPhaseProbe probe;
    probe.programmed();
    for (unsigned i = 0; i < 3 * VsyncPhaseProbe::kFlips; ++i)
        EXPECT_EQ(probe.flip(true, false, false), VsyncPhaseProbe::kUnrouted);
    for (unsigned i = 1; i < VsyncPhaseProbe::kFlips; ++i)
        EXPECT_EQ(probe.flip(true, true, false), VsyncPhaseProbe::kRun);
    EXPECT_EQ(probe.flip(true, false, false), VsyncPhaseProbe::kUnrouted);
    for (unsigned i = 1; i < VsyncPhaseProbe::kFlips; ++i)
        EXPECT_EQ(probe.flip(true, true, false), VsyncPhaseProbe::kRun) << "counted again from zero";
    EXPECT_EQ(probe.flip(true, true, false), VsyncPhaseProbe::kFailed);
}
