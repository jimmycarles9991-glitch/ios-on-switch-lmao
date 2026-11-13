// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window A's flip written by its producer's completion (SwitchCompletionFlip.h): for a direct frame, a GPU copy and a
// VIC copy, native_legacy_gpu_complete writes window A's START_ADDR, layout and request on whatever thread delivers it
// (the GM20B interrupt pass for a watched present, a present job's completion work item, host1x's fence callback), so
// the request reaches DC A with no display work-loop hop. Everything else stays on the gate: the completion's own pass
// puts the flip in the ring exactly where presentEntry would have written it, and a frame end that comes first runs
// that pass before it reads the latch, so the latch, the retirement and the present stamps see the flip at the
// boundary that took it.
//
// Earlier still, the producer's primary interrupt filter writes it (writeEarlyCompletionFlip): the GPU's nonstall
// filter once a watched present's producer is complete and visible (TegraScanoutCopy.h tgm_present_filter_callout),
// host1x's syncpoint filter once a VIC copy's job is certainly done (switch_host1x_filter_wait,
// switch_gpu_backing_token_filter_status). Its completion follows on the producer's work loop and finds the flip
// written. A frame end or SC7 that comes between puts the early flip in the ring alone (adoption); the completion's
// pass then presents nothing, and retires a swap whose latch it held back. The filters' side is
// SwitchCompletionFlipFilter.h.
//
// The completion writes window A through its own aperture (mainline drm/tegra dc.c tegra_plane_offset, window A's
// plane->offset 0xa00: DC_WIN 0x700 + i at 0xb80 + i, DC_WINBUF 0x800 + i at 0xbc0 + i), never WINDOW_HEADER or
// STATE_ACCESS, which the gate switches for its reads of the active copy. Its WIN_A_UPDATE promotes all of window
// A's assembly copy, so a gate write that puts something else there (a recovery or held flip, window A's blend over
// an overlay, the palette bit, the stop) first withdraws an armed flip under the lock; that frame then flips from the
// gate as before. Included in the class body; writeCompletionFlip runs on the completion's thread,
// writeEarlyCompletionFlip in an interrupt filter, the rest under the gate.
//
// Window A's WINBUF aperture (0xbc0..) is unproven on this unit (its DC_WIN words at 0xb80.. and windows B and C are),
// so every completion flip is checked where the gate already reads the latch: one whose request was taken while window
// A kept scanning another surface turns completion flips off for the boot, and that frame flips from the gate. Until one
// has been seen latched as written, a completion flip keeps window A's layout (LINE_STRIDE at 0xb8a would land without
// SURFACE_KIND and START_ADDR), and a settle that takes the last request as shown with no frame end to check it (SC7, the
// scanout stopped) writes it again from the gate. A refusal turns the filters' writes off with the rest.

// Static, as the landscape and window probe state: the instance is at the 16 KiB a kalloc_type class may have.
// completionFlipLock is taken with interrupts off, and inside the producers' primary filters: under it only plain loads
// and stores to values resolved before it was taken. No IOMemoryMap/IOMemoryDescriptor call, IOLock, IORecursiveLock,
// allocation, IOLog or command gate.
static inline IOSimpleLock *completionFlipLock = nullptr;
static inline SwitchCompletionFlip completionFlip;
static constexpr uint32_t kWinbufABase = 0xbc0;
static constexpr uint32_t kWinAStart = kWinbufABase, kWinAKind = kWinbufABase + (kWinbufSurfaceKind - kWinbufStartAddr),
                          kWinAHOffset = kWinbufABase + (kWinbufHOffset - kWinbufStartAddr),
                          kWinAStride = kWinABase + (kWinLineStride - kWinOptions);

struct CompletionUnverified {
    int entry;
    uint32_t start;
};

static inline CompletionUnverified completionUnverified = {kNoBuffer, 0};  // in the ring, no latch read has judged it
static inline bool completionFlipsProven = false, completionFlipsRefused = false;
static inline bool completionFilterOff = false;  // the producers' filters write no flips: read in interrupt context
#if SWITCH_FRAME_TIMELINE
static constexpr unsigned kFlipTotals = 8;
// completion, gate, lead us, withdrawn, refused, filter, filter lead us, filter waiters declined
static inline uint32_t flipTotals[kFlipTotals] = {};
static inline struct sysctl_oid flipTotalOids[kFlipTotals] = {};
#endif

void startCompletionFlip()
{
    IOSimpleLock *lock = IOSimpleLockAlloc();
    if (!lock)
        panic("SwitchDisplay: no lock for window A's completion flips");
    __atomic_store_n(&completionFlipLock, lock, __ATOMIC_RELEASE);
    flipTotalsRegister();
    mediaFlipTotalsRegister();
    tgm_present_filter_callout(&gpuFilterFlip<decltype(this)>, this);
}

bool completionFlipIdle() const
{
    return __atomic_load_n(&completionFlip.state, __ATOMIC_RELAXED) == SwitchCompletionFlip::kIdle;
}

bool completionFlipOut() const
{
    return !completionFlipIdle() || !mediaFlipIdle();
}

// Armed only when presentEntry would write this frame at once whatever latches meanwhile: no hold, no window B change
// riding on it, nothing else owning window A. A recovery between here and the completion withdraws it (flipFromGate).
bool completionFlipMay(int entry)
{
    if (completionFlipsRefused || !rotated || !rotationLatched || !doubleBuffer ||
        (entry >= kSlot0 && !directAttached) || hwSuspended || gPanicRestore.panicking || planeNextSet ||
        probeRequest || probe.active() || columnScanBusy() || windowProbeBusy() || landscapeOn() ||
        landscapeHoldsKicks() || !mediaFlipIdle())
        return false;
    if (ring.held != kNoBuffer || ring.superseded != kNoBuffer)
        return false;
    if (ring.pending != kNoBuffer)
        return ring.replace && sameLayout(entry, ring.pending);
    return completionFlipsProven || sameLayout(entry, ring.front);
}

// Before the offer: its completion may run on another CPU before the offer returns. True when armed.
bool armCompletionFlip(int entry)
{
    if (!completionFlipLock)
        startCompletionFlip();
    if (!completionFlipMay(entry))
        return false;
    if (!completionFlipIdle())
        panic("SwitchDisplay: an offer for entry %d while entry %d's completion flip is out (state %u)", entry,
              completionFlip.entry, static_cast<unsigned>(completionFlip.state));
    uint32_t kind = 0, stride = 0, hOffset = 0;
    layoutOf(entry, kind, stride, hOffset);
    const uint32_t start = surfaceAddress(entry);
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
    completionFlip.arm(entry, start);
    completionFlip.armLayout(kind, stride, hOffset);
    IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
    return true;
}

int offerWithCompletionFlip(const TegraScanoutCopy &copy, int entry)
{
    const bool armed = armCompletionFlip(entry);
    const int offered = tegra_gm20b_scanout_copy(&copy);
    if (offered && armed)
        withdrawCompletionFlip();
    return offered;
}

// Under the lock: dsb, then six posted stores.
void writeCompletionFlipLocked()
{
    __asm__ volatile("dsb sy" ::: "memory");
    if (completionFlip.layout) {
        dc[kWinAKind] = completionFlip.kind;
        dc[kWinAStride] = completionFlip.stride;
        dc[kWinAHOffset] = completionFlip.hOffset;
    }
    dc[kWinAStart] = completionFlip.start;
    dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
    completionFlip.requestedAt = __atomic_load_n(&frameEnds, __ATOMIC_RELAXED);
    completionFlip.writtenAt = mach_absolute_time();
}

// The completion's thread, after gpuDonePending is set: a failed frame withdraws the flip, a done one is requested.
void writeCompletionFlip(int status)
{
    IOSimpleLock *lock = __atomic_load_n(&completionFlipLock, __ATOMIC_ACQUIRE);
    if (!lock)
        return;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(lock);
    if (SWITCH_FRAME_TIMELINE && completionFlip.early())
        completionFlip.deliveredAt = mach_absolute_time();
    if (gPanicRestore.panicking)
        completionFlip.withdraw();
    const bool write = completionFlip.claim(status);
    if (write)
        writeCompletionFlipLocked();
    writeMediaFlipLocked(status);
    [[maybe_unused]] const uint32_t start = completionFlip.start;
    IOSimpleLockUnlockEnableInterrupt(lock, state);
    if (write && SWITCH_FRAME_TIMELINE)
        KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_FLIP), start);
}

void withdrawCompletionFlip()
{
    if (completionFlipIdle())
        return;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
    const bool armed = completionFlip.state == SwitchCompletionFlip::kArmed;
    completionFlip.withdraw();
    IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
    if (armed && SWITCH_FRAME_TIMELINE)
        noteFlipTotal(3, 1);
}

// writeFlip's first step: the gate writes window A itself.
void flipFromGate()
{
    withdrawCompletionFlip();
    completionUnverified.entry = kNoBuffer;
    if (SWITCH_FRAME_TIMELINE)
        noteFlipTotal(1, 1);
}

// The completion's own pass, run now (gpuDoneFired's), when its flip is already written: a frame end or SC7 that
// comes first must see the flip in the ring.
void runCompletionPass()
{
    if (!__atomic_exchange_n(&gpuDonePending, false, __ATOMIC_ACQUIRE))
        panic("SwitchDisplay: window A was flipped to %#x by its producer's completion, which signalled nothing",
              completionFlip.start);
    finishGpuSwap(__atomic_load_n(&gpuStatus, __ATOMIC_RELAXED));
}

// What the gate does with the flip before it reads window A's latch or saves it (a frame end, SC7): a written one's
// pass runs; an early one is adopted, in the ring now and its completion's pass left with nothing to enter.
uint8_t takeWrittenFlip(SwitchCompletionFlip &flip)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
    const uint8_t seen = completionFlip.state;
    if (seen == SwitchCompletionFlip::kEarly)
        completionFlip.state = SwitchCompletionFlip::kAdopted;
    flip = completionFlip;
    IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
    if (seen == SwitchCompletionFlip::kWritten)
        runCompletionPass();
    else if (seen == SwitchCompletionFlip::kEarly)
        completionFlipIntoRing(flip);
    return seen;
}

// vblankTick's first step.
void takeCompletionFlip()
{
    SwitchCompletionFlip flip;
    if (!completionFlipIdle())
        takeWrittenFlip(flip);
    takeMediaFlip();
}

// Nothing armed, written or early once this returns: an armed flip is withdrawn, a written one goes in the ring, an
// early one is adopted, and an adopted one's completion comes when it comes (gpuPresentDeferred, or after the resume).
// The pass may kick the next offer, armed in turn unless completion flips are off.
void drainCompletionFlip()
{
    SwitchCompletionFlip flip;
    while (!completionFlipIdle()) {
        withdrawCompletionFlip();
        const uint8_t seen = takeWrittenFlip(flip);
        if (seen != SwitchCompletionFlip::kWritten && seen != SwitchCompletionFlip::kEarly)
            return;
    }
}

// SC7, before the controller's state is saved: no completion may write the controller once it is unclocked, and the
// saved assembly copy is what window A gets back.
void suspendCompletionFlip()
{
    drainCompletionFlip();
    mediaFlipFromGate();
    flipUnverifiedFromGate();
}

// SC7 and settleStopped take the last request as shown with no frame end to check it: an unproven completion flip
// still asked for goes into window A's assembly copy from the gate as well.
void flipUnverifiedFromGate()
{
    if (!completionFlipsProven && completionUnverified.entry != kNoBuffer && completionUnverified.entry == ring.pending)
        writeFlip(ring.pending);
}

// checkFlip's read of what the boundary took: STATE_CONTROL and window A's active START_ADDR, with no completion
// write between them and the ring's view (one written before is put in the ring first). A refused completion flip is
// flipped from the gate and the latch read again.
struct LatchState {
    uint32_t control, start;
};

LatchState latchState()
{
    SwitchCompletionFlip flip;
    for (;;) {
        LatchState seen = {};
        if (completionFlipIdle()) {
            seen = {dc[kDcStateControl], uiActive(kWinbufStartAddr)};
        } else {
            const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
            const uint8_t held = completionFlip.state;
            const bool written = held == SwitchCompletionFlip::kWritten || held == SwitchCompletionFlip::kEarly;
            if (!written)
                seen = {dc[kDcStateControl], uiActive(kWinbufStartAddr)};
            IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
            if (written) {
                takeWrittenFlip(flip);
                continue;
            }
        }
        if (!completionFlipRefused(seen))
            return seen;
    }
}

// The verdict on the completion flip still asked for: proven once window A scans it; refused once a boundary took its
// request (ACT_REQ clear) with window A on another surface, i.e. its aperture stores never reached the assembly copy
// WIN_A_UPDATE promotes. A refusal drains the completion flip machinery and asks for the ring's pending frame again
// through WINDOW_HEADER; the ring sees that request still out (kWaiting, or kTakenReplaced for a replaced gate flip).
bool completionFlipRefused(const LatchState &seen)
{
    const CompletionUnverified flip = completionUnverified;
    if (flip.entry == kNoBuffer)
        return false;
    if (flip.entry != ring.pending) {
        completionUnverified.entry = kNoBuffer;
        return false;
    }
    if (seen.start == flip.start) {
        completionUnverified.entry = kNoBuffer;
        completionFlipsProven = true;
        return false;
    }
    if (seen.control & (kGeneralActReq | kWinAActReq))
        return false;
    if (!completionFlipsRefused)
        IOLog("SwitchDisplay: window A's completion flip did not latch (wrote START_ADDR %#x, active %#x): window A "
              "flips from the display's work loop from here on\n",
              flip.start, seen.start);
    completionFlipsRefused = true;
    __atomic_store_n(&completionFilterOff, true, __ATOMIC_RELAXED);
    if (SWITCH_FRAME_TIMELINE)
        noteFlipTotal(4, 1);
    drainCompletionFlip();
    writeFlip(ring.pending);
    return true;
}

// presentEntry's first step: true when the completion already wrote entry i, which then enters the ring as
// presentEntry's own write would have (unless the gate adopted it already).
bool presentedByCompletion(int i)
{
    if (!completionFlipLock)
        startCompletionFlip();
    if (completionFlipIdle())
        return false;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
    const SwitchCompletionFlip flip = completionFlip;
    const bool written = completionFlip.present(i);
    IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
    if ((flip.state == SwitchCompletionFlip::kWritten || flip.early()) && !written)
        panic("SwitchDisplay: window A flips to entry %d while entry %d's completion flip (state %u) is not in the "
              "ring",
              i, flip.entry, static_cast<unsigned>(flip.state));
    if (!written) {
        if (flip.state == SwitchCompletionFlip::kArmed && SWITCH_FRAME_TIMELINE)
            noteFlipTotal(3, 1);
        return false;
    }
    if (flip.state == SwitchCompletionFlip::kAdopted)
        adoptedFlipPresented(flip);
    else
        completionFlipIntoRing(flip);
    return true;
}

// flip()'s first step, a copy's pass: true when the gate adopted the copy's flip, whose buffer is no longer the
// ring's target.
bool flippedByCompletion()
{
    if (completionFlipIdle())
        return false;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(completionFlipLock);
    const SwitchCompletionFlip flip = completionFlip;
    const bool adopted = completionFlip.state == SwitchCompletionFlip::kAdopted && completionFlip.present(flip.entry);
    IOSimpleLockUnlockEnableInterrupt(completionFlipLock, state);
    if (adopted)
        adoptedFlipPresented(flip);
    return adopted;
}

void completionFlipIntoRing(const SwitchCompletionFlip &flip)
{
    const int i = flip.entry;
    const bool mayReplace = ring.pending == kNoBuffer || sameLayout(i, ring.pending);
    if (ring.ready(i, mayReplace) == TegraScanoutRing::kHold)
        panic("SwitchDisplay: window A was flipped to %#x by its producer's completion, but the ring holds it "
              "(front %d pending %d replace %d)",
              flip.start, ring.front, ring.pending, ring.replace);
    entrySeq[i] = entryPendingSeq[i] = seqOf(flipRequest);
    entryPlaneSet[i] = false;
    markFlip(i);
    marks[i].ready = flip.writtenAt;
    flipRequested = flip.requestedAt;
    completionUnverified = {i, flip.start};
    armFlipDeadline();
    if (SWITCH_FRAME_TIMELINE && flip.state != SwitchCompletionFlip::kAdopted)
        noteCompletionFlip(flip);
}

// An adopted flip's completion pass. A latch the gate saw before it could not retire the swap, whose completion was
// still out (retireThrough stops at gpuRequest): it retires here.
void adoptedFlipPresented(const SwitchCompletionFlip &flip)
{
    if (SWITCH_FRAME_TIMELINE)
        noteCompletionFlip(flip);
    if (ring.front == flip.entry)
        retireLatchedThrough(entrySeq[flip.entry]);
}

// A completion's pass after the latch that showed its frame: the swaps through it retire as that latch's
// retireThrough would have, had their completion not been out.
void retireLatchedThrough(uint64_t seq)
{
    if (!latchRetire || !seq)
        return;
    for (const OriginalLegacySwapRequest *head = pendingSwapRequest(); head; head = pendingSwapRequest()) {
        const uint64_t at = seqOf(head);
        if (!at || at > seq || !retireSwap(head, true))
            break;
    }
}

#if SWITCH_FRAME_TIMELINE
void noteCompletionFlip(const SwitchCompletionFlip &flip)
{
    const uint64_t now = mach_absolute_time();
    const uint64_t lead = now > flip.writtenAt ? Span::ticksToUs(now - flip.writtenAt) : 0;
    noteFlipTotal(0, 1);
    noteFlipTotal(2, static_cast<uint32_t>(lead));
    if (flipTotals[0] == 1)
        IOLog("SwitchDisplay: first window A flip written by its producer's completion (entry %d), %llu us before "
              "the display's work loop took it\n",
              flip.entry, static_cast<unsigned long long>(lead));
    if (flip.writer == SwitchCompletionFlip::kFromCompletion)
        return;
    const uint64_t ahead = flip.deliveredAt > flip.writtenAt ? Span::ticksToUs(flip.deliveredAt - flip.writtenAt) : 0;
    noteFlipTotal(6, static_cast<uint32_t>(ahead));
    static bool logged[3];
    if (!logged[flip.writer]) {
        logged[flip.writer] = true;
        IOLog("SwitchDisplay: first window A flip written from %s interrupt filter (entry %d), %llu us before its "
              "work loop\n",
              flip.writer == SwitchCompletionFlip::kFromGpuFilter ? "the GPU's" : "host1x's syncpoint (VIC)",
              flip.entry, static_cast<unsigned long long>(ahead));
    }
}

static void noteFlipTotal(unsigned which, uint32_t n)
{
    __atomic_fetch_add(&flipTotals[which], n, __ATOMIC_RELAXED);
}

static int flipTotalSysctl(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    const int value =
        static_cast<int>(__atomic_load_n(&flipTotals[static_cast<unsigned>(which) % kFlipTotals], __ATOMIC_RELAXED));
    return SYSCTL_OUT(req, &value, sizeof(value));
}

static void flipTotalsRegister()
{
    static const char *const names[kFlipTotals] = {"switch_flips_completion",     "switch_flips_gate",
                                                   "switch_flips_lead_us",        "switch_flips_withdrawn",
                                                   "switch_flips_refused",        "switch_flips_filter",
                                                   "switch_flips_filter_lead_us", "switch_flips_filter_declined"};
    for (unsigned i = 0; i < kFlipTotals; ++i) {
        switch_sysctl_oid(&flipTotalOids[i], &sysctl__kern_children, names[i],
                          static_cast<int>(CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED), flipTotalSysctl, "I",
                          "window A since boot: flips its producers' completions wrote, flips the display's gate "
                          "wrote, microseconds the completions' writes came before the gate took them, completion "
                          "flips withdrawn, completion flips window A did not latch as written, flips their "
                          "producers' interrupt filters wrote, microseconds those came before their completions, VIC "
                          "copies whose job was done or whose waiter host1x refused at the arm");
        flipTotalOids[i].oid_arg2 = static_cast<int>(i);
        sysctl_register_oid(&flipTotalOids[i]);
    }
}
#else
void noteCompletionFlip(const SwitchCompletionFlip &)
{}

static void noteFlipTotal(unsigned, uint32_t)
{}

static void flipTotalsRegister()
{}
#endif
