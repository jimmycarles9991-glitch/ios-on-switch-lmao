// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's window B changes whose surface the GPU renders: written only once a GPU present fenced on that
// surface's writers completed (NativePlaneFence.h has the rule). Included in the class body after NativePlanesDisplay.h;
// everything here runs under the family's work-loop gate. Planes run on the primary display alone, so the one set waiting
// for its fence is the class's.

static inline PlaneSet planeFencing = {};
static inline bool planeFencePending = false;

bool planeFenced(const PlaneSet &p) const
{
    for (const auto &overlay : p.window) {
        if (!overlay.on)
            continue;
        const auto *native = planeSlots[overlay.slot].native;
        if (native && native->lease) {
            if (planeOverlayWaits(*native))
                return true;
        } else if (overlay.image.surfaceKind) {
            return true;
        }
    }
    return false;
}

uint64_t planeFencePa(const PlaneSet &p) const
{
    for (const auto &overlay : p.window) {
        if (!overlay.on || planeSlots[overlay.slot].native || !overlay.image.surfaceKind ||
            !planeSlots[overlay.slot].memory)
            continue;
        IOByteCount length = 0;
        return planeSlots[overlay.slot].memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone) & ~0xfffULL;
    }
    return 0;
}

uint64_t planeNextAlsoWritten() const
{
    return planeNextSet ? planeFencePa(planeNext) : 0;
}

// A plane-only swap's set whose surface the GPU renders: a present of its own, with no copy, whose completion writes it
// (finishPlaneFence). False: the GPU declined it.
bool planeFence(const PlaneSet &p)
{
    if (!gpuCopy || !gpuDoneSource || !copySource || copyStopping)
        return false;
    planeFencing = p;
    planeFencePending = true;
    planeTarget = p;
    gpuSequence = gpuSequence + 1;
    frameInfo->tickets.gpu(p.seq, gpuSequence);
#if SWITCH_FRAME_TIMELINE
    const auto *ticket = frameInfo->tickets.byTake(p.seq);
    KDBG(SWITCH_KDBG_CODE(SWITCH_KDBG_PRESENT), gpuSequence, ticket ? ticket->id : 0, ticket ? ticket->swapId : 0,
         p.seq);
#endif
    gpuOfferedAt = mach_absolute_time();
    __atomic_store_n(&gpuScanout, true, __ATOMIC_RELEASE);
    gpuDeadlines = 0;
    gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
    if (!planeMediaFence(p)) {
        planeFenceQueued = true;
        copySource->interruptOccurred(nullptr, nullptr, 0);
    }
    return true;
}

// From the fence's completion: the set out now (or with the window A frame held for a flip, or at the SC7 resume); not shown
// when its writers' work was lost or failed, or planes went off meanwhile.
void finishPlaneFence(int status)
{
    mediaFlipPass();
    gpuDeadline->cancelTimeout();
    __atomic_store_n(&gpuScanout, false, __ATOMIC_RELEASE);
    planeFencePending = false;
    if (copyPlaneJoined) {
        copiedPlaneFenceFinished(status);
        return;
    }
    transferred = false;
    if (status || !planesOn) {
        planeNotShown(planeFencing);
    } else if (hwSuspended) {
        planeHeld = planeFencing;
        planeHeldSet = true;
    } else {
        planeOut(planeFencing);
    }
    kickSwapQueue();
}

// p reached no register: counted; its swap retires with whatever is still out, and the target is what window B keeps.
// NOLINTNEXTLINE(misc-no-recursion): kickSwapQueue's kicking flag stops re-entry; a nested kick only loops
void planeNotShown(const PlaneSet &p)
{
    if (stats)
        ++stPlaneDropped;
    if (planeHeldSet) {
        planeTarget = planeHeld;
    } else {
        planeTarget = planeSetOf(planeLatch.requested() ? planeLatch.armed : planeLatch.active);
    }
    retireUnshown(p.seq);
}
