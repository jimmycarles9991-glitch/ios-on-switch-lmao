// SPDX-License-Identifier: BSD-2-Clause-Patent
// A ring entry's START_ADDR: a scan surface's physical address, read once when its mapping was made (surface 0 is the
// boot framebuffer's), or a direct slot's IOVA. Plain loads only: window A's completion flip resolves it with
// interrupts off, where IOMemoryMap::getPhysicalAddress (the descriptor's IORecursiveLock) would panic. Included in the
// class body.

uint32_t surfaceAddress(int i) const
{
    if (i >= kSlot0)
        return static_cast<uint32_t>(kProbeSlot + static_cast<uint64_t>(i - kSlot0) * kSlotSpan);
    return static_cast<uint32_t>(scanAddresses[i]);
}
