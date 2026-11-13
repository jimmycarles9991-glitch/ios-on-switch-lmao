// SPDX-License-Identifier: BSD-2-Clause-Patent

void presentEntry(int i)
{
    if (presentedByCompletion(i))
        return;
    const bool mayReplace = ring.pending == kNoBuffer || sameLayout(i, ring.pending);
    const TegraScanoutRing::Ready how = ring.ready(i, mayReplace);
    entrySeq[i] = entryPendingSeq[i] = seqOf(flipRequest);
    // r18-planes: the window B change of the swap this frame shows goes out with it (writeFlip), now or at the latch
    // that releases a held frame.
    entryPlaneSet[i] = planeNextSet;
    if (planeNextSet) {
        entryPlane[i] = planeNext;
        entryPlane[i].seq = entrySeq[i];
        planeTarget = entryPlane[i];
        planeNextSet = false;
    }
    markFlip(i);
    if (how != TegraScanoutRing::kHold) {
        if (!rotated)
            startRotation(i);
        else
            writeFlip(i);
    }
}

void startRotation(int target = kNoBuffer)
{
    const bool entryPrepared = target != kNoBuffer;
    if (!entryPrepared)
        target = doubleBuffer ? ring.target() : 0;
    if (target == kNoBuffer)
        panic("SwitchDisplay: the first frame has no scanout surface (front %d pending %d replaced %d)", ring.front,
              ring.pending, ring.superseded);
    checkPitchWindow();
    uint32_t kind = 0, stride = 0, hOffset = 0;
    layoutOf(target, kind, stride, hOffset);
    // Latch window A at the frame boundary: a layout change taken mid-frame would tear across the whole picture.
    dc[kDcRegActControl] = pitchActControl & ~kWinAActHcntrSel;
    dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcWindowHeader] = kWindowASelect;
    dc[kWinLineStride] = stride;
    dc[kWinbufSurfaceKind] = kind;
    dc[kWinbufHOffset] = hOffset;
    dc[kWinOptions] = pitchOptions | kBlockScan;
    dc[kWinbufStartAddr] = surfaceAddress(target);
    // From here a panic must put the pitch window back (see restorePitchOnPanic); disarmed it only stops this driver's flips.
    chainPanicHook();
    gPanicRestore.armed = true;
    // Underflow latches without raising the line (INT_MASK untouched); checkRotation reads and clears it.
    dc[kDcIntStatus] = kWinAUnderflowInt;
    dc[kDcIntEnable] = dc[kDcIntEnable] | kWinAUnderflowInt;
    rotated = true;
    rotationLatched = false;
    rotationRequested = frameEnds;
    underflowRun = 0;
    if (doubleBuffer && !entryPrepared) {
        ring.ready(target);
        entrySeq[target] = seqOf(flipRequest);
        entryPendingSeq[target] = entrySeq[target];
        markFlip(target);
    }
    uint32_t update = kGeneralUpdate | kWinAUpdate;
    uint32_t request = kGeneralActReq | kWinAActReq;
    if (entryPrepared && entryPlaneSet[target]) {
        entryPlaneSet[target] = false;
        planeStage(entryPlane[target], update, request);
    } else if (planeNextSet) {
        planeNextSet = false;
        planeNext.seq = entrySeq[target];
        planeStage(planeNext, update, request);
    }
    scanoutMemoryBarrier();
    dc[kDcStateControl] = update;
    dc[kDcStateControl] = request;
    if (doubleBuffer) {
        flipRequested = frameEnds;
        armFlipDeadline();
    }
    IOLog("SwitchDisplay: window A %s scanout to %#x requested (KIND %#x STRIDE %#x ACT_CONTROL %#x -> %#x)\n",
          kBlockScanName, surfaceAddress(target), kind, stride, pitchActControl, pitchActControl & ~kWinAActHcntrSel);
}

void checkRotation()
{
    if (!rotationLatched) {
        const uint32_t control = dc[kDcStateControl];
        if (control & (kGeneralActReq | kWinAActReq)) {
            if (frameEnds - rotationRequested < kLatchFrames)
                return;
            panic("SwitchDisplay: window A never latched the rotation in %u frames: STATE_CONTROL %#x "
                  "ACT_CONTROL %#x OPTIONS %#x "
                  "STRIDE %#x KIND %#x H %#x",
                  kLatchFrames, control, static_cast<uint32_t>(dc[kDcRegActControl]), windowActive(kWinOptions),
                  windowActive(kWinLineStride), windowActive(kWinbufSurfaceKind), windowActive(kWinbufHOffset));
        }
        const uint32_t options = windowActive(kWinOptions);
        const uint32_t stride = windowActive(kWinLineStride);
        const uint32_t kind = windowActive(kWinbufSurfaceKind);
        const uint32_t hOffset = windowActive(kWinbufHOffset);
        const uint32_t start = windowActive(kWinbufStartAddr);
        uint32_t wantKind = kBlockSurfaceKind;
        uint32_t wantStride = kBlockLineStride;
        uint32_t wantH = kBlockHOffset;
        if (doubleBuffer)
            layoutOf(ring.pending != kNoBuffer ? ring.pending : ring.front, wantKind, wantStride, wantH);
        if (options != (pitchOptions | kBlockScan) || (stride & 0xffff) != wantStride || kind != wantKind ||
            hOffset != wantH || start != expectedStart()) {
            panic("SwitchDisplay: DC A did not take the initial window A: OPTIONS %#x STRIDE %#x KIND %#x "
                  "H %#x START %#x, wrote %#x %#x %#x %#x %#x",
                  options, stride, kind, hOffset, start, pitchOptions | kBlockScan, wantStride, wantKind, wantH,
                  expectedStart());
        }
        rotationLatched = true;
        underflowRun = 0;
        dc[kDcIntStatus] = kWinAUnderflowInt;
        stopConsoleTape();  // window A has left it
        applyUiMemoryPlan();
        if (!rotationAnnounced) {
            rotationAnnounced = true;
            IOLog("SwitchDisplay: window A scanout latched at %#x after %llu frame ends\n", start,
                  frameEnds - rotationRequested);
        }
        if (ring.pending != kNoBuffer) {
            // start is expectedStart(), the flip asked for last: taken (a replaced one never scanned for long).
            const int asked = ring.pending;
            const int replaced = ring.superseded;
            ring.latch(surfaceIndex(start), false);
            releaseIdleDirectReads();
            if (replaced != kNoBuffer)
                ++stReplaced;
            const uint64_t stamp = __atomic_load_n(&feStamp, __ATOMIC_ACQUIRE);
            notePresent(stamp, frameEnds - flipRequested, marks[asked]);
            retireThrough(entrySeq[asked], stamp);  // r18-planes: retiring at the latch from the first frame on
            flipLatched();
        }
        return;
    }
    if (ring.pending != kNoBuffer)
        checkFlip(true);
    const uint32_t underflow = dc[kDcIntStatus] & uiUnderflow();
    landscapeRunUnderflow(underflow);
    if (!underflow) {
        underflowRun = 0;
        return;
    }
    dc[kDcIntStatus] = underflow;
    noteLatchedUnderflow(underflow);
    if (stats)
        stUnderflows++;
    if (probe.active())
        probeUnderflow = true;
    if (directOn && ring.front >= kSlot0 && underflowRun + 1 >= kDirectUnderflowFrames) {
        directOn = false;
        IOLog("SwitchDisplay: window A underflowed %u frames in a row scanning a surface through the SMMU: direct "
              "scanout off, frames are copied again (swaps still retire at their latch)\n",
              underflowRun + 1);
    }
    if (++underflowRun < kUnderflowFrames)
        return;
    panic("SwitchDisplay: window %c underflowed %u frames in a row since the rotation: UFLOW_STATUS %#x OPTIONS "
          "%#x KIND %#x STRIDE %#x",
          uiName(), underflowRun, underflowCounts[uiName() - 'A'], uiActive(kWinOptions), uiActive(kWinbufSurfaceKind),
          uiActive(kWinLineStride));
}

// Under the gate: the UI window (A; B in landscape) to surface i from the next frame boundary (its layout once direct
// scanout is on, START_ADDR, then the window and general requests).
void writeFlip(int i)
{
    flipFromGate();
    scanoutMemoryBarrier();  // the frame is in memory before the window can fetch it
    dc[kDcStateAccess] = dc[kDcStateAccess] & ~(kReadMuxActive | kWriteMuxActive);
    dc[kDcWindowHeader] = kWindowASelect;
    if (directAttached) {
        uint32_t kind = 0;
        uint32_t stride = 0;
        uint32_t hOffset = 0;
        layoutOf(i, kind, stride, hOffset);
        uiWord(kWinbufSurfaceKind) = kind;
        uiWord(kWinLineStride) = stride;
        uiWord(kWinbufHOffset) = hOffset;
    }
    uiWord(kWinbufStartAddr) = surfaceAddress(i);
    // The overlays' change for this frame, if any, into their assembly registers: one UPDATE write arms every window and
    // one ACT_REQ write latches them at the same boundary (the ARM copy makes a replaced pair whole either way).
    uint32_t update = kGeneralUpdate | uiUpdate();
    uint32_t request = kGeneralActReq | uiRequest();
    // NOLINTNEXTLINE(clang-analyzer-security.ArrayBound): i is a ring entry, never kNoBuffer
    if (entryPlaneSet[i]) {
        entryPlaneSet[i] = false;
        planeStage(entryPlane[i], update, request);
    }
    dc[kDcStateControl] = update;
    dc[kDcStateControl] = request;
    if (SWITCH_FRAME_TIMELINE)
        KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_FLIP), surfaceAddress(i));
    flipRequested = frameEnds;  // a replaced flip's boundary is this one's too
    armFlipDeadline();
}
