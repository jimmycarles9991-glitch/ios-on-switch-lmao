// SPDX-License-Identifier: BSD-2-Clause-Patent
// DC A's frame end between its primary filter and the gate: INT_MASK's FRAME_END routing, the filter, and what it hands
// the frame-end action: per-cause counts only the filter writes, which the action compares with the counts it handled.
// Included in the class body (and in the host harness). Its words are static, as the completion flip's: the instance
// is at the 16 KiB a kalloc_type class may have, and only the primary display runs the frame end.

// FRAME_END's INT_MASK bit as last written while the scanout is live: set under intMaskLock by whoever unmasks it (the
// masked->unmasked edge, any context), cleared only by the gate (the idle mask, a stopped or suspended scanout, the
// stop). The gate reading it set knows FRAME_END stays unmasked until the gate itself masks it, so the kick takes no lock.
static inline bool frameEndUnmasked = false;
static inline uint32_t intMaskTakes = 0;  // debug kernels: intMaskLock's takes (counted under it), for the stats line

void noteIntMaskLock()
{
    if (stats)
        __atomic_store_n(&intMaskTakes, __atomic_load_n(&intMaskTakes, __ATOMIC_RELAXED) + 1u, __ATOMIC_RELAXED);
}

void noteFrameEndMask(uint32_t mask)
{
    const bool unmasked = (mask & kFrameEndInt) && intMaskLive && !scanoutStopped && !hwSuspended;
    __atomic_store_n(&frameEndUnmasked, unmasked, __ATOMIC_RELAXED);
}

// Under the gate.
void writeIntMask(uint32_t clear, uint32_t set)
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
    noteIntMaskLock();
    const uint32_t mask = (dc[kDcIntMask] & ~clear) | set;
    dc[kDcIntMask] = mask;
    noteFrameEndMask(mask);
    IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
}

// Any context. A stopped or powered-down scanout gives no frame end; setScanout unmasks it at the restart. INT_ENABLE keeps
// FRAME_END latching while it is masked, so the latched one is stale (a frame end long ago, not now) and goes first.
void unmaskFrameEnd()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
    noteIntMaskLock();
    if (intMaskLive && !scanoutStopped && !hwSuspended) {
        if (!(dc[kDcIntMask] & kFrameEndInt)) {
            dc[kDcIntStatus] = kFrameEndInt;
            dc[kDcIntMask] = dc[kDcIntMask] | kFrameEndInt;
        }
        __atomic_store_n(&frameEndUnmasked, true, __ATOMIC_RELAXED);
    }
    IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
}

// The kick's first step (under the gate): every display job starts there and some wait for a frame end.
void kickUnmaskFrameEnd()
{
    if (!__atomic_load_n(&frameEndUnmasked, __ATOMIC_RELAXED))
        unmaskFrameEnd();
}

// Under the gate, at a frame end with nothing waiting for the next: masked unless a hold is out. A holder increments
// gFrameEndHolds before it takes this lock, so a hold this reads as absent unmasks again after it.
void maskIdleFrameEnd()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
    noteIntMaskLock();
    if (!__atomic_load_n(&gFrameEndHolds, __ATOMIC_ACQUIRE)) {
        dc[kDcIntMask] = dc[kDcIntMask] & ~kFrameEndInt;
        __atomic_store_n(&frameEndUnmasked, false, __ATOMIC_RELAXED);
    }
    IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
}

// Under the gate, with the lock, as the stop or a power-down takes INT_MASK from the display.
void intMaskDead()
{
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(intMaskLock);
    intMaskLive = false;
    __atomic_store_n(&frameEndUnmasked, false, __ATOMIC_RELAXED);
    IOSimpleLockUnlockEnableInterrupt(intMaskLock, state);
}

// Primary interrupt context. The action is scheduled with signalInterrupt() and the filter returns false: returning
// true on this level line makes IOFilterInterruptEventSource::disableInterruptOccurred mask it until the work loop has
// run the action, and TegraPanel's DSI steps and the console tape need every frame end however long the gate is held.
// The filter clears the DC's status and reads it back first, which is what signalInterrupt() requires.
template <class Self> static bool vblankFilter(OSObject *owner, IOFilterInterruptEventSource *source)
{
    Self self = static_cast<Self>(owner);
    const uint32_t status = self->dc[kDcIntStatus] & (kVblankInt | kFrameEndInt | kVpulse2Int);
    if (!status)
        return false;
    self->dc[kDcIntStatus] = status;
    (void)self->dc[kDcIntStatus];  // read back: the line is low before ArmCpu's EOI
    if (gPanicRestore.panicking)
        return false;  // window A is the panic's (restorePitchOnPanic): no flip, no DSI command
    if (status & kVblankInt) {
        self->vblankTime = mach_absolute_time();
        self->vblankContinuous = mach_continuous_time();
    }
    if (status & kVpulse2Int) {  // r17-touch: the vsync phase's line
        self->vpulseTime = mach_absolute_time();
        self->vpulseContinuous = mach_continuous_time();
    }
    // The panel's DSI command steps, timed from the frame end as hekate does (the line is already low; the last step takes ~620 us).
    // A console waiting to scroll (waitTapeFrame) wakes at every frame end while it draws on the tape.
    if (status & kFrameEndInt) {
        if (__atomic_load_n(&self->scanoutQuiescing, __ATOMIC_ACQUIRE)) {
            bump(self->scanoutStopFrames, 1ULL);
            thread_wakeup(reinterpret_cast<event_t>(&self->scanoutStopFrames));
        }
        tegra_panel_frame_end_filter();
        self->color->frameEndFilter();
        if (self->tapeAttached) {
            bump(self->tapeFrames, 1ULL);
            thread_wakeup((event_t)&self->tapeFrames);
        }
        // r6-lows: the frame end's time (the present stamp); debug kernels also the DC A period.
        const uint64_t now = mach_absolute_time();
        const uint64_t last = self->feLast;
        __atomic_store_n(&self->feStamp, now, __ATOMIC_RELEASE);
        self->feLast = now;
        if (self->stats) {
            // Only consecutive frame ends (a stopped scanout or SC7 in between is no period).
            if (last && now > last && (!self->vsyncInterval || now - last < 2 * self->vsyncInterval)) {
                const uint64_t period = now - last;
                bump(self->fePeriodSum, period);
                bump(self->fePeriodN, 1u);
                if (period < __atomic_load_n(&self->fePeriodMin, __ATOMIC_RELAXED))
                    __atomic_store_n(&self->fePeriodMin, period, __ATOMIC_RELAXED);
                if (period > __atomic_load_n(&self->fePeriodMax, __ATOMIC_RELAXED))
                    __atomic_store_n(&self->fePeriodMax, period, __ATOMIC_RELAXED);
            }
        }
    }
    // Last, with release: the action that reads a count sees the stamps and times stored before it.
    if (status & kVblankInt)
        bump(self->vblankTaken, 1u);
    if (status & kVpulse2Int)
        bump(self->vpulseTaken, 1u);
    if (status & kFrameEndInt)
        bump(self->feTaken, 1u);
    source->signalInterrupt();
    return false;
}

// The filter's own counts: one INTID's handler never runs on two cores at once (the GIC holds it active until its EOI),
// so it is their one writer and a load of its own last value and a store count them, with no exclusive-monitor loop.
template <typename Count, typename By> static void bump(Count &count, By by)
{
    __atomic_store_n(&count, static_cast<Count>(__atomic_load_n(&count, __ATOMIC_RELAXED) + by), __ATOMIC_RELEASE);
}

// What the filter took per cause, and what the gate has handled of it (the gate's own words, read by the console).
static inline uint32_t feTaken = 0, vblankTaken = 0, vpulseTaken = 0;
static inline uint32_t feHandled = 0, vblankHandled = 0, vpulseHandled = 0;

static uint32_t newSince(const uint32_t &taken, uint32_t &handled, uint32_t &folded)
{
    const uint32_t now = __atomic_load_n(&taken, __ATOMIC_ACQUIRE);
    const uint32_t seen = __atomic_load_n(&handled, __ATOMIC_RELAXED);
    folded = now - seen > 1u ? now - seen - 1u : 0u;
    __atomic_store_n(&handled, now, __ATOMIC_RELAXED);
    return now - seen;
}

// The frame-end action's first step: what the filter took since the last action. Two of one cause before the action ran
// are one status bit, as they were OR-ed before; the stats count the fold.
uint32_t takeFrameEndStatus()
{
    uint32_t folded = 0, status = 0;
    if (newSince(vpulseTaken, vpulseHandled, folded))
        status |= kVpulse2Int;
    if (const uint32_t vblanks = newSince(vblankTaken, vblankHandled, folded)) {
        status |= kVblankInt;
        if (stats && vsyncArmed) {
            stVblanks += vblanks;
            stVsyncFolded += folded;
        }
    }
    if (newSince(feTaken, feHandled, folded)) {
        status |= kFrameEndInt;
        if (stats)
            stFeFolded += folded;
    }
    return status;
}

// Any context: a cause the filter took that the gate has not handled yet.
bool frameEndPending(uint32_t bits) const
{
    const auto pending = [](const uint32_t &taken, const uint32_t &handled) {
        return __atomic_load_n(&taken, __ATOMIC_ACQUIRE) != __atomic_load_n(&handled, __ATOMIC_RELAXED);
    };
    return ((bits & kFrameEndInt) && pending(feTaken, feHandled)) ||
           ((bits & kVblankInt) && pending(vblankTaken, vblankHandled)) ||
           ((bits & kVpulse2Int) && pending(vpulseTaken, vpulseHandled));
}

// Under the gate: what the filter took of these causes so far is stale (latched while masked, or before a stop).
void dropFrameEndStatus(uint32_t bits)
{
    uint32_t folded = 0;
    if (bits & kFrameEndInt)
        (void)newSince(feTaken, feHandled, folded);
    if (bits & kVblankInt)
        (void)newSince(vblankTaken, vblankHandled, folded);
    if (bits & kVpulse2Int)
        (void)newSince(vpulseTaken, vpulseHandled, folded);
}
