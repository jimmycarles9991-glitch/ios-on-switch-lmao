// SPDX-License-Identifier: BSD-2-Clause-Patent
// HW Test display.x1.host1x_flip, debug kernels only: window A flipped by class 0x70 streams on the display's own
// host1x channel, each held behind a producer's syncpoint (SwitchHost1xFlipRun.h has the run). Writing N to
// debug.switch_display_host1x_flip starts N flips (N | kForcedFailure: the middle one's producer fails); reading it
// gives the run's line. The swaps are held and the frame end routed while it runs, as for the column scan, and nothing
// else may write window A or a request then (completionFlipMay, mediaFlipMay, landscapeBusy, the colour and palette
// requests). The second surface is the front's own pages mapped into a plane slot, so the run is invisible.
// kern.switch_flips_host1x counts the flips host1x wrote and window A latched as written. The channel is the class
// probe's too: one of them claims it at a time. Its state is static: the instance is at the 16 KiB a kalloc_type class
// may have. Included in the class body after SwitchDcHost1xClassProbe.h.

#if SWITCH_FRAME_TIMELINE
struct Host1xFlipState {
    switch_host1x_flip::Run run;
    uint32_t syncpts[2] = {};  // producer, display
    bool syncptsReady = false, holding = false;
    int aliasSlot = -1;
    char line[360] = "host1x_flip=not_run";
};

static Host1xFlipState host1xFlip;
static inline uint32_t host1xFlipsLatched = 0;
static inline struct sysctl_oid host1xFlipOid = {}, host1xFlipsOid = {};
static constexpr uint32_t kHost1xFlipChannel = TEGRA_HOST1X_CHANNEL_DISPLAY;

static bool host1xFlipBusy()
{
    return host1xFlip.run.busy();
}

static int host1xFlipSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    NativeLegacyDisplay *display = __atomic_load_n(&primaryDisplay, __ATOMIC_ACQUIRE);
    if (!display)
        return ENODEV;
    if (req->newptr) {
        int value = 0;
        if (const int error = SYSCTL_IN(req, &value, sizeof(value)))
            return error;
        if (const int error = display->host1xFlipStart(static_cast<uint32_t>(value)))
            return error;
    }
    char line[sizeof(host1xFlip.line)];
    display->host1xFlipLine(line, sizeof(line));
    return SYSCTL_OUT(req, line, strlen(line) + 1);
}

static int host1xFlipsSysctl(struct sysctl_oid *, void *, int, struct sysctl_req *req)
{
    const int value = static_cast<int>(__atomic_load_n(&host1xFlipsLatched, __ATOMIC_RELAXED));
    return SYSCTL_OUT(req, &value, sizeof(value));
}

static void host1xFlipRegister()
{
    if (host1xFlipOid.oid_name)
        return;
    switch_sysctl_oid(&host1xFlipOid, &sysctl__debug_children, "switch_display_host1x_flip",
                      static_cast<int>(CTLTYPE_STRING | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED),
                      host1xFlipSysctl, "A",
                      "write N: window A flips N times through the display's host1x channel (N | 65536: the middle "
                      "flip's producer fails); read: the run");
    sysctl_register_oid(&host1xFlipOid);
    switch_sysctl_oid(&host1xFlipsOid, &sysctl__kern_children, "switch_flips_host1x",
                      static_cast<int>(CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED), host1xFlipsSysctl, "I",
                      "window A flips since boot that host1x wrote and window A latched as written");
    sysctl_register_oid(&host1xFlipsOid);
}

template <typename Body> void host1xFlipOnLoop(Body body)
{
    getWorkLoop()->runAction(
        [](OSObject *, void *b, void *, void *, void *) -> IOReturn {
            (*static_cast<Body *>(b))();
            return kIOReturnSuccess;
        },
        this, &body);
}

void host1xFlipLine(char *line, size_t size)
{
    host1xFlipOnLoop([&] {
        if (host1xFlip.run.busy())
            snprintf(line, size, "host1x_flip=running flips=%u", host1xFlip.run.flips);
        else
            strlcpy(line, host1xFlip.line, size);
    });
}

// From the sysctl's thread: the channel claimed (the class probe's flag), host1x up, the syncpoints taken once, then
// the run begun under the gate.
int host1xFlipStart(uint32_t request)
{
    const uint32_t flips = request & 0xffff;
    if (!flips || flips > switch_host1x_flip::kMaxFlips || (request & ~(0xffffu | switch_host1x_flip::kForcedFailure)))
        return EINVAL;
    if (!getWorkLoop())
        return ENODEV;
    // A sysctl thread each may race for the channel: one exchange on this start path, never per flip.
    if (__atomic_exchange_n(&dcClassBusy, true, __ATOMIC_ACQUIRE))
        return EBUSY;
    const char *why = nullptr;
    if (tegra_host1x_start() != TEGRA_HOST1X_OK) {
        why = "host1x";
    } else if (!host1xFlip.syncptsReady) {
        for (uint32_t &id : host1xFlip.syncpts)
            if (!why && tegra_host1x_syncpt_alloc(&id) != TEGRA_HOST1X_OK)
                why = "no_syncpoint";
        host1xFlip.syncptsReady = !why;
    }
    host1xFlipOnLoop([&] {
        if (!why)
            why = host1xFlipRefusal();
        if (why) {
            snprintf(host1xFlip.line, sizeof(host1xFlip.line), "host1x_flip=not_run reason=%s", why);
            return;
        }
        host1xFlip.run.begin(flips, (request & switch_host1x_flip::kForcedFailure) != 0u);
        host1xFlip.holding = true;
        native_legacy_frame_end_hold();
    });
    if (why) {
        __atomic_store_n(&dcClassBusy, false, __ATOMIC_RELEASE);
        return EAGAIN;
    }
    return 0;
}

// Under the gate: what the run needs that waiting for a quiet frame end cannot bring.
const char *host1xFlipRefusal()
{
    if (!rotated || !rotationLatched || !doubleBuffer)
        return "window_a_not_flipping";
    if (!directAttached || !planesOn)
        return "no_smmu_alias";
    if (hwSuspended || scanoutStopped)
        return "display_off";
    if (landscapeOn() || landscapeHoldsKicks())
        return "landscape";
    if (columnScanBusy() || windowProbeBusy() || host1xFlip.run.busy())
        return "experiment";
    return nullptr;
}

// What keeps window A or a request moving under the run (nullptr: nothing).
const char *host1xFlipInFlight()
{
    if (transferred || copyQueued || planeFencePending)
        return "a swap is in flight";
    if (ring.pending != kNoBuffer || ring.drawing != kNoBuffer || ring.held != kNoBuffer ||
        ring.superseded != kNoBuffer || completionFlipOut())
        return "a window A flip is out";
    if (planeLatch.requested() || planeHeldSet || planeRecovering)
        return "an overlay change is out";
    if (probeRequest || probe.active() || planeProbe.active())
        return "an SMMU probe runs";
    if (tapeAttached || tapeSettle)
        return "the console tape is on";
    if (hwSuspended || scanoutStopped)
        return "the display is off";
    if (dc[kDcStateControl] & (kGeneralActReq | kWinAActReq | kWinBActReq | kWinCActReq))
        return "a request is out";
    return nullptr;
}

struct Host1xFlipIo {
    NativeLegacyDisplay *d;

    bool quiet() const
    {
        return d->host1xFlipInFlight() == nullptr;
    }

    const char *surfaces(switch_host1x_flip::Surface &front, switch_host1x_flip::Surface &alias) const
    {
        return d->host1xFlipSurfaces(front, alias);
    }

    bool underflowed() const
    {
        const uint32_t underflow = d->dc[kDcIntStatus] & kWinAUnderflowInt;
        if (underflow)
            d->dc[kDcIntStatus] = underflow;
        return underflow != 0u;
    }

    switch_host1x_flip::Active active() const
    {
        return d->host1xFlipActive();
    }

    static uint32_t producer()
    {
        return host1xFlip.syncpts[0];
    }

    static uint32_t display()
    {
        return host1xFlip.syncpts[1];
    }

    static uint32_t producerValue()
    {
        return tegra_host1x_syncpt_read(producer());
    }

    static uint32_t displayValue()
    {
        return tegra_host1x_syncpt_read(display());
    }

    static uint32_t *reserve(uint32_t capacity)
    {
        uint32_t *words = nullptr;
        return switch_host1x_job_reserve(kHost1xFlipChannel, capacity, &words) == TEGRA_HOST1X_OK ? words : nullptr;
    }

    static int submit(const uint32_t *words, uint32_t count, uint32_t displayAt)
    {
        return switch_host1x_job_submit(kHost1xFlipChannel, words, count, display(), displayAt);
    }

    // The CPU stands in for the producer: its work is done when the stream is queued.
    static int raiseProducer(uint32_t at)
    {
        return tegra_host1x_syncpt_release_failed(producer(), at, 1);
    }

    static void releaseProducer(uint32_t at)
    {
        tegra_host1x_syncpt_release_failed(producer(), at, 1);
    }

    static void teardown()
    {
        tegra_host1x_channel_teardown(kHost1xFlipChannel);
    }

    static void restart()
    {
        tegra_host1x_channel_restart(kHost1xFlipChannel);
    }

    void gateWrite(const switch_host1x_flip::Surface &s) const
    {
        d->host1xFlipGateWrite(s);
    }

    void error(const char *what) const
    {
        d->host1xFlipError(what);
    }

    static void latchedByHost1x()
    {
        __atomic_store_n(&host1xFlipsLatched, host1xFlipsLatched + 1, __ATOMIC_RELAXED);
    }

    void finished() const
    {
        d->host1xFlipFinished();
    }
};

// At the first quiet frame end: window A's active surface, which must be the ring's front, and its pages mapped again
// into a plane slot (the column scan's route into the display's SMMU domain).
const char *host1xFlipSurfaces(switch_host1x_flip::Surface &front, switch_host1x_flip::Surface &alias)
{
    front = host1xFlipActive().window;
    if (front.start != expectedStart())
        return "front_not_the_rings";
    const int entry = ring.front;
    IOMemoryDescriptor *memory = nullptr;
    OSObject *surface = nullptr;
    uint64_t low = 0, high = kNativeBlockBytes;
    bool tiled = false;
    if (entry >= kSlot0) {
        const DirectSlot &slot = slots[entry - kSlot0];
        memory = slot.memory;
        surface = slot.surface;
        low = slot.sourceOffset;
        high = low + uint64_t(slot.pages) * 4096;
        tiled = slot.tiled;
    } else if (IOMemoryMap *map = surfaceMapping(entry)) {
        memory = map->getMemoryDescriptor();
    }
    const int k = planeSlotFor(memory, low, high, false, surface, tiled);
    if (k < 0)
        return "no_alias_slot";
    if (!host1xFlipAliasIsFront(entry, k))
        return "alias_not_the_front";
    host1xFlip.aliasSlot = k;
    alias = front;
    alias.start = planeSlotIova(k);
    return nullptr;
}

// The plane slot maps the descriptor's pages and the front's own mapping another list (a direct slot: its scanout
// snapshot's; a scan buffer: its identity range), so the alias is the front only when every page is the same one.
bool host1xFlipAliasIsFront(int entry, int k) const
{
    const auto &plane = planeSlots[k];
    const SwitchNativeScanout *read = entry >= kSlot0 ? slots[entry - kSlot0].read : nullptr;
    const uint64_t count = entry >= kSlot0 ? slots[entry - kSlot0].pages : kNativeBlockBytes / 4096;
    if (plane.high - plane.low != count * 4096 || (entry >= kSlot0 && (!read || !read->allocation.pages)))
        return false;
    for (uint64_t n = 0; n < count; ++n) {
        IOByteCount length = 0;
        const uint64_t mapped = plane.memory->getPhysicalSegment(plane.low + n * 4096, &length, kIOMemoryMapperNone);
        const uint64_t scanned =
            read ? read->allocation.pages[plane.low / 4096 + n] : uint64_t(surfaceAddress(entry)) + n * 4096;
        if (mapped != scanned)
            return false;
    }
    return true;
}

// STATE_CONTROL before the window's words, as latchState reads them: a boundary between the reads then shows the
// request still out, never a latched flip as another surface.
switch_host1x_flip::Active host1xFlipActive()
{
    switch_host1x_flip::Active seen = {};
    seen.control = dc[kDcStateControl];
    seen.window = {windowActive(kWinbufStartAddr), windowActive(kWinbufSurfaceKind), windowActive(kWinLineStride),
                   windowActive(kWinbufHOffset)};
    return seen;
}

// The gate's own write of a flip host1x will not make: every word a torn stream may have reached, then the request.
void host1xFlipGateWrite(const switch_host1x_flip::Surface &s)
{
    scanoutMemoryBarrier();
    dc[switch_host1x_flip::kWinAKind] = s.kind;
    dc[switch_host1x_flip::kWinAStride] = s.stride;
    dc[switch_host1x_flip::kWinAHOffset] = s.hOffset;
    dc[switch_host1x_flip::kWinAStart] = s.start;
    dc[kDcStateControl] = kGeneralUpdate | kWinAUpdate;
    dc[kDcStateControl] = kGeneralActReq | kWinAActReq;
}

void host1xFlipError(const char *what)
{
    const auto &r = host1xFlip.run;
    const switch_host1x_flip::Active seen = host1xFlipActive();
    char channel[200];
    tegra_host1x_describe_channel(kHost1xFlipChannel, channel, sizeof(channel));
    IOLog("SwitchDisplay: host1x flip %u of %u to %#x: %s, so the display's work loop writes it; window A active "
          "START %#x KIND %#x STRIDE %#x H %#x, STATE_CONTROL %#x; producer %u at %u (wait %u), display %u at %u "
          "(wait %u); %s\n",
          r.issued, r.flips, r.out.start, what, seen.window.start, seen.window.kind, seen.window.stride,
          seen.window.hOffset, seen.control, Host1xFlipIo::producer(), Host1xFlipIo::producerValue(), r.producerAt,
          Host1xFlipIo::display(), Host1xFlipIo::displayValue(), r.displayAt, channel);
}

// The run's end, under the gate: the alias slot goes once window A scans the front again, the swaps go on.
void host1xFlipFinished()
{
    auto &s = host1xFlip;
    const auto &r = s.run;
    const switch_host1x_flip::Active seen = host1xFlipActive();
    if (s.aliasSlot >= 0 && seen.window.start == r.front.start && !(seen.control & kWinAActReq) &&
        !planeSlotBusy(s.aliasSlot))
        planeDropSlot(static_cast<unsigned>(s.aliasSlot));
    s.aliasSlot = -1;
    const auto &t = r.totals;
    snprintf(s.line, sizeof(s.line),
             "host1x_flip=%s flips=%u forced=%d host1x=%u gate=%u latched=%u not_as_written=%u unwritten=%u "
             "teardowns=%u after_teardown=%u written=%u display_delta=%u underflows=%u waited=%u stop=%s",
             r.issued || r.why == nullptr ? "done" : "not_run", r.flips, r.failAt != r.kNoFlip, t.host1x, t.gate,
             t.latched, t.notAsWritten, t.unwritten, t.teardowns, t.afterTeardown, t.written,
             r.issued ? Host1xFlipIo::displayValue() - r.displayStart : 0u, t.underflows, t.waited,
             r.why ? r.why : "none");
    IOLog("SwitchDisplay: host1x flip run: %s\n", s.line);
    if (s.holding) {
        s.holding = false;
        native_legacy_frame_end_release();
    }
    __atomic_store_n(&dcClassBusy, false, __ATOMIC_RELEASE);
}

// Every DC A frame end, before checkRotation: window A's underflow during the run is the run's.
void host1xFlipFrameEnd()
{
    if (!host1xFlip.run.busy())
        return;
    Host1xFlipIo io{this};
    host1xFlip.run.frameEnd(io);
    if (!host1xFlip.run.busy() && kickDeferred) {
        kickDeferred = false;
        kickSwapQueue();
    }
}

void host1xFlipAbort(const char *reason)
{
    Host1xFlipIo io{this};
    host1xFlip.run.abort(io, reason);
}
#else
static bool host1xFlipBusy()
{
    return false;
}

static void host1xFlipRegister()
{}

void host1xFlipFrameEnd()
{}

void host1xFlipAbort(const char *)
{}
#endif
