// SPDX-License-Identifier: BSD-2-Clause-Patent
// Window B's surfaces in use (SwitchSurfaceUse.h). A slot holds one use count on the IOSurface its lease shows, from the
// kick that takes the surface for window B until the lease goes back: after the latch of the set that follows it, with
// the slot, or at the stop. A producer's pool writes a buffer again once it reads not in use, and a set asked for
// latches at whatever boundary comes next, so the count covers the surface for as long as window B may scan it, as
// the display's own count does on a swap's surfaces from its submission. SC7 keeps the leases and window B shows the
// same set at the resume, so the counts stay too. The count follows the lease, not the mapping, which outlives it.
// Included in the class body after NativePlanesDisplay.h; runs under the family's work-loop gate, in thread context,
// since the last count going may take the surface's own lock.

struct PlaneSurfaceUse {
    OSObject *noted = nullptr;  // retained: the surface the slot's lease was taken from
    uint64_t allocation = 0, generation = 0;
    OSObject *held = nullptr;  // retained while its count is held
    bool counted = false;
};

PlaneSurfaceUse *planeUses = nullptr;  // kPlaneSlots, from the first slot noted: the display class has a size limit

int planeUseNoted(int slot, OSObject *surface)
{
    if (slot < 0 || slot >= int(kPlaneSlots) || !surface)
        return slot;
    if (!planeUses)
        planeUses = IONewZero(PlaneSurfaceUse, kPlaneSlots);
    if (!planeUses)
        return slot;
    auto &use = planeUses[slot];
    if (use.noted != surface) {
        surface->retain();
        if (use.noted)
            use.noted->release();
        use.noted = surface;
    }
    use.allocation = planeSlots[slot].mappedAllocation;
    use.generation = planeSlots[slot].mappedGeneration;
    planeUseHold(unsigned(slot));
    return slot;
}

// The slot's surface counted in use, once, while its lease is the one noted.
void planeUseHold(unsigned k)
{
    auto &use = planeUses[k];
    const auto &slot = planeSlots[k];
    if (use.held || !use.noted || !slot.native || !slot.native->lease || slot.mappedAllocation != use.allocation ||
        slot.mappedGeneration != use.generation)
        return;
    use.held = use.noted;
    use.held->retain();
    use.counted = switch_display::surfaceUseCounted(use.held);
    if (use.counted)
        switch_surface_use_increment(use.held, 0);
    planeUseAnnounce(use);
}

// A latch's step: whatever window B shows is counted (a lease taken before its slot's surface was noted).
void planeUseHoldScanned()
{
    if (!planeSets || !planeUses)
        return;
    const PlaneSet &shown = planeSetOf(planeLatch.active);
    for (unsigned k = 0; k < kPlaneSlots; ++k)
        if (shown.uses(int(k)))
            planeUseHold(k);
}

void planeUseRelease(unsigned k)
{
    if (!planeUses)
        return;
    auto &use = planeUses[k];
    if (use.held) {
        if (use.counted)
            switch_surface_use_decrement(use.held, 0);
        use.held->release();
    }
    if (use.noted)
        use.noted->release();
    use = {};
}

void releasePlaneUses()
{
    if (!planeUses)
        return;
    for (unsigned k = 0; k < kPlaneSlots; ++k)
        planeUseRelease(k);
    IODelete(planeUses, PlaneSurfaceUse, kPlaneSlots);
    planeUses = nullptr;
}

#if (defined(DEVELOPMENT) && DEVELOPMENT) || (defined(DEBUG) && DEBUG)
bool planeUseAnnounced = false;

void planeUseAnnounce(const PlaneSurfaceUse &use)
{
    if (planeUseAnnounced)
        return;
    planeUseAnnounced = true;
    IOLog("SwitchDisplay: window B holds a use count on each surface it scans (the first: use count %u%s)\n",
          switch_surface_use_count(use.held), use.counted ? "" : ", made with kIOSurfaceProhibitUseCount");
}
#else
void planeUseAnnounce(const PlaneSurfaceUse &)
{}
#endif
