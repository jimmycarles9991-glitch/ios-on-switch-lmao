// drivers/TegraDisplayFlip/TegraScanoutRing.h on the host, against a model of DC A's window A latch: an assembly START_ADDR, the
// active one, and ACT_REQ, which a frame boundary consumes by copying assembly to active. The driver side does what
// NativeLegacyDisplay does under its gate (swaps arrive and are kicked, copies complete and flip, frame-end actions check the
// latch), in random order against boundaries that may fall anywhere, including between a flip's START_ADDR store and its
// ACT_REQ store, and several of which may pass before the frame-end action runs (folded frame ends). Checked:
//  - no frame is ever written into a buffer DC A scans or could take at the next boundary;
//  - a buffer being copied into is never scanned;
//  - a frame whose flip was complete before a boundary is on screen from that boundary on (or a newer one is);
//  - latch() reads every hardware state right (never inconsistent; taken means scanned);
//  - with two buffers it is plain double buffering: nothing is replaced and frames wait for the latch;
//  - against a controller that ignores a START_ADDR store while a request is out (one way DC A could differ from the model),
//    each replacement it does not take reads as inconsistent, recover() puts that flip out double-buffered, replacement is
//    back at the next plain latch, and every check above but the latest-frame one keeps holding, with presents going on.
#include <gtest/gtest.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "../TegraScanoutRing.h"

namespace {
__attribute__((format(printf, 1, 2))) std::string message(const char *format, ...)
{
    char text[512];
    va_list ap;
    va_start(ap, format);
    vsnprintf(text, sizeof(text), format, ap);
    va_end(ap);
    return text;
}

#define CHECK(cond, ...) EXPECT_TRUE(cond) << message(__VA_ARGS__)

uint64_t state = 0x9e3779b97f4a7c15ull;

unsigned rnd(unsigned n)
{
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return (unsigned)(state % n);
}

struct Dc {
    int assembly = 0, active = 0;
    bool request = false;
    bool deviant = false;  // ignores a START_ADDR store while a request is out

    void storeStart(int i)
    {
        if (!(deviant && request))
            assembly = i;
    }

    void boundary()
    {
        if (request) {
            active = assembly;
            request = false;
        }
    }
};

struct Sim {
    TegraScanoutRing ring;
    Dc dc;
    unsigned buffers;
    uint64_t seq[TegraScanoutRing::kMaxBuffers] = {};  // the frame each buffer holds
    uint64_t arrived = 0;                              // swaps so far; a swap's number is its arrival
    uint64_t newestQueued =
        0;                // the newest swap waiting in the family's queue (the older ones are collapsed when it goes)
    unsigned queued = 0;  // swaps waiting in the family's queue
    bool forceAsync = false, torn = true;  // torn: boundaries may land between a flip's stores
    int copying = TegraScanoutRing::kNone;
    bool deferred = false;
    uint64_t readyBefore = 0;        // the newest frame whose flip was complete (both stores) since the last boundary
    uint64_t mustShow = 0;           // the newest frame that must be on screen now
    unsigned boundariesPending = 0;  // boundaries the frame-end action has not handled yet
    uint64_t shown = 0, replaced = 0, collapsed = 0, waited = 0, boundaries = 0, latePresents = 0, holds = 0;
    unsigned recoveries = 0;
    uint64_t shownAtRecovery = 0, replacedAtRecovery = 0, fewestBetweenRecoveries = UINT64_MAX;

    explicit Sim(unsigned n, bool deviant = false) : buffers(n)
    {
        ring.reset(n);
        dc.deviant = deviant;
    }

    bool latchable(int i) const
    {
        return i == dc.active || (dc.request && i == dc.assembly);
    }

    void boundary()
    {
        dc.boundary();
        ++boundaries;
        if (readyBefore > mustShow)
            mustShow = readyBefore;
        CHECK(dc.deviant || seq[dc.active] >= mustShow,
              "boundary %llu scans frame %llu, but frame %llu was ready before it", (unsigned long long)boundaries,
              (unsigned long long)seq[dc.active], (unsigned long long)mustShow);
        CHECK(copying == TegraScanoutRing::kNone || dc.active != copying, "buffer %d scanned while being copied into",
              copying);
        ++boundariesPending;
    }

    // A boundary may land anywhere a store can be torn from the next.
    void maybeBoundary()
    {
        if (torn && !rnd(7))
            boundary();
    }

    void write(int t)
    {
        CHECK(t != TegraScanoutRing::kNone, "no target");
        CHECK(!latchable(t),
              "frame written into buffer %d, which DC A scans (%d) or may take (assembly %d, request %d)", t, dc.active,
              dc.assembly, dc.request);
    }

    // NativeLegacyDisplay::writeFlip: START_ADDR, then UPDATE and ACT_REQ.
    void writeFlip(int t)
    {
        CHECK(ring.replace || !dc.request, "double-buffered, START_ADDR %d stored with a request out", t);
        dc.storeStart(t);
        maybeBoundary();
        dc.request = true;
        if (seq[t] > readyBefore)
            readyBefore = seq[t];
    }

    void flip(int t)
    {
        switch (ring.ready(t)) {
        case TegraScanoutRing::kHold:
            ++holds;
            CHECK(!ring.replace, "a frame held with replacement on");
            return;
        case TegraScanoutRing::kReplace:
            ++replaced;
            CHECK(buffers == 3 && ring.replace, "a flip was replaced with %u buffers or after the fallback", buffers);
            break;
        case TegraScanoutRing::kFlip:
            break;
        }
        writeFlip(t);
    }

    void kick()
    {
        if (copying != TegraScanoutRing::kNone || !queued)
            return;
        const int t = ring.target();
        if (t == TegraScanoutRing::kNone) {
            if (!deferred)
                ++waited;
            deferred = true;
            return;
        }
        if (queued > 1) {  // the stale ones retire unshown
            collapsed += queued - 1;
            queued = 1;
        }
        --queued;
        write(t);
        seq[t] = newestQueued;
        if (forceAsync || rnd(3)) {  // a GPU or copy-thread copy, finished later
            ring.claim(t);
            copying = t;
        } else
            flip(t);
    }

    void arrive()
    {
        newestQueued = ++arrived;
        ++queued;
        kick();
    }

    void copyDone()
    {
        if (copying == TegraScanoutRing::kNone)
            return;
        const int t = copying;
        CHECK(!latchable(t), "copy target %d became latchable before its flip", t);
        copying = TegraScanoutRing::kNone;
        if (!rnd(50)) {  // a failed copy: nothing to show, the next swap goes
            ring.release();
            seq[t] = 0;
        } else
            flip(t);
        kick();
    }

    void frameEndAction()
    {
        if (!boundariesPending)
            return;
        boundariesPending = 0;
        if (ring.pending == TegraScanoutRing::kNone)
            return;
        const int pending = ring.pending, superseded = ring.superseded, front = ring.front;
        const TegraScanoutRing::Latch latch = ring.latch(dc.active, dc.request);
        switch (latch) {
        case TegraScanoutRing::kTaken:
            CHECK(dc.active == pending && ring.front == pending, "taken but DC A scans %d (pending %d)", dc.active,
                  pending);
            ++shown;
            break;
        case TegraScanoutRing::kTakenReplaced:
            // (a deviant controller can leave the replaced address in its assembly copy: the next frame end reads inconsistent)
            CHECK(dc.active == superseded && dc.request && (dc.assembly == pending || dc.deviant),
                  "replaced flip taken: active %d request %d assembly %d (pending %d superseded %d)", dc.active,
                  dc.request, dc.assembly, pending, superseded);
            ++shown;
            break;
        case TegraScanoutRing::kWaiting:
            CHECK(dc.request && dc.active == front, "waiting, but DC A has request %d active %d (front %d)", dc.request,
                  dc.active, front);
            break;
        case TegraScanoutRing::kInconsistent:
            CHECK(dc.deviant && ring.replace,
                  "inconsistent: active %d request %d assembly %d, ring front %d pending %d superseded %d", dc.active,
                  dc.request, dc.assembly, front, pending, superseded);
            if (ring.replace) {  // NativeLegacyDisplay::recoverFlip: one line, then double buffering
                if (recoveries && shown - shownAtRecovery < fewestBetweenRecoveries)
                    fewestBetweenRecoveries = shown - shownAtRecovery;
                ++recoveries;
                shownAtRecovery = shown;
                replacedAtRecovery = replaced;
                const int again = ring.recover(dc.active);
                if (again != TegraScanoutRing::kNone)
                    writeFlip(again);
            }
            return;
        }
        CHECK(ring.front == dc.active || latch == TegraScanoutRing::kWaiting, "ring front %d, DC A scans %d",
              ring.front, dc.active);
        if (latch != TegraScanoutRing::kWaiting) {
            const int held = ring.flipHeld();
            if (held != TegraScanoutRing::kNone)
                writeFlip(held);
        }
        if (latch != TegraScanoutRing::kWaiting && deferred) {
            deferred = false;
            kick();
        }
    }

    void run(unsigned steps)
    {
        for (unsigned i = 0; i < steps; ++i) {
            switch (rnd(10)) {
            case 0:
            case 1:
                boundary();
                if (rnd(4))
                    frameEndAction();  // else folded: another event, or boundary, first
                break;
            case 2:
            case 3:
            case 4:
                arrive();
                break;
            case 5:
            case 6:
            case 7:
                copyDone();
                break;
            default:
                frameEndAction();
                break;
            }
        }
    }
};

// The steady case the ring is for: the compositor draws every frame a few milliseconds after the boundary and the copy is quick,
// but one frame was late. With three buffers the lateness is gone at the next boundary; with two it stays.
unsigned lateFramesAfterOneSlip(unsigned buffers)
{
    Sim s(buffers);
    s.forceAsync = true;
    s.torn = false;
    unsigned late = 0;
    for (unsigned frame = 0; frame < 40; ++frame) {
        // the frame end action first (the latch), then the compositor's swap for this vsync, drawn a few ms after it
        s.frameEndAction();
        s.arrive();
        if (frame != 10)
            s.copyDone();  // frame 10's copy finishes only after the next boundary
        s.boundary();
        if (frame == 10)
            s.copyDone();
        if (frame > 12 && s.seq[s.dc.active] < s.arrived)
            ++late;  // this vsync's frame is not what DC A scans
    }
    return late;
}
}  // namespace

TEST(ScanoutRing, TwoAndThreeBuffers)
{
    for (unsigned buffers = 2; buffers <= 3; ++buffers) {
        for (unsigned seed = 1; seed <= 200; ++seed) {
            state = 0x9e3779b97f4a7c15ull * seed;
            Sim s(buffers);
            s.run(4000);
            CHECK(s.shown > 100, "%u buffers, seed %u: only %llu presents", buffers, seed, (unsigned long long)s.shown);
            if (buffers == 2)
                CHECK(s.replaced == 0, "double buffering replaced a flip");
            if (buffers == 3 && seed == 1)
                std::printf("3 buffers: %llu shown, %llu replaced, %llu collapsed, %llu waited of %llu boundaries\n",
                            (unsigned long long)s.shown, (unsigned long long)s.replaced,
                            (unsigned long long)s.collapsed, (unsigned long long)s.waited,
                            (unsigned long long)s.boundaries);
        }
    }
}

// A controller that ignores a START_ADDR store while a request is out: each mismatch costs that one flip (double-buffered,
// presents going on), replacement is back at the next plain latch, so the deviant controller falls back again and
// again in one run instead of once, and every check above keeps holding.
TEST(ScanoutRing, DeviantControllerCostsOneFlipPerMismatch)
{
    unsigned fewest = UINT32_MAX;
    for (unsigned seed = 1; seed <= 60; ++seed) {
        state = 0x51ed27f1u * seed;
        Sim s(3, true);
        s.run(4000);
        CHECK(s.recoveries >= 10, "seed %u: only %u fallbacks", seed, s.recoveries);
        CHECK(s.shown > 100, "seed %u: only %llu presents", seed, (unsigned long long)s.shown);
        if (s.recoveries < fewest)
            fewest = s.recoveries;
    }
    std::printf("deviant controller: at least %u fallbacks in every run\n", fewest);
}

// One late frame: three buffers recover at the next boundary, two stay a frame behind.
TEST(ScanoutRing, OneLateFrame)
{
    state = 0x9e3779b97f4a7c15ull;
    const unsigned late2 = lateFramesAfterOneSlip(2), late3 = lateFramesAfterOneSlip(3);
    std::printf("after one late frame: %u late frames with two buffers, %u with three\n", late2, late3);
    CHECK(late2 > 20, "two buffers should stay a frame behind after a slip (%u)", late2);
    CHECK(late3 == 0, "three buffers should recover at the next boundary (%u)", late3);
}
