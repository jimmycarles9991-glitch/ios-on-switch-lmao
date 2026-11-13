// SPDX-License-Identifier: BSD-2-Clause-Patent
// NativeLegacyDisplay's window B slots going back: included in the class body; runs under the family's work-loop gate.
//
// An engine allocation stays mapped after the frame it showed retires. Its lease goes back at that latch, so the decoder
// recycles the buffer into its next picture and a GPU or VIC writer renders into it again; that next frame reuses the
// mapping (planeMapSnapshot). Allocation IDs are never reused and a backing's page list changes only with its
// generation, so a slot's key names exactly the pages it maps. A slot without a lease is in no set: window B never
// reads pages a freed allocation gave back, and the next mapping of the slot replaces them.

void planeDropSlot(unsigned k)
{
    planeUseRelease(k);
    PlaneSlot &slot = planeSlots[k];
    if (slot.memory || slot.mappedAllocation || (slot.native && slot.native->lease)) {
        probeDomain.unmap(planeSlotIova(static_cast<int>(k)), static_cast<uint32_t>((slot.high - slot.low) / 4096));
        if (slot.layout)
            tegra_gm20b_surface_unhold(slot.layout);
        if (slot.memory) {
            slot.memory->complete(kIODirectionOut);
            slot.memory->release();
        }
        if (slot.native) {
            switch_native_scanout_deliver(slot.native);
            switch_native_scanout_release(slot.native);
        }
        cleanDomain();
    }
    OSSafeReleaseNULL(slot.surface);
    auto *storage = slot.native;
    slot = {};
    slot.native = storage;
}

void planeReleaseLease(unsigned k)
{
    planeUseRelease(k);
    auto *native = planeSlots[k].native;
    if (native && native->lease) {
        switch_native_scanout_deliver(native);
        switch_native_scanout_release(native);
    }
}

// After a latch: every frame no set shows goes back to its producer; with window B idle, GPU surfaces go back too.
void planeReleaseIdle()
{
    planeUseHoldScanned();
    const bool idle = planeWindowIdle();
    for (unsigned k = 0; k < kPlaneSlots; ++k) {
        if (planeSlotBusy(static_cast<int>(k)))
            continue;
        if (planeSlots[k].mappedAllocation)
            planeReleaseLease(k);
        else if (idle)
            planeDropSlot(k);
    }
}
