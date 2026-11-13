// SPDX-License-Identifier: BSD-2-Clause-Patent
// A window probe's run (DisplayWindowProbe.h has the plans), one DC A frame end at a time with the driver's I/O kept
// out: Action says what to write. The CRC is read as the SMMU probe reads it (DisplaySmmuProbe.h): 0 is never a sample,
// a sample is two frame ends in a row reading the same value, and the first two frame ends after a latch can still
// carry the previous frame's CRC. A step that never settles is recorded unsettled and the run goes on, so one bad step
// does not hide the others; a step whose registers no frame boundary takes ends the run (restored).
#pragma once
#include <stdint.h>
#include <string.h>

#include "DisplayWindowProbe.h"

namespace display_window_probe {

struct StepResult {
    uint32_t crc = 0;
    bool settled = false;
    unsigned underflows = 0, changes = 0, mismatches = 0;
};

struct Run {
    enum Stage : uint8_t {
        kIdle,
        kWaiting,  // for nothing in flight on windows A, B and C (swaps are held from here)
        kLatch,
        kSettle,
        kHold,
        kRestoreLatch,
        kDone,
    };

    enum Action : uint8_t {
        kNothing,
        kStart,    // save windows A and B and the CRC unit, CSC and taps for the probe, the CRC unit on, step 0
        kShow,     // step `step`'s windows
        kRestore,  // windows A and B and the CRC unit as saved
        kFinish,   // the CSC and taps back, the line, the swaps go
    };

    // The swaps are held from the wait on, and IOMobileFramebuffer cancels its queue after 5 s without a completion:
    // the wait is a second at most.
    static constexpr unsigned kWaitFrames = 60, kLatchFrames = 4, kTransitionFrames = 2, kHoldFrames = 1;
    static constexpr unsigned kZeroReads = 8, kSettleReads = 6;
    static constexpr const char *kNeverQuiet = "the display never went quiet";

    Stage stage = kIdle, failedAt = kIdle;
    const char *why = nullptr;
    unsigned steps = 0, step = 0, frames = 0, latchFrames = 0, wait = 0, zeros = 0, reads = 0, hold = 0;
    uint32_t last = 0;
    StepResult result[kMaxSteps] = {};
    bool restored = false, restoreWritten = false;

    void begin(unsigned count)
    {
        *this = Run();
        steps = count < kMaxSteps ? count : kMaxSteps;
        stage = kWaiting;
    }

    bool busy() const
    {
        return stage >= kWaiting && stage <= kRestoreLatch;
    }

    bool windowsChanged() const
    {
        return stage >= kLatch && stage <= kRestoreLatch;
    }

    // Every DC A frame end while busy(): whether nothing is in flight (read while waiting), whether a request the run
    // wrote is still out (ACT_REQ), the latched CRC, whether window A or B underflowed in the frame that ended, and
    // whether both windows' active registers are the set last written (the step's, or the saved ones once restoring).
    Action frameEnd(bool quiet, bool requestOut, uint32_t crc, bool underflow, bool asWritten)
    {
        ++frames;
        StepResult &r = result[step < kMaxSteps ? step : kMaxSteps - 1];
        switch (stage) {
        case kWaiting:
            if (quiet) {
                stage = kLatch;
                latchFrames = 0;
                return kStart;
            }
            return frames >= kWaitFrames ? fail(kNeverQuiet) : kNothing;
        case kLatch:
            r.underflows += static_cast<unsigned>(underflow);
            if (requestOut)
                return ++latchFrames >= kLatchFrames ? fail("a window did not take the step's registers") : kNothing;
            stage = kSettle;
            wait = kTransitionFrames;
            zeros = reads = 0;
            last = 0;
            return kNothing;
        case kSettle:
            r.underflows += static_cast<unsigned>(underflow);
            r.mismatches += static_cast<unsigned>(!asWritten);
            if (wait != 0) {
                --wait;
                return kNothing;
            }
            return settle(r, crc);
        case kHold:
            r.underflows += static_cast<unsigned>(underflow);
            r.mismatches += static_cast<unsigned>(!asWritten);
            r.changes += static_cast<unsigned>(crc != r.crc);
            return --hold == 0 ? next() : kNothing;
        case kRestoreLatch:
            if (requestOut && ++latchFrames < kLatchFrames)
                return kNothing;
            restored = !requestOut && asWritten;
            if (!restored && !why) {
                why = requestOut ? "the windows did not take their registers back"
                                 : "the windows' registers are not the saved ones";
                failedAt = stage;
            }
            stage = kDone;
            return kFinish;
        default:
            return kNothing;
        }
    }

    Action settle(StepResult &r, uint32_t crc)
    {
        if (!crc) {
            last = 0;
            return ++zeros >= kZeroReads ? fail("the CRC unit reads 0") : kNothing;
        }
        if (crc != last) {
            last = crc;
            r.crc = crc;
            return ++reads >= kSettleReads ? next() : kNothing;
        }
        r.crc = crc;
        r.settled = true;
        stage = kHold;
        hold = kHoldFrames;
        return kNothing;
    }

    Action next()
    {
        if (++step < steps) {
            stage = kLatch;
            latchFrames = 0;
            return kShow;
        }
        step = steps - 1;
        return restore();
    }

    Action restore()
    {
        stage = kRestoreLatch;
        latchFrames = 0;
        restoreWritten = true;
        return kRestore;
    }

    Action fail(const char *reason)
    {
        why = reason;
        failedAt = stage;
        if (stage == kWaiting) {
            stage = kDone;
            return kFinish;
        }
        return restore();
    }

    // The display stops, sleeps or goes off: no frame end will come. True when the saved registers must be written now;
    // the driver then finishes without waiting for their latch.
    bool abort(const char *reason)
    {
        if (!busy())
            return false;
        if (!why) {
            why = reason;
            failedAt = stage;
        }
        const bool write = windowsChanged() && !restoreWritten;
        restoreWritten = restoreWritten || write;
        stage = kDone;
        return write;
    }

    static const char *name(Stage s)
    {
        static const char *const names[] = {"idle", "waiting", "latch", "settle", "hold", "restore latch", "done"};
        return s <= kDone ? names[s] : "?";
    }
};

enum Verdict : uint32_t {
    kPass = 0,
    kFail = 1,
    kInconclusive = 2,
};

inline const char *verdictName(Verdict v)
{
    return v == kPass ? "PASS" : v == kFail ? "FAIL" : "INCONCLUSIVE";
}

struct Finding {
    Verdict verdict = kInconclusive;
    const char *note = "";
    int step = -1;  // the step the note is about
};

inline bool stable(const StepResult &r)
{
    return r.settled && !r.changes && !r.mismatches;
}

// The claim: every kEqual step shows its reference's CRC and every control another, each step stable and as written,
// and both windows back as they were. A run that ended early or left the windows changed says nothing; an underflow
// says the window ran short of memory bandwidth, not what it scans (INCONCLUSIVE, with the count).
inline Finding judge(const Plan &plan, const Run &run)
{
    if (run.stage != Run::kDone)
        return {kInconclusive, "the run has not finished"};
    if (run.why && !run.restoreWritten)
        return {kInconclusive, run.why};
    if (!run.restored)
        return {kFail, run.why ? run.why : "the windows were not restored"};
    if (run.why)
        return {kInconclusive, run.why};
    for (unsigned i = 0; i < plan.steps; ++i) {
        const Step &s = plan.step[i];
        if (s.check != kEqual && s.check != kDiffer)
            continue;
        if (!stable(run.result[i]) || !stable(run.result[s.against]))
            return {kFail, "a step was unstable or not as written",
                    static_cast<int>(stable(run.result[i]) ? s.against : i)};
    }
    for (unsigned i = 0; i < plan.steps; ++i) {
        if (run.result[i].underflows)
            return {kInconclusive, "a window underflowed", static_cast<int>(i)};
    }
    for (unsigned i = 0; i < plan.steps; ++i) {
        const Step &s = plan.step[i];
        if (s.check == kDiffer && run.result[i].crc == run.result[s.against].crc)
            return {kInconclusive, "the control showed the same CRC: the CRC did not see the difference",
                    static_cast<int>(i)};
    }
    for (unsigned i = 0; i < plan.steps; ++i) {
        const Step &s = plan.step[i];
        if (s.check == kEqual && run.result[i].crc != run.result[s.against].crc)
            return {plan.mismatchFails ? kFail : kInconclusive, "a step's CRC is not its reference's",
                    static_cast<int>(i)};
    }
    return {kPass, ""};
}

inline void copyName(char *out, size_t size, const char *name)
{
    size_t n = 0;
    for (; name && name[n] && n + 1 < size; ++n)
        out[n] = name[n];
    out[n] = 0;
}

// The reply HW Test reads for the last run of `plan`.
inline void replyOf(const Plan &plan, const Run &run, SwitchDisplayWindowProbeReply &reply)
{
    memset(&reply, 0, sizeof(reply));
    reply.magic = SWITCH_DISPLAY_WINDOW_PROBE_MAGIC;
    reply.probe = plan.probe;
    reply.state = run.stage == Run::kDone ? SWITCH_DISPLAY_WINDOW_PROBE_DONE
                  : run.busy()            ? SWITCH_DISPLAY_WINDOW_PROBE_RUNNING
                                          : SWITCH_DISPLAY_WINDOW_PROBE_IDLE;
    const Finding finding = judge(plan, run);
    reply.verdict = finding.verdict;
    reply.steps = plan.steps;
    reply.restored = run.restored ? 1 : 0;
    reply.frames = run.frames;
    copyName(reply.why, sizeof(reply.why), run.why ? run.why : finding.note);
    for (unsigned i = 0; i < plan.steps && i < kMaxSteps; ++i) {
        SwitchDisplayWindowProbeStep &s = reply.step[i];
        const StepResult &r = run.result[i];
        s.crc = r.crc;
        s.settled = r.settled ? 1 : 0;
        s.underflows = static_cast<uint8_t>(r.underflows > 255 ? 255 : r.underflows);
        s.changes = static_cast<uint8_t>(r.changes > 255 ? 255 : r.changes);
        s.mismatches = static_cast<uint8_t>(r.mismatches > 255 ? 255 : r.mismatches);
        s.check = plan.step[i].check;
        s.against = plan.step[i].against;
        copyName(s.name, sizeof(s.name), plan.step[i].name);
    }
}

}  // namespace display_window_probe
