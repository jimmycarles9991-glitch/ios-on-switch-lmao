// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

class IOMemoryDescriptor;
class OSObject;
struct SwitchNativeScanout;

namespace switch_display {
// One of window B's slots, its mapping placed in the plane IOVA space at iova (0: nothing mapped). A GPU surface's
// (memory): as a DirectSlot, the surface retained, its memory prepared and its layout held while mapped. An engine
// allocation's (native): its pages mapped once, named by mappedAllocation and mappedGeneration; the frame it shows is
// leased through native->lease, and the mapping outlives that lease.
struct PlaneSlot {
    IOMemoryDescriptor *memory;
    OSObject *surface;
    void *layout;
    uint64_t low, high;  // the surface bytes mapped (page-aligned) at iova
    uint32_t used;
    bool tiled;      // block-linear: window B scans it so
    bool forgotten;  // its allocation was freed while a set used the slot: dropped once none does
    SwitchNativeScanout *native = nullptr;
    uint64_t mappedAllocation = 0, mappedGeneration = 0;
    uint32_t iova = 0;
};

// The last kKeys engine allocations (ID and page-list generation) whose mappings were dropped for room: a later mapping
// of one is a remap, which a pool that fits window B's slots never makes.
struct PlaneDropped {
    static constexpr unsigned kKeys = 64;
    uint64_t allocation[kKeys] = {}, generation[kKeys] = {};
    unsigned next = 0;

    void note(uint64_t id, uint64_t pages)
    {
        if (!id)
            return;
        allocation[next] = id;
        generation[next] = pages;
        next = (next + 1) % kKeys;
    }

    // True once for a key noted.
    bool take(uint64_t id, uint64_t pages)
    {
        for (unsigned i = 0; i < kKeys; ++i) {
            if (id && allocation[i] == id && generation[i] == pages) {
                allocation[i] = 0;
                return true;
            }
        }
        return false;
    }
};

// The engine allocations window B's slots map, published for the backing registry's retirement thread, and the IDs that
// thread forgets (switch_gpu_backing_register_forget) on their way to the gate, which drops their slots. One producer
// (that thread: one work item, never two runs at once) and one consumer (the gate), so the ring needs no lock. The
// thread queues only an ID a slot maps, and the gate drains before each new mapping, so between two drains at most
// Slots + 1 IDs arrive: kIds holds them.
template <unsigned Slots> struct PlaneForgets {
    static constexpr unsigned kIds = 64;
    static_assert(Slots + 1 <= kIds, "an ID for every slot and the one mapped after a drain");
    uint64_t mapped[Slots] = {};
    uint64_t ids[kIds] = {};
    uint32_t head = 0, tail = 0;

    // The gate, as slot k's mapping changes.
    void publish(unsigned k, uint64_t id)
    {
        __atomic_store_n(&mapped[k], id, __ATOMIC_RELAXED);
    }

    // The retirement thread. False: the ring was full and the ID is lost (its slot goes back by least recent use).
    bool forget(uint64_t id)
    {
        bool slot = false;
        for (unsigned k = 0; k < Slots && !slot; ++k)
            slot = id && __atomic_load_n(&mapped[k], __ATOMIC_RELAXED) == id;
        if (!slot)
            return true;
        if (head - __atomic_load_n(&tail, __ATOMIC_ACQUIRE) == kIds)
            return false;
        ids[head % kIds] = id;
        __atomic_store_n(&head, head + 1, __ATOMIC_RELEASE);
        return true;
    }

    // The gate.
    template <class Drop> void drain(Drop drop)
    {
        const uint32_t end = __atomic_load_n(&head, __ATOMIC_ACQUIRE);
        if (tail == end)
            return;
        for (uint32_t at = tail; at != end; ++at)
            drop(ids[at % kIds]);
        __atomic_store_n(&tail, end, __ATOMIC_RELEASE);
    }
};
}  // namespace switch_display
