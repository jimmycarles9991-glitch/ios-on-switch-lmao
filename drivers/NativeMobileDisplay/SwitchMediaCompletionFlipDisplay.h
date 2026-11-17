// SPDX-License-Identifier: BSD-2-Clause-Patent
// A same-geometry media set is a host1x stream: producer WAITs precede the window addresses and UPDATE/ACT_REQ,
// then DC A increments the display fence. Its filter and completion adopt that request without CPU register stores.
// The gate restores the newest requested addresses while the set is staged: this unit promotes another window's
// assembly copy at UPDATE, so an unrelated request must never expose a frame its producer is still writing.
// Failed producer releases stop a dependent stream first. A gate withdrawal can meet partial controller stores;
// its candidate slots remain held until the existing latch observes the restored last-good set.

// Static, as window A's completion flip: the instance is at the 16 KiB a kalloc_type class may have.
static inline SwitchPlaneCompletionFlip mediaFlip;
static inline int mediaFlipUnverified = PlaneCommits::kNone;  // the latch entry the completion requested, unjudged
static inline uint64_t mediaFlipRequestedSeq = 0;  // the set finishPlaneFence requested: planeOut writes nothing for it
#if SWITCH_FRAME_TIMELINE
static constexpr unsigned kMediaFlipTotals = 10;
// completion, withdrawn, refused, lead us, armed, filter, filter waiters declined; video sets whose producers were done
// at their swap, sets that waited for VIC or NVDEC, of those the ones not armed
static inline uint32_t mediaFlipTotals[kMediaFlipTotals] = {};
static inline struct sysctl_oid mediaFlipTotalOids[kMediaFlipTotals] = {};
#endif

#include "SwitchHostMediaFlipDisplay.h"

bool mediaFlipIdle() const
{
    return SwitchFlipWord::stateOf(__atomic_load_n(&mediaFlip.bits, __ATOMIC_RELAXED)) == SwitchFlipWord::kIdle;
}

static bool sameWindowLayout(const switch_display::Overlay &a, const switch_display::Overlay &b)
{
    return switch_display::sameGeometry(a, b) && !(a.on && b.image.convert);
}

uint32_t windowAddress(const switch_display::Overlay &overlay, unsigned plane) const
{
    const uint64_t base = static_cast<uint64_t>(planeSlotIova(overlay.slot)) - planeSlots[overlay.slot].low;
    return static_cast<uint32_t>(base + overlay.image.start[plane]);
}

const PlaneSet &newestRequestedSet() const
{
    return planeSetOf(planeLatch.armed != PlaneCommits::kNone ? planeLatch.armed : planeLatch.active);
}

// planeStage would write p at once, whatever latches before the completion: assigned is p as it would place it.
bool mediaFlipMay(const PlaneSet &p, PlaneSet &assigned)
{
    if (!planesOn || !planesAttached || planeRecovering || planeHeldSet || planeProbe.active() || copyPlaneJoined ||
        hwSuspended || gPanicRestore.panicking || ring.held != kNoBuffer || !planeLatch.mayWrite() ||
        !planeLatch.armedRequested || p.on() != planeBlending || !completionFlipIdle() || !mediaFlipIdle() ||
        p.landscape || landscapeOn() || landscapeHoldsKicks() || windowProbeBusy() || host1xFlipBusy())
        return false;
    const switch_display::WindowState windows[2] = {planeWindowState(switch_landscape::kB),
                                                    planeWindowState(switch_landscape::kC)};
    const auto assignment = switch_display::assign(p.window, windows);
    if (assignment.convert)
        return false;
    assigned = p;
    if (assignment.window[0] != 0) {
        assigned.window[0] = p.window[1];
        assigned.window[1] = p.window[0];
    }
    const PlaneSet &now = newestRequestedSet();
    return sameWindowLayout(assigned.window[0], now.window[0]) && sameWindowLayout(assigned.window[1], now.window[1]) &&
           planeReserveBandwidth(assigned);
}

// Under the gate, before the token fence (its callback may run on another CPU before the fence call returns).
bool armMediaFlip(const PlaneSet &p)
{
    if (!completionFlipStarted())
        startCompletionFlip();
    PlaneSet assigned = {};
    if (!mediaFlipMay(p, assigned)) {
        if (SWITCH_FRAME_TIMELINE)
            noteMediaFlipTotal(9, 1);
        return false;
    }
    const PlaneSet now = newestRequestedSet();
    const PlaneSet target = planeTarget;
    uint32_t update = 0, request = 0;
    if (!planeStage(p, update, request)) {
        planeHeldSet = false;
        planeTarget = target;
        IOLog("SwitchDisplay: windows B and C's set (take %llu) could not be staged at its offer: it flips from the "
              "display's work loop\n",
              static_cast<unsigned long long>(p.seq));
        return false;
    }
    planeLatch.deferRequest();
    mediaFlip.clear();
    for (unsigned window = 0; window != 2; ++window) {
        const switch_display::Overlay &next = assigned.window[window];
        if (!next.on)
            continue;
        for (unsigned plane = 0; plane != (next.image.yuv ? 2u : 1u); ++plane) {
            const uint32_t word = switch_landscape::bufferBase(planeWindowOf(assigned, window)) +
                                  (plane ? native_planes::kStartAddrU : native_planes::kStartAddr);
            dc[word] = windowAddress(now.window[window], plane);
            mediaFlip.store(word, windowAddress(next, plane));
        }
    }
    mediaFlip.arm(update, request);
    if (SWITCH_FRAME_TIMELINE)
        noteMediaFlipTotal(4, 1);
    return true;
}

// planeMediaFence's fence: the producers' completion writes the set when it was armed.
int planeFenceWithMediaFlip(const PlaneSet &p, const switch_gpu_backing_token *producers, unsigned count)
{
    if (count > SwitchHost1xDisplayStream::kProducers)
        return -22;
    return armMediaFlip(p) ? submitMediaHostFlip(producers, count)
                           : switch_host1x_token_fence(producers, count, native_legacy_gpu_complete, this);
}

void writeEarlyMediaFlip(uint32_t generation)
{
    if (gPanicRestore.panicking || !mediaFlip.claimEarly(generation))
        return;
    stampMediaHostFlip();
    mediaFlip.claimed(SwitchCompletionFlip::kEarly);
    if (SWITCH_FRAME_TIMELINE)
        noteMediaFlipTotal(5, 1);
}

// completionFlipVerdict's: a set host1x's filter wrote is on screen, so its completion is a success. A failure breaks
// the producers' contract: one error line.
int mediaFlipVerdict(int status)
{
    if (!status || !mediaFlip.early())
        return status;
    IOLog(
        "SwitchDisplay: windows B and C's set was written from host1x's syncpoint filter, whose completion then failed "
        "(%d): shown as done\n",
        status);
    return 0;
}

// writeCompletionFlip's, on the completion's thread with interrupts off.
void writeMediaFlip(int status)
{
    if (status || gPanicRestore.panicking)
        mediaFlip.withdraw();
}

void withdrawMediaFlip()
{
    if (mediaFlipIdle() || !mediaFlip.withdraw())
        return;
    planeLatch.withdraw();
    if (SWITCH_FRAME_TIMELINE)
        noteMediaFlipTotal(1, 1);
}

// A written set's pass runs; one host1x's filter wrote before its completion is adopted: in the latch now.
void takeMediaFlip()
{
    if (mediaFlipIdle())
        return;
    const uint8_t seen = mediaFlip.adopt();
    const uint64_t requestedAt = mediaFlip.requestedAt, writtenAt = mediaFlip.writtenAt;
    if (seen == SwitchCompletionFlip::kWritten)
        runCompletionPass();
    else if (seen == SwitchCompletionFlip::kEarly)
        mediaFlipIntoLatch(requestedAt, writtenAt, true);
}

// The first step of every other gate writer of windows B and C: nothing of the completion's is left to land after it.
// A pass run here may kick the next offer, which arms in turn; the withdraw after it covers that one.
void mediaFlipFromGate()
{
    cancelMediaHostFlip();
    takeMediaFlip();
    restoreCancelledMediaHostFlip();
    withdrawMediaFlip();
}

// finishPlaneFence's first step: a written set's request is the latch's from here; otherwise the latch is as it was
// before the offer (a failed producer, a panic) and the set goes out through planeOut.
void mediaFlipPass()
{
    restoreCancelledMediaHostFlip();
    if (mediaFlipIdle() && planeLatch.armedRequested)
        return;
    mediaFlip.withdraw();
    const bool adopted = mediaFlip.idleUnless([](uint8_t state) {
        return state != SwitchCompletionFlip::kAdopted;
    }) == SwitchCompletionFlip::kAdopted;
    const bool filtered = mediaFlip.filtered;
    const bool written = !adopted && mediaFlip.take();
    const uint64_t requestedAt = mediaFlip.requestedAt, writtenAt = mediaFlip.writtenAt;
    if (adopted)
        return;
    if (!written) {
        planeLatch.withdraw();
        return;
    }
    mediaFlipIntoLatch(requestedAt, writtenAt, filtered);
}

void mediaFlipIntoLatch(uint64_t requestedAt, uint64_t writtenAt, bool filtered)
{
    planeLatch.request();
    planeRequestedAt = requestedAt;
    mediaFlipUnverified = planeLatch.armed;
    mediaFlipRequestedSeq = planeSetOf(planeLatch.armed).seq;
    armFlipDeadline();
    if (SWITCH_FRAME_TIMELINE)
        noteMediaFlip(writtenAt, filtered);
}

// planeOut's first step. An adopted set the latch already took is retired here, by the retirement that latch observed:
// it could not take it then, with the set's fence still out (oldestPendingFrame holds planeFencing back).
bool presentedByMediaFlip(const PlaneSet &p)
{
    if (!mediaFlipRequestedSeq || p.seq != mediaFlipRequestedSeq)
        return false;
    mediaFlipRequestedSeq = 0;
    if (planeSetOf(planeLatch.active).seq == p.seq)
        retireThrough(p.seq, 0);
    return true;
}

// planeFrameEnd's: a set the completion wrote meanwhile is in the latch before it is read.
bool planeLatchRequested()
{
    takeMediaFlip();
    return planeLatch.requested();
}

bool planeLatchedAtFrameEnd()
{
    for (;;) {
        const bool out = (dc[kDcStateControl] & landscapeRequestBits()) != 0u;
        const int scanned = planeScannedSet();
        if (!mediaFlipRefused(out, scanned)) {
            const bool latched = planeLatch.latched(out, scanned);
            if (latched && mediaHostHeldSlots)
                mediaHostHeldSlots = 0;
            return latched;
        }
    }
}

// The verdict on the set the completion requested: taken once it is scanned; refused once no request is out and
// windows B and C scan something else. A refusal asks for it again from the gate, and the latch reads that request.
bool mediaFlipRefused(bool out, int scanned)
{
    const int id = mediaFlipUnverified;
    if (id == PlaneCommits::kNone)
        return false;
    if (id != planeLatch.armed || !planeLatch.armedRequested || scanned == id) {
        mediaFlipUnverified = PlaneCommits::kNone;
        return false;
    }
    if (out)
        return false;
    mediaFlipUnverified = PlaneCommits::kNone;
    IOLog("SwitchDisplay: windows B and C's completion flip did not latch (set %d written, set %d scanned): that set "
          "flips from the display's work loop\n",
          id, scanned);
    if (SWITCH_FRAME_TIMELINE)
        noteMediaFlipTotal(2, 1);
    withdrawMediaFlip();
    const PlaneSet &set = planeSetOf(id);
    for (unsigned window = 0; window != 2; ++window)
        planeWriteWindowRegisters(planeWindowOf(set, window), set.window[window],
                                  planeOnScreen(static_cast<int>(window)));
    __asm__ volatile("dsb sy" ::: "memory");
    dc[kDcStateControl] = kGeneralUpdate | switch_landscape::overlayUpdates(set.landscape);
    dc[kDcStateControl] = kGeneralActReq | switch_landscape::overlayRequests(set.landscape);
    planeRequestedAt = frameEnds;
    armFlipDeadline();
    return true;
}

#if SWITCH_FRAME_TIMELINE
void noteMediaFlip(uint64_t writtenAt, bool filtered)
{
    const uint64_t now = mach_absolute_time();
    const uint64_t lead = now > writtenAt ? Span::ticksToUs(now - writtenAt) : 0;
    noteMediaFlipTotal(0, 1);
    noteMediaFlipTotal(3, static_cast<uint32_t>(lead));
    if (mediaFlipTotals[0] == 1)
        IOLog("SwitchDisplay: first windows B and C set written by its producers' completion, %llu us before the "
              "display's work loop took it\n",
              static_cast<unsigned long long>(lead));
    static bool loggedFilter;
    if (filtered && !loggedFilter) {
        loggedFilter = true;
        IOLog("SwitchDisplay: first windows B and C set written from host1x's syncpoint filter, %llu us before the "
              "display's work loop took it\n",
              static_cast<unsigned long long>(lead));
    }
}

static void noteMediaFlipTotal(unsigned which, uint32_t n)
{
    __atomic_fetch_add(&mediaFlipTotals[which], n, __ATOMIC_RELAXED);
}

static int mediaFlipTotalSysctl(struct sysctl_oid *, void *, int which, struct sysctl_req *req)
{
    const int value = static_cast<int>(
        __atomic_load_n(&mediaFlipTotals[static_cast<unsigned>(which) % kMediaFlipTotals], __ATOMIC_RELAXED));
    return SYSCTL_OUT(req, &value, sizeof(value));
}

static void mediaFlipTotalsRegister()
{
    static const char *const names[kMediaFlipTotals] = {
        "switch_media_flips_completion",      "switch_media_flips_withdrawn",    "switch_media_flips_refused",
        "switch_media_flips_lead_us",         "switch_media_flips_armed",        "switch_media_flips_filter",
        "switch_media_flips_filter_declined", "switch_media_flips_done_at_swap", "switch_media_flips_fenced",
        "switch_media_flips_declined"};
    for (unsigned i = 0; i < kMediaFlipTotals; ++i) {
        switch_sysctl_oid(&mediaFlipTotalOids[i], &sysctl__kern_children, names[i],
                          static_cast<int>(CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED), mediaFlipTotalSysctl, "I",
                          "windows B and C since boot: media plane sets their producers' completions wrote, ones "
                          "withdrawn before their completion, ones that did not latch as written, microseconds the "
                          "completions' writes came before the gate took them, sets armed for their completion, "
                          "sets host1x's syncpoint filter wrote, producers whose job was done or whose waiter host1x "
                          "refused at the arm, overlay sets whose producers were done at their swap, overlay sets that "
                          "waited for VIC or NVDEC, of those the ones not armed");
        mediaFlipTotalOids[i].oid_arg2 = static_cast<int>(i);
        sysctl_register_oid(&mediaFlipTotalOids[i]);
    }
}
#else
void noteMediaFlip(uint64_t, bool)
{}

static void noteMediaFlipTotal(unsigned, uint32_t)
{}

static void mediaFlipTotalsRegister()
{}
#endif
