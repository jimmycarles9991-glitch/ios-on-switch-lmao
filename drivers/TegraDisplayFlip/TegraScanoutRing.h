// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window A's block-linear scanout buffers (NativeLegacyDisplay): which one DC A scans, which one a
// flip asked for, and which one a frame is being written into. Plain state, no IOKit: NativeLegacyDisplay keeps one under its work-loop
// gate, and test/scanout_ring_test.cpp drives it against a model of DC A's frame-boundary latch.
//
// Three buffers, latest frame wins. With two, a frame that came while the previous flip was out could not be written until
// that flip latched (the only other buffer was being scanned): it waited for the frame end, was copied, and latched one frame
// later, and once one frame was late every later one was too, for as long as the compositor kept drawing every frame. With a
// third buffer a frame is copied as soon as it comes, and when its copy is done while an older flip is still out, window A is
// pointed at the newer frame instead: DC A takes whatever START_ADDR its assembly copy holds at the frame boundary, so a frame
// finished before vblank N scans out at N, and the older one is not shown (it was late; the compositor draws by time, so the
// newer frame is the right picture for N).
//
// A replaced flip may still have been taken: the boundary can fall before the new START_ADDR is stored. Its buffer is held
// (superseded) until a frame end shows which buffer window A scans; only then may a frame be written into it. The five roles are
// disjoint: front (scanned), pending (asked for, not seen taken), superseded (asked for, then replaced: maybe scanned), drawing (a
// copy is writing it), held (complete, waiting for a flip; double buffering only). A frame goes only into a buffer in none of them.
//
// Replacing a flip still out is the one thing double buffering never does to DC A. Should the controller not take a replaced
// START_ADDR as modelled (latch() says kInconsistent), recover() turns replacement off for good: from then on a frame finished
// while a flip is out is held and flipped at that flip's latch, and no frame is written while a flip is out, which is double
// buffering (with two buffers, when no third could be had, it is that from the start).
#pragma once

// Direct scanout: entries kMaxBuffers..kMaxEntries-1 are the compositor's own surfaces, mapped into SMMU slots.
// They take the same roles (front, pending, superseded, held) but are never a copy target: target() hands out scan buffers
// only. A frame whose window layout (pitch slot or block-linear buffer, stride) differs from the flip still out never
// replaces it (a boundary between the stores would latch a mix): ready(i, false) holds it for that flip's latch instead.
struct TegraScanoutRing {
    static constexpr int kNone = -1;
    static constexpr unsigned kMaxBuffers = 3, kMaxEntries = 8;
    unsigned buffers = 0;
    int front = 0, pending = kNone, superseded = kNone, drawing = kNone, held = kNone;
    bool replace = true;  // latest frame wins; false after recover(): double buffering

    // count buffers (2 or 3), window A scanning buffer 0, nothing asked for, replacement on.
    void reset(unsigned count)
    {
        buffers = count > kMaxBuffers ? kMaxBuffers : count;
        front = 0;
        pending = superseded = drawing = held = kNone;
        replace = true;
    }

    bool busy(int i) const
    {
        return i == front || i == pending || i == superseded || i == drawing || i == held;
    }

    // The buffer the next frame is written into: the one being drawn, else the first after front that is not busy; kNone when every
    // buffer is busy or, double-buffered, while a flip is out or a frame waits for one: the frame waits for a frame end.
    int target() const
    {
        if (drawing != kNone)
            return drawing;
        if (held != kNone || (!replace && (pending != kNone || superseded != kNone)))
            return kNone;
        for (unsigned k = 1; k < buffers; ++k) {
            const int i = static_cast<int>((static_cast<unsigned>(front) + k) % buffers);
            if (!busy(i))
                return i;
        }
        return kNone;
    }

    // A copy that finishes later (GPU or copy thread) writes i from now on; one that failed leaves nothing in it to show.
    void claim(int i)
    {
        drawing = i;
    }

    void release()
    {
        drawing = kNone;
    }

    enum Ready {
        kFlip,  // point window A at it (no flip was out)
        kReplace,  // point window A at it, replacing a flip not yet seen taken (whose frame is not shown unless the boundary beat
                   // the replacement: latch() says which)
        kHold,  // double-buffered with a flip out: leave window A alone; flipHeld() hands it back at that flip's latch
    };

    // The frame in i is complete; mayReplace false: it may not replace a flip still out (another window layout).
    Ready ready(int i, bool mayReplace = true)
    {
        if (drawing == i)
            drawing = kNone;
        if (pending == kNone) {
            pending = i;
            return kFlip;
        }
        if (!replace || !mayReplace) {
            held = i;  // kNone before: target() hands out no buffer while one is held, and the display waits for none
            return kHold;
        }
        // superseded is kNone here: with it busy as well no buffer was free for i.
        superseded = pending;
        pending = i;
        return kReplace;
    }

    // After a latch, or at an SC7 resume: the held frame, now asked for (the caller points window A at it); kNone when there is none
    // or a flip is still out.
    int flipHeld()
    {
        if (held == kNone || pending != kNone)
            return kNone;
        pending = held;
        held = kNone;
        return pending;
    }

    enum Latch {
        kWaiting,        // nothing taken yet (asked for after the boundary): look again at the next frame end
        kTaken,          // pending is scanned now (front); a superseded buffer is free again, its frame never shown
        kTakenReplaced,  // the replaced flip was taken at this boundary and is scanned now; pending is still out
        kInconsistent,   // window A scans none of what was asked for: the controller did not do what was modelled
    };

    // At a frame end or vblank with a flip out (pending != kNone): window A's active START_ADDR as a buffer index (kNone: none of
    // these buffers) and whether STATE_CONTROL still holds an ACT_REQ. A boundary between the START_ADDR store and the ACT_REQ store
    // takes the new address under the old request and leaves the new request standing, so "taken" is decided by the address alone.
    // Double-buffered (after recover()) it is taken only once ACT_REQ is clear, and then window A must scan the flip asked for; so
    // no START_ADDR is ever stored while a request is out.
    Latch latch(int active, bool requested)
    {
        if (pending == kNone)
            return kWaiting;
        if (!replace) {
            if (requested)
                return active == front || active == pending ? kWaiting : kInconsistent;
            if (active != pending)
                return kInconsistent;
            front = pending;
            pending = superseded = kNone;
            return kTaken;
        }
        if (active == pending) {
            front = pending;
            pending = superseded = kNone;
            return kTaken;
        }
        if (!requested)
            return kInconsistent;
        if (superseded != kNone && active == superseded) {
            front = superseded;
            superseded = kNone;
            return kTakenReplaced;
        }
        return active == front ? kWaiting : kInconsistent;
    }

    // After kInconsistent with replacement on: DC A scans active (kNone: none of these buffers). Replacement is off from here. The
    // buffer to ask for again is returned (the newest complete frame or, when window A scans none of these, the one believed
    // scanned); the caller points window A at it. A replaced buffer it does not scan stays busy until a frame end shows a plain
    // latch, and a copy in flight finishes into its own buffer and is held until then: nothing is written into a buffer DC A may
    // still take.
    int recover(int active)
    {
        replace = false;
        if (active != kNone)
            front = active;
        if (superseded == active)
            superseded = kNone;
        if (pending == active)
            pending = kNone;
        if (pending == kNone && active == kNone)
            pending = front;
        return pending;
    }

    // SC7: the last request is what window A gets back at the resume (its assembly registers are restored as they were); a held
    // frame stays held for flipHeld() at the resume.
    void settle()
    {
        if (pending != kNone)
            front = pending;
        pending = superseded = kNone;
    }
};
