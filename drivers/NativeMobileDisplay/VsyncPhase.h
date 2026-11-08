// SPDX-License-Identifier: BSD-2-Clause-Patent
// The compositor's vsync phase (r17-touch): when, before the frame boundary that latches its flip, the render server is
// told to draw. NativeLegacyDisplay feeds this the times it sees and programs what it decides; nothing here touches
// hardware, so test/vsync_phase_test.cpp runs it against a model compositor.
//
// Why: backboardd draws as soon as its vsync arrives, finishes 2-3 ms in, and its flip then waits for the next boundary:
// r8a's quiet windows show latch 12.5-14 ms mean with a tight spread, a whole frame of touch-to-photon spent waiting.
// Told `lead` before the boundary instead, it finishes just before it and the flip goes out at once.
//
// How (all times microseconds of one monotonic clock):
//   * vsync(t)      a notification went to the render server at t, with the lead then in effect (0: at V_BLANK).
//   * replaced(...) a flip was replaced before it was shown: with a lead in effect, that frame missed.
//   * present(...)  a flip latched: its swap's submission, when it was ready (the compositor's GPU work done and the
//                   flip asked for) and when it latched. The vsync it answered is the latest one at or before the
//                   submission; response = ready - that vsync. With a lead in effect the flip was meant for the first
//                   boundary after that vsync: latched more than half a frame after it, it missed.
//   * lead()        the lead to program: 0 (off: vsync at V_BLANK, as before r17) until a window of kWindow responses
//                   says the larger of p99 + margin and the longest response of the last ~32 s + kMarginUs fits with
//                   kMinSaving to spare (so a stall the frame would have absorbed without a phase is absorbed with one),
//                   and above any response that missed, a floor that comes down 1 ms a minute without misses. It engages at the most conservative lead that saves
//                   kMinSaving, comes down half the way (at least kStepUs) a window towards p99 + margin, and grows at
//                   once when responses do. A miss turns it off at once (the next vsync is at V_BLANK again), holds it
//                   off for kHoldOffUs and adds kMarginStepUs to the margin (up to kMaxMarginUs): a phase never costs more
//                   than one late frame per hold-off, and the margin learns from each.
// It also tells a long present's cause apart (paced()): a swap within half a frame of the vsync it answered came promptly,
// so the frames before it had nothing new to show (a client drawing at 20 Hz, r8a's clock widget), not a late compositor.
#pragma once
#include <stdint.h>

class VsyncPhase {
public:
    static constexpr unsigned kWindow = 120;     // responses per decision
    static constexpr uint32_t kMarginUs = 1500;  // over the window's p99, at first
    static constexpr uint32_t kMarginStepUs = 1000, kMaxMarginUs = 5000;
    static constexpr uint32_t kMinLeadUs = 2500;         // never asked to draw in less
    static constexpr uint32_t kMinSavingUs = 2000;       // a phase that saves less is not worth one late frame
    static constexpr uint32_t kStepUs = 1000;            // the most a lead shrinks per window
    static constexpr uint64_t kHoldOffUs = 10000000;     // after a miss
    static constexpr unsigned kBins = 64;                // 0.5 ms response bins, 32 ms
    static constexpr uint64_t kFloorDecayUs = 60000000;  // a miss's floor comes down 1 ms a minute without misses

    explicit VsyncPhase(uint32_t periodUs = 16667) : period(periodUs)
    {}

    void setPeriod(uint32_t periodUs)
    {
        if (periodUs > 4000 && periodUs < 100000)
            period = periodUs;
    }

    // A vsync notification went out at t with lead `inEffect` (0: at V_BLANK).
    void vsync(uint64_t t, uint32_t inEffect)
    {
        sent[sentAt] = {t, inEffect};
        sentAt = (sentAt + 1) % kSent;
    }

    struct Outcome {
        bool answered;  // a vsync was found for it
        bool missed;    // meant for the boundary after its vsync, latched a frame later
        bool paced;     // submitted within half a frame of its vsync
        uint32_t responseUs;
    };

    // A flip latched at `latch`; its swap was submitted at `submit` and its flip asked for at `ready`.
    Outcome present(uint64_t submit, uint64_t ready, uint64_t latch)
    {
        Outcome out = {};
        const Sent *v = answered(submit);
        if (!v || ready < v->t || latch < ready)
            return out;
        out.answered = true;
        out.responseUs = static_cast<uint32_t>(ready - v->t < 0xffffffffu ? ready - v->t : 0xffffffffu);
        out.paced = submit - v->t < period / 2;
        if (v->lead && latch - v->t > v->lead + period / 2) {
            out.missed = true;
            miss(latch, out.responseUs);
            return out;
        }
        // A response longer than a frame answered no boundary in particular (the compositor stalled): it says nothing
        // about the phase, but it does say a phase could not have held, so it counts.
        add(out.responseUs);
        if (count >= kWindow) {
            while (missFloor && latch > floorSince + kFloorDecayUs) {
                floorSince += kFloorDecayUs;
                missFloor = missFloor > 1000 ? missFloor - 1000 : 0;
            }
            if (latch >= holdUntil)
                decide();
            clear();
        }
        return out;
    }

    // A flip replaced before it was shown (a newer one latched first, latest frame wins): it missed its boundary, so with a
    // lead in effect it counts as a miss.
    void replaced(uint64_t submit, uint64_t ready, uint64_t t)
    {
        if (!leadUs)
            return;
        const Sent *v = answered(submit);
        miss(t, v && ready > v->t ? static_cast<uint32_t>(ready - v->t) : leadUs + period / 4);
    }

    uint32_t floorUs() const
    {
        return missFloor;
    }

    // Whether a swap submitted at `submit` came within half a frame of the vsync it answered (Outcome::paced).
    bool paced(uint64_t submit) const
    {
        const Sent *v = answered(submit);
        return (v != nullptr) && submit - v->t < period / 2;
    }

    uint32_t lead() const
    {
        return leadUs;
    }

    uint32_t marginUs() const
    {
        return margin;
    }

    uint32_t missCount() const
    {
        return misses;
    }

    uint32_t lastP99() const
    {
        return p99;
    }

private:
    struct Sent {
        uint64_t t;
        uint32_t lead;
    };

    static constexpr unsigned kSent = 8;
    Sent sent[kSent] = {};
    unsigned sentAt = 0;
    uint32_t period;
    uint32_t leadUs = 0, margin = kMarginUs, misses = 0, p99 = 0;
    uint64_t holdUntil = 0;
    uint32_t bins[kBins] = {};
    unsigned count = 0;
    uint32_t over = 0;  // responses past the last bin
    static constexpr unsigned kPeaks = 16;
    uint32_t peaks[kPeaks] = {}, windowPeak = 0;
    unsigned peakAt = 0;
    // The longest response that ever missed, coming down kFloorDecayUs at a time without another miss: a stall rarer than
    // the peaks' ~32 s costs one frame per decay, not one per stall.
    uint32_t missFloor = 0;
    uint64_t floorSince = 0;

    // The vsync a swap submitted at `submit` answered: the latest one at or before it.
    const Sent *answered(uint64_t submit) const
    {
        const Sent *v = nullptr;
        for (const Sent &s : sent) {
            if (s.t && s.t <= submit && (!v || s.t > v->t))
                v = &s;
        }
        return v;
    }

    void miss(uint64_t t, uint32_t responseUs)
    {
        ++misses;
        leadUs = 0;
        holdUntil = t + kHoldOffUs;
        if (margin + kMarginStepUs <= kMaxMarginUs)
            margin += kMarginStepUs;
        notePeak(responseUs);
        if (responseUs > missFloor)
            missFloor = responseUs;
        floorSince = t;
        clear();
    }

    // The longest response of each of the last kPeaks windows (and of each miss): the lead covers the longest of them
    // too, so a stall the frame could absorb without a phase (an 8 ms one in a 16.7 ms frame) is no dropped frame with
    // one, as long as one like it came within the last kPeaks windows (~32 s at 60 Hz).
    void notePeak(uint32_t us)
    {
        if (us > windowPeak)
            windowPeak = us;
    }

    uint32_t recentPeak() const
    {
        uint32_t most = windowPeak;
        for (const uint32_t p : peaks)
            most = p > most ? p : most;
        return most;
    }

    void add(uint32_t us)
    {
        notePeak(us);
        ++count;
        if (us / 500 >= kBins) {
            ++over;
        } else {
            ++bins[us / 500];
        }
    }

    void clear()
    {
        for (uint32_t &b : bins)
            b = 0;
        count = over = 0;
        peaks[peakAt] = windowPeak;
        peakAt = (peakAt + 1) % kPeaks;
        windowPeak = 0;
    }

    void decide()
    {
        const unsigned want = (count * 99 + 99) / 100;
        unsigned seen = 0;
        unsigned bin = 0;
        for (; bin < kBins; ++bin) {
            seen += bins[bin];
            if (seen >= want)
                break;
        }
        p99 = bin < kBins ? (bin + 1) * 500 : 0xffffffffu;
        const uint32_t longest = recentPeak() > missFloor ? recentPeak() : missFloor;
        const uint64_t peak = static_cast<uint64_t>(longest) + kMarginUs;
        const uint64_t need = static_cast<uint64_t>(p99) + margin > peak ? static_cast<uint64_t>(p99) + margin : peak;
        uint32_t target = kMinLeadUs;
        if (need >= kMinLeadUs)
            target = static_cast<uint32_t>(need < 0xffffffffu ? need : 0xffffffffu);
        if (static_cast<uint64_t>(target) + kMinSavingUs > period) {
            leadUs = 0;  // no room: at V_BLANK
            return;
        }
        if (target >= leadUs) {
            // Off: engaged at the most conservative lead that still saves kMinSavingUs, then down a step a window, each
            // step measured where it runs (the compositor's time can depend on where in the frame it draws). On: a
            // longer response is followed at once.
            leadUs = leadUs ? target : period - kMinSavingUs;
            if (leadUs < target)
                leadUs = target;
        } else {
            // Down half the way to the target (at least kStepUs), so a 20 Hz client (6 s windows) settles in a few.
            const uint32_t half = (leadUs - target) / 2;
            leadUs = leadUs - target > kStepUs ? leadUs - (half > kStepUs ? half : kStepUs) : target;
        }
    }
};
