// SPDX-License-Identifier: BSD-2-Clause-Patent
// The host1x flip trial's run (SwitchHost1xFlipDisplay.h drives it at DC A's frame ends, debug kernels): N window A
// flips, each written by a class 0x70 stream on the display's own host1x channel that waits (WAIT_SYNCPT) for its
// producer's syncpoint, instead of by the CPU. The CPU stands in for the producer and raises that syncpoint once the
// stream is queued. The flips alternate between the surface window A scanned at the start and the same pages mapped at
// a second IOVA, so the panel shows the same picture throughout and an even count ends where it began. Each flip is
// judged at the next frame end as the display judges a completion flip: window A's active copy is what was written, or
// its request is still out (a frame more, kLatchFrames at most), or it is refused.
//
// Two probes ride on the run (host1x items 2 and 4, c38-audit/perf1/learn/host1x.md A4 and A6):
// - kWindowIncrements: each stream also raises two syncpoints through window A's own syncpoint client, on REG_WR_SAFE
//   and on LINE_FLIPPED (TRM 24.5.6; the conditions are the same for windows A, B and C). The run reports, per flip,
//   whether each was reached before the frame end that saw the flip latched, at it, later, or never. That says whether
//   the hardware can sign a latch and an old surface's release with no CPU read.
// - kVblankWait: each stream first waits for the vblank syncpoint (9, DC_CMD_CONT_SYNCPT_VSYNC) two vblanks ahead. The
//   run reports the vblank count at the frame end that saw the flip latched, against the one the stream waited for, so
//   the stream can time a present.
//
// A flip host1x will not write (its producer failed, or the channel had not run it by the second frame end) is taken
// back. The channel is torn down first, so the stream can never write behind the gate; a failed producer's syncpoint
// is released only after that, as its engine's cancel releases it; then, unless the display syncpoint says the stream
// ran after all, the gate writes that flip, with one error line. Nothing is retried: a failure the run did not force
// ends it once that flip has latched, and window A goes back to where it started. TRM 24.6.28: ACT_REQ is written
// apart from its UPDATE.
#pragma once
#include <stdint.h>
#include "../TegraHost1x/TegraHost1x.h"

namespace switch_host1x_flip {
// DC A's registers as class 0x70 methods (TRM 24.5.1): window A's own DC_A_WINBUF instance at 0xbc0, never the
// DISPLAY_WINDOW_HEADER alias the gate switches for its reads; STATE_CONTROL; DC_CMD_GENERAL_INCR_SYNCPT, whose
// IMMEDIATE returns the index once DC A has taken every word before it (TRM 24.5.6).
constexpr uint32_t kWinAKind = 0xbcb, kWinAStride = 0xb8a, kWinAHOffset = 0xbc6, kWinAStart = 0xbc0;
constexpr uint32_t kStateControl = 0x041, kIncrSyncpt = 0x000, kWinAIncrSyncpt = 0x008;
constexpr uint32_t kRegWrSafe = 3, kLineFlipped = 4;  // DC_CMD_WIN_A_INCR_SYNCPT conditions (TRM 24.6.4)
constexpr uint32_t kVblankSyncpt = 9, kVblankLead = 2;
constexpr uint32_t kUpdate = (1u << 8) | (1u << 9), kRequest = (1u << 0) | (1u << 1);  // general and window A
constexpr uint32_t kStreamWords = SWITCH_HOST1X_WAIT_WORDS + 1 + 2 * 3 + 1;
constexpr uint32_t kProbeStreamWords = kStreamWords + SWITCH_HOST1X_WAIT_WORDS + 2;
constexpr uint32_t kLatchFrames = 4, kWriteFrames = 2, kWaitFrames = 180, kMaxFlips = 120;
constexpr uint32_t kForcedFailure = 1u << 16;     // with the count: the middle flip's producer fails
constexpr uint32_t kWindowIncrements = 1u << 17;  // window A's REG_WR_SAFE and LINE_FLIPPED increments
constexpr uint32_t kVblankWait = 1u << 18;        // each stream waits kVblankLead vblanks
constexpr uint32_t kModes = kForcedFailure | kWindowIncrements | kVblankWait;

struct Surface {
    uint32_t start, kind, stride, hOffset;
};

struct Active {
    Surface window;
    uint32_t control;
};

enum Latch : uint8_t {
    kOut,
    kAsWritten,
    kNotAsWritten
};

inline bool reached(uint32_t value, uint32_t threshold)
{
    return static_cast<int32_t>(value - threshold) >= 0;
}

inline bool sameSurface(const Surface &a, const Surface &b)
{
    return a.start == b.start && a.kind == b.kind && (a.stride & 0xffff) == (b.stride & 0xffff) &&
           a.hOffset == b.hOffset;
}

inline Latch latchOf(const Surface &written, const Active &active)
{
    if (sameSurface(active.window, written))
        return kAsWritten;
    return (active.control & kRequest) != 0u ? kOut : kNotAsWritten;
}

// What a probe adds to a stream: the vblank it waits for, and window A's two condition syncpoints.
struct Probes {
    bool vblank = false, window = false;
    uint32_t vblankAt = 0, regWrSafe = 0, lineFlipped = 0;
};

// The producer's WAIT_SYNCPT, window A's START_ADDR, its UPDATE then its ACT_REQ, then DC A's increment of the display
// syncpoint. Both surfaces share one layout, so nothing else of window A is written. SETCLASS, INCR and IMM are what
// the class probe proved on T210 (SwitchDcHost1xClassProbe.h).
inline uint32_t stream(uint32_t *w, uint32_t start, uint32_t producer, uint32_t producerAt, uint32_t display,
                       const Probes &probes = {})
{
    uint32_t n = tegra_host1x_op_wait_syncpt(w, producer, producerAt);
    if (probes.vblank)
        n += tegra_host1x_op_wait_syncpt(w + n, kVblankSyncpt, probes.vblankAt);
    w[n++] = tegra_host1x_op_setclass(TEGRA_HOST1X_CLASS_DC, 0, 0);
    const uint32_t writes[3][2] = {{kWinAStart, start}, {kStateControl, kUpdate}, {kStateControl, kRequest}};
    for (const auto &write : writes) {
        w[n++] = tegra_host1x_op_incr(write[0], 1);
        w[n++] = write[1];
    }
    w[n++] = tegra_host1x_op_imm(kIncrSyncpt, (TEGRA_HOST1X_COND_IMMEDIATE << 8) | display);
    if (probes.window) {
        w[n++] = tegra_host1x_op_imm(kWinAIncrSyncpt, (kRegWrSafe << 8) | probes.regWrSafe);
        w[n++] = tegra_host1x_op_imm(kWinAIncrSyncpt, (kLineFlipped << 8) | probes.lineFlipped);
    }
    return n;
}

// When a window condition's syncpoint was first reached, against the frame end that saw its flip latched.
enum Seen : uint8_t {
    kBefore,
    kAtLatch,
    kLater,
    kNever,
    kSeens
};

struct Totals {
    uint32_t host1x, gate, latched, notAsWritten, unwritten, teardowns, afterTeardown, written, underflows, waited;
    uint32_t window[2][kSeens];  // REG_WR_SAFE, LINE_FLIPPED
    uint32_t vblank[4];          // the vblank count at the latch less the one waited for: below it, 0, 1, 2 or more
};

// Io (the display, or a host test's model): quiet, surfaces (nullptr, or why there are none), underflowed, active,
// producer/display (syncpoint ids), producerValue/displayValue, reserve, submit, raiseProducer, releaseProducer,
// teardown, restart, gateWrite, error, latchedByHost1x, finished; for the probes, window(i) and windowValue(i) (i 0:
// REG_WR_SAFE, 1: LINE_FLIPPED), settleWindow (window A's client reset and both syncpoints released to the values
// given), vblankValue.
struct Run {
    enum Stage : uint8_t {
        kIdle,
        kWaiting,
        kFlipping,
        kRestoring,
        kDone
    };

    static constexpr uint32_t kNoFlip = ~0u;

    // A window condition's threshold, the frame end (since its flip's issue) it was first seen reached, and, once its
    // flip latched unreached, the threshold still to be seen.
    struct Mark {
        uint32_t at = 0, late = 0;
        int32_t seen = -1;
        bool pending = false;
    };

    Stage stage = kIdle;
    uint32_t flips = 0, issued = 0, failAt = kNoFlip, modes = 0;
    Totals totals = {};
    Mark marks[2] = {};
    uint32_t windowNext[2] = {}, vblankAt = 0, flipFrames = 0;
    const char *why = nullptr;  // one word: why the run ended short or never flipped
    Surface front = {}, alias = {}, out = {};
    uint32_t producerAt = 0, displayAt = 0, displayStart = 0, frames = 0, writeFrames = 0;
    bool gateOwned = false, written = false, stopping = false, torn = false;

    bool busy() const
    {
        return stage == kWaiting || stage == kFlipping || stage == kRestoring;
    }

    void begin(uint32_t n, uint32_t requested)
    {
        *this = Run();
        stage = kWaiting;
        flips = n;
        modes = requested & kModes;
        failAt = modes & kForcedFailure ? n / 2 : kNoFlip;
    }

    template <class Io> void frameEnd(Io &io)
    {
        if (stage == kWaiting) {
            wait(io);
            return;
        }
        if (stage != kFlipping && stage != kRestoring)
            return;
        if (io.underflowed())
            ++totals.underflows;
        ++flipFrames;
        if (modes & kWindowIncrements)
            watchWindow(io);
        if (!gateOwned && !written) {
            if (!reached(io.displayValue(), displayAt)) {
                if (++writeFrames < kWriteFrames + (modes & kVblankWait ? kVblankLead : 0))
                    return;
                takeBack(io, "host1x had not written it by the second frame end", kUnwritten);
                return;
            }
            written = true;
            ++totals.written;
        }
        judge(io, latchOf(out, io.active()));
    }

    // The display stops, goes off or sleeps: nothing of the stream may reach DC A after this, and window A gets the
    // surface it started on back in its assembly copy.
    template <class Io> void abort(Io &io, const char *reason)
    {
        if (!busy())
            return;
        if (stage != kWaiting) {
            io.teardown();
            io.restart();
            io.gateWrite(front);
        }
        end(io, reason);
    }

private:
    enum Loss : uint8_t {
        kForced,
        kUnraised,
        kUnwritten
    };

    void stop(const char *reason)
    {
        stopping = true;
        if (!why)
            why = reason;
    }

    template <class Io> void wait(Io &io)
    {
        if (!io.quiet()) {
            if (++totals.waited >= kWaitFrames)
                end(io, "busy");
            return;
        }
        if (const char *missing = io.surfaces(front, alias)) {
            end(io, missing);
            return;
        }
        stage = kFlipping;
        displayStart = io.displayValue();
        for (uint32_t i = 0; i < 2; ++i)
            windowNext[i] = io.windowValue(i);
        issue(io);
    }

    template <class Io> void watchWindow(Io &io)
    {
        for (uint32_t i = 0; i < 2; ++i) {
            Mark &m = marks[i];
            const uint32_t value = io.windowValue(i);
            if (m.pending && reached(value, m.late)) {
                ++totals.window[i][kLater];
                m.pending = false;
            }
            if (!gateOwned && m.seen < 0 && m.at && reached(value, m.at))
                m.seen = static_cast<int32_t>(flipFrames);
        }
    }

    void windowAtLatch()
    {
        for (uint32_t i = 0; i < 2; ++i) {
            Mark &m = marks[i];
            if (m.seen >= 0) {
                ++totals.window[i][static_cast<uint32_t>(m.seen) < flipFrames ? kBefore : kAtLatch];
                continue;
            }
            if (m.pending)
                ++totals.window[i][kNever];
            m.pending = true;
            m.late = m.at;
        }
    }

    template <class Io> void judge(Io &io, Latch latch)
    {
        const bool restoring = stage == kRestoring;
        if (latch == kOut) {
            if (++frames < kLatchFrames)
                return;
            io.error(restoring ? "window A did not take the restore" : "window A did not latch it");
            if (restoring) {
                end(io, "restore_not_latched");
                return;
            }
            stop("not_latched");
            restore(io);
            return;
        }
        if (latch == kNotAsWritten) {
            io.error(restoring ? "window A latched something other than the restore"
                               : "window A latched something other than the flip");
            if (restoring) {
                end(io, "restore_not_as_written");
                return;
            }
            ++totals.notAsWritten;
            stop("not_as_written");
            if (gateOwned)
                restore(io);
            else
                gateFlip(io);
            return;
        }
        if (restoring) {
            end(io, why);
            return;
        }
        ++totals.latched;
        if (gateOwned) {
            ++totals.gate;
        } else {
            ++totals.host1x;
            if (torn)
                ++totals.afterTeardown;
            io.latchedByHost1x();
            if (modes & kWindowIncrements)
                windowAtLatch();
            if (modes & kVblankWait) {
                const int32_t after = static_cast<int32_t>(io.vblankValue() - vblankAt);
                ++totals.vblank[after < 0 ? 0 : after > 2 ? 3 : after + 1];
            }
        }
        if (!stopping && issued < flips)
            issue(io);
        else if (out.start == front.start)
            end(io, why);
        else
            restore(io);
    }

    template <class Io> void issue(Io &io)
    {
        const uint32_t index = issued++;
        out = index % 2 ? front : alias;
        frames = writeFrames = flipFrames = 0;
        gateOwned = written = false;
        producerAt = io.producerValue() + 1;
        displayAt = io.displayValue() + 1;
        Probes probes;
        if (modes & kVblankWait) {
            probes.vblank = true;
            probes.vblankAt = vblankAt = io.vblankValue() + kVblankLead;
        }
        if (modes & kWindowIncrements) {
            probes.window = true;
            for (uint32_t i = 0; i < 2; ++i)
                marks[i] = {++windowNext[i], marks[i].late, -1, marks[i].pending};
            probes.regWrSafe = io.window(0);
            probes.lineFlipped = io.window(1);
        }
        uint32_t *words = io.reserve(kProbeStreamWords);
        const int submitted =
            words
                ? io.submit(words, stream(words, out.start, io.producer(), producerAt, io.display(), probes), displayAt)
                : TEGRA_HOST1X_ENOMEM;
        if (submitted) {
            for (uint32_t i = 0; modes & kWindowIncrements && i < 2; ++i) {
                --windowNext[i];
                marks[i].at = 0;
            }
            stop("submit");
            io.error("the display's host1x channel did not take its stream");
            gateFlip(io);
            return;
        }
        if (index == failAt) {
            takeBack(io, "its producer failed", kForced);
            return;
        }
        if (io.raiseProducer(producerAt)) {
            stop("producer");
            takeBack(io, "its producer's syncpoint did not rise", kUnraised);
        }
    }

    // The stream may still be held: the channel stops before anything else, so it cannot write behind the gate.
    template <class Io> void takeBack(Io &io, const char *what, Loss loss)
    {
        io.teardown();
        ++totals.teardowns;
        torn = true;
        const bool ran = reached(io.displayValue(), displayAt);
        // A torn-down stream's window conditions may wait in the client's FIFO: reset it, and count them raised.
        if (modes & kWindowIncrements) {
            io.settleWindow(windowNext[0], windowNext[1]);
            marks[0].at = marks[1].at = 0;
        }
        if (loss == kForced && !ran)
            io.releaseProducer(producerAt);
        io.restart();
        if (ran)
            return;
        if (loss == kUnwritten) {
            ++totals.unwritten;
            stop("unwritten");
        }
        io.error(what);
        gateFlip(io);
    }

    template <class Io> void gateFlip(Io &io)
    {
        gateOwned = true;
        frames = 0;
        io.gateWrite(out);
    }

    template <class Io> void restore(Io &io)
    {
        stage = kRestoring;
        out = front;
        gateFlip(io);
    }

    template <class Io> void end(Io &io, const char *reason)
    {
        if (!why)
            why = reason;
        for (uint32_t i = 0; i < 2; ++i)
            if (marks[i].pending) {
                ++totals.window[i][kNever];
                marks[i].pending = false;
            }
        // A condition still in window A's syncpoint client would wait there for good, and raise its syncpoint whenever
        // its condition comes, on a set another probe may hold by then. Drop it before the channel and the set go.
        if ((modes & kWindowIncrements) && stage != kWaiting &&
            (!reached(io.windowValue(0), windowNext[0]) || !reached(io.windowValue(1), windowNext[1]))) {
            io.teardown();
            io.restart();
            io.settleWindow(windowNext[0], windowNext[1]);
        }
        stage = kDone;
        io.finished();
    }
};
}  // namespace switch_host1x_flip
