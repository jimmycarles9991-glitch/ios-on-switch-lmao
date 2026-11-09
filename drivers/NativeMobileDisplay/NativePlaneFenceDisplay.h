// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's window B changes whose surface the GPU renders: written only once a GPU present fenced on that
// surface's writers completed (NativePlaneFence.h has the rule). Included in the class body after NativePlanesDisplay.h;
// everything here runs under the family's work-loop gate. Planes run on the primary display alone, so the one set waiting
// for its fence is the class's.

static inline PlaneSet planeFencing = {};
static inline bool planeFencePending = false;

static bool planeFenced(const PlaneSet &p)
{
    return p.on && p.image.surfaceKind != 0;
}

// The first page of the surface p scans (the GPU keys a surface's writers by it), 0 for a set nothing on the GPU writes.
uint64_t planeFencePa(const PlaneSet &p) const
{
    if (!planeFenced(p) || !planeSlots[p.slot].memory)
        return 0;
    IOByteCount length = 0;
    return planeSlots[p.slot].memory->getPhysicalSegment(0, &length, kIOMemoryMapperNone) & ~0xfffULL;
}

// The window A present's other surface: window B's, when its change goes out with this frame and the GPU renders it.
uint64_t planeNextAlsoWritten() const
{
    return planeNextSet ? planeFencePa(planeNext) : 0;
}

// A window A frame that no GPU present carries: a change it would carry whose surface the GPU renders is not shown.
void planeNextUnfenced()
{
    if (!planeNextSet ||
        native_planes::planeRoute(planeFenced(planeNext), true, false) != native_planes::PlaneRoute::kNotShown)
        return;
    planeNextSet = false;
    if (stats)
        ++stPlaneDropped;
}

// A plane-only swap's set whose surface the GPU renders: a present of its own, with no copy, whose completion writes it
// (finishPlaneFence). False: the GPU declined it.
bool planeFence(const PlaneSet &p)
{
    const uint64_t pa = planeFencePa(p);
    if (!gpuCopy || !gpuDoneSource || !pa || tegra_gm20b_scanout_fence(this, pa))
        return false;
    planeFencing = p;
    planeFencePending = true;
    planeTarget = p;
    gpuSequence = gpuSequence + 1;
    gpuOfferedAt = mach_absolute_time();
    __atomic_store_n(&gpuScanout, true, __ATOMIC_RELEASE);
    gpuDeadlines = 0;
    gpuDeadline->setTimeoutMS(kGpuCopyDeadlineMS);
    return true;
}

// From the fence's completion: the set out now (or with the window A frame held for a flip, or at the SC7 resume); not shown
// when its writers' work was lost or failed, or planes went off meanwhile.
void finishPlaneFence(int status)
{
    gpuDeadline->cancelTimeout();
    __atomic_store_n(&gpuScanout, false, __ATOMIC_RELEASE);
    planeFencePending = false;
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
