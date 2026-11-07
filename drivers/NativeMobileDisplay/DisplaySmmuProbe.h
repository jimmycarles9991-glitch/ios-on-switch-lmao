// SPDX-License-Identifier: BSD-2-Clause-Patent
// The direct-scanout gate: does DC A read through the SMMU, and read the
// right bytes? Run once at boot on the frame window A scans anyway, then everything goes back: the swgroup to bypass, window A
// to the buffer's physical address, the CRC unit off, the one pixel it changed restored. Nothing switches to direct scanout
// here; the line it logs is the proof (or the step that failed) the next stage needs.
//
// The only read-back of what DC A scanned is its CRC unit (L4T dc_reg.h DC_COM_CRC_CONTROL 0x300: ALWAYS 3, ACTIVE_DATA 2,
// ENABLE 0; DC_COM_CRC_CHECKSUM_LATCHED 0x329, the CRC of the last whole frame). The register is read at DC A's frame-end
// interrupt, never polled, and it trails: it reads 0 until the unit has latched its first frame (on the Switch: 0 at the second frame
// end after the unit went on, the CRC from the third), and the latch of a frame can come after that frame's own FRAME_END. So a stage
// takes a sample only once the register has settled: two frame ends in a row reading the same CRC, not 0 (0 is never a sample:
// it is the register before its first latch). kZeroReads frame ends of 0 fail as "the CRC unit reads 0", kSettleReads nonzero
// reads that never repeat as "not stable". Each stage reads it from two frame ends after the change took effect:
//   crc    the CRC unit on; then the CRC of the scanned buffer by its physical address: settled (stable over two frames) and
//          not 0
//   pixel  one pixel's low bits changed: the CRC must change (the unit really reads the frame), then restored: back to base
//   attach the display's SMMU domain attached (the scan surfaces mapped 1:1, one IOVA slot below DRAM mapped onto the
//          scanned buffer): the CRC must stay the base (window A still reads its physical address, now through the identity)
//   iova   window A to the slot's IOVA: the CRC must be the base (an address outside DRAM can only read these bytes through
//          the slot's translation); no underflow while it scans through the SMMU
//   back   window A to the physical address again: base; then the swgroup goes back to bypass
// Any other CRC, a latch not seen in kLatchFrames frame ends, an underflow after the attach, or a failed attach ends it at that
// stage: the driver undoes what was done (abort) and logs the one line. Plain state: test/display_probe_test.cpp runs it
// against a model of DC A, its CRC and the SMMU.
#pragma once
#include <stdint.h>

struct DisplaySmmuProbe {
    enum Stage {
        kIdle,
        kCrc,
        kPixel,
        kRestored,
        kAttached,
        kIova,
        kBack,
        kDone,
        kFailed
    };

    enum Action {
        kNothing,
        kTweakPixel,    // change the scanned buffer's first pixel's low bits
        kRestorePixel,  // put it back
        kAttach,        // build and attach the domain; the driver calls attached(false) if that failed
        kScanIova,      // window A START_ADDR to the slot, ACT_REQ
        kScanPa,        // window A START_ADDR to the buffer's physical address, ACT_REQ
        kFinish,        // detach (bypass), CRC unit off: passed
        kAbort,         // undo whatever is still changed (pixel, START_ADDR, attach, CRC unit): failed at `stage`
    };

    static constexpr unsigned kLatchFrames = 4;
    // A stage's sample: the latched CRC once two frame ends in a row read it, not 0. At most kZeroReads reads of 0 (the unit
    // has not latched a frame yet) and kSettleReads nonzero reads that do not repeat, then the stage fails.
    static constexpr unsigned kZeroReads = 8, kSettleReads = 4;
    Stage stage = kIdle, failedAt = kIdle;
    unsigned wait = 0;  // frame ends to let pass before reading the CRC
    bool latchPending = false;
    unsigned latchFrames = 0;
    uint32_t base = 0, again = 0, tweaked = 0, seen = 0;  // CRCs, for the line
    uint32_t last = 0;              // the stage's last nonzero read (0: none yet, or the last read was 0)
    unsigned zeros = 0, reads = 0;  // the stage's reads of 0 and its nonzero reads that did not settle
    unsigned frames = 0,
             firstLatch = 0;  // frame ends since start(); the first that read a CRC other than 0 (0: none yet)
    bool pixelChanged = false, isAttached = false, onIova = false;
    const char *why = "";

    bool active() const
    {
        return stage != kIdle && stage != kDone && stage != kFailed;
    }

    // The caller has just turned the CRC unit on.
    void start()
    {
        stage = kCrc;
        wait = 2;
        frames = firstLatch = 0;
        base = again = 0;
        settleReset();
    }

    void settleReset()
    {
        last = 0;
        zeros = reads = 0;
    }

    // One frame end's read of the latched CRC in a stage: true with the sample when the register settled (this read and the
    // one before it equal, not 0); *failed when the stage has read too long without one
    bool settle(uint32_t crc, Action *failed)
    {
        *failed = kNothing;
        if (!crc) {  // not latched yet (or never): never a sample, and it ends any run
            last = 0;
            if (++zeros >= kZeroReads)
                *failed = fail(firstLatch ? "the CRC unit went back to 0" : "the CRC unit reads 0 (no frame latched)");
            return false;
        }
        if (!firstLatch)
            firstLatch = frames;
        if (stage == kCrc) {  // for the line: the first CRC read, and the latest
            if (!base)
                base = crc;
            again = crc;
        }
        if (crc == last) {
            settleReset();
            return true;
        }
        last = crc;
        if (++reads >= kSettleReads)
            *failed = fail("the CRC is not stable on a still frame");
        return false;
    }

    void attached(bool ok)
    {
        isAttached = ok;
        if (!ok)
            why = "the domain did not attach";
    }

    // At every DC A frame end while active: the latched CRC, whether a window A request is still out (ACT_REQ), and whether
    // window A underflowed in the frame that ended.
    Action frameEnd(uint32_t crc, bool requestOut, bool underflow)
    {
        if (!active())
            return kNothing;
        ++frames;
        if (stage == kAttached && !isAttached)
            return fail("the domain did not attach");
        if (isAttached && underflow)
            return fail("window A underflowed through the SMMU");
        if (latchPending) {
            if (requestOut)
                return ++latchFrames >= kLatchFrames ? fail("window A did not take the start address") : kNothing;
            latchPending = false;
            wait =
                2;  // the frame starting at this boundary is the first from the new address; its CRC is latched at the
                    // next frame end, one more in case the latched CRC trails a frame
            return kNothing;
        }
        if (wait && --wait)
            return kNothing;
        seen = crc;
        Action failed;
        if (!settle(crc, &failed))
            return failed;
        switch (stage) {
        case kCrc:
            base = again = crc;
            pixelChanged = true;
            return next(kPixel, 2, kTweakPixel);
        case kPixel:
            tweaked = crc;
            if (crc == base)
                return fail("the CRC did not change with a pixel");
            pixelChanged = false;
            return next(kRestored, 2, kRestorePixel);
        case kRestored:
            if (crc != base)
                return fail("the CRC did not come back with the pixel");
            return next(kAttached, 2, kAttach);
        case kAttached:
            if (crc != base)
                return fail("the identity map changed the frame");
            onIova = true;
            return latch(kIova, kScanIova);
        case kIova:
            if (crc != base)
                return fail("the frame through the IOVA slot differs");
            onIova = false;
            return latch(kBack, kScanPa);
        case kBack:
            if (crc != base)
                return fail("the frame by physical address differs after the slot");
            stage = kDone;
            isAttached = false;
            return kFinish;
        default:
            return kNothing;
        }
    }

    Action next(Stage s, unsigned frameEnds, Action a)
    {
        stage = s;
        wait = frameEnds;
        settleReset();
        return a;
    }

    Action latch(Stage s, Action a)
    {
        stage = s;
        latchPending = true;
        latchFrames = 0;
        settleReset();
        return a;
    }

    Action fail(const char *reason)
    {
        why = reason;
        failedAt = stage;
        stage = kFailed;
        return kAbort;
    }

    // Carries out an action with the driver's (or the test model's) I/O: tweakPixel(), restorePixel(), attach() -> bool,
    // scan(bool iova) (window A START_ADDR and ACT_REQ), detach(), crcOff(). An abort undoes, in reverse, what is still
    // changed. Returns true when the probe is over (passed or failed).
    template <typename Io> bool perform(Action a, Io &io)
    {
        switch (a) {
        case kNothing:
            return false;
        case kTweakPixel:
            io.tweakPixel();
            return false;
        case kRestorePixel:
            io.restorePixel();
            return false;
        case kAttach:
            attached(io.attach());
            return false;
        case kScanIova:
            io.scan(true);
            return false;
        case kScanPa:
            io.scan(false);
            return false;
        case kFinish:
            io.detach();
            io.crcOff();
            return true;
        case kAbort:
            if (pixelChanged)
                io.restorePixel();
            pixelChanged = false;
            if (onIova || latchPending)
                io.scan(false);  // a START_ADDR request may be out: the physical address wins
            onIova = latchPending = false;
            if (isAttached)
                io.detach();
            isAttached = false;
            io.crcOff();
            return true;
        }
        return false;
    }

    static const char *name(Stage s)
    {
        static const char *const names[] = {"idle", "crc",  "pixel", "pixel", "attach",
                                            "iova", "back", "done",  "failed"};
        return s <= kFailed ? names[s] : "?";
    }
};
