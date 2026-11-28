// SPDX-License-Identifier: BSD-2-Clause-Patent
// The development probe's counts and its table of mappings still held; see UsbDmaProbe.h.
//
// Maps and unmaps arrive from any work loop on any CPU, and the mapper's own-address path takes no lock, so neither
// does this: a slot is claimed and freed by compare-and-swap on its state.
#include "UsbDmaProbe.h"

#if SWITCH_STATS_IO
#include <IOKit/IOLib.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include "../TegraSMMU/TegraSMMU.h"
#include "UsbDmaProbeInternal.h"

namespace {
constexpr uint32_t kSlots = 128;
constexpr uint32_t kClasses = 8;

enum : uint32_t {
    kFree,
    kWriting,
    kHeld
};

enum Path : uint32_t {
    kOwn,
    kWindow,
    kBounced,
    kPaths
};

struct Slot {
    uint32_t state;
    Path path;
    bool cacheable;
    const IOMemoryDescriptor *memory;  // identity only
    uint64_t address, pa;
    const char *className;
};

struct ClassCount {
    const char *name;
    uint32_t maps, unmaps;
    uint32_t modes;  // bit n: a map in cache mode n (kIOMemoryBufferCacheMask)
};

Slot gSlots[kSlots];
ClassCount gClasses[kClasses];
uint32_t gMaps[kPaths], gUnmaps[kPaths];
uint32_t gUntracked, gUnknownUnmaps;

uint32_t cacheMode(IOMemoryDescriptor *memory)
{
    return (memory->getFlags() & kIOMemoryBufferCacheMask) >> kIOMemoryBufferCacheShift;
}

// A mapping that requires per-transfer completion.
bool wrongToHold(const Slot &slot)
{
    return slot.path == kBounced || slot.cacheable;
}

Path pathOf(const char *path)
{
    if (path[0] == 'o')
        return kOwn;
    return path[0] == 'w' ? kWindow : kBounced;
}

// Class names are the metaclasses' own strings, so one class is one pointer. A class past the table counts nowhere.
ClassCount *classCount(const char *name)
{
    for (ClassCount &entry : gClasses) {
        const char *seen = __atomic_load_n(&entry.name, __ATOMIC_ACQUIRE);
        if (seen == name)
            return &entry;
        const char *expected = nullptr;
        if (!seen &&
            __atomic_compare_exchange_n(&entry.name, &expected, name, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return &entry;
        if (expected == name)
            return &entry;
    }
    return nullptr;
}

// Live objects of the classes a host session should leave nothing of (OSMetaClass instance counts), so a summary tells
// objects never freed from objects freed whose buffers stayed mapped. A class whose kext is not loaded is left out.
void live(const char *when)
{
    static const char *const kClasses[] = {
        "IOUSBHostDevice",
        "IOUSBHostInterface",
        "IOUSBHostPipe",
        "AppleUSBHostIORequestPool",
        "AppleUSBHostDMABufferMemoryDescriptor",
        "AppleUSBHostClientBufferMemoryDescriptor",
        "AppleUSBHostControllerDMACommand",
        "AppleUSBHostDARTDMACommand",
        "AppleUSBHostBouncedDMACommand",
    };
    char line[480] = "";
    size_t used = 0;
    for (const char *name : kClasses) {
        const OSSymbol *symbol = OSSymbol::withCStringNoCopy(name);
        const OSMetaClass *meta = symbol ? OSMetaClass::copyMetaClassWithName(symbol) : nullptr;
        OSSafeReleaseNULL(symbol);
        if (!meta)
            continue;
        const int n = snprintf(line + used, sizeof(line) - used, " %s %u", name, meta->getInstanceCount());
        meta->releaseMetaClass();
        if (n < 0 || static_cast<size_t>(n) >= sizeof(line) - used)
            break;
        used += static_cast<size_t>(n);
    }
    IOLog("SwitchUSB: DMA probe summary (%s): live:%s\n", when, line);
}

}  // namespace

namespace usb_dma_probe {
void heldMap(IOMemoryDescriptor *memory, uint64_t address, uint64_t pa, const char *path)
{
    const Path p = pathOf(path);
    __atomic_add_fetch(&gMaps[p], 1, __ATOMIC_RELAXED);
    const char *name = memory->getMetaClass()->getClassName();
    if (ClassCount *entry = classCount(name)) {
        __atomic_add_fetch(&entry->maps, 1, __ATOMIC_RELAXED);
        __atomic_or_fetch(&entry->modes, 1u << (cacheMode(memory) & 31u), __ATOMIC_RELAXED);
    }
    for (Slot &slot : gSlots) {
        uint32_t expected = kFree;
        if (!__atomic_compare_exchange_n(&slot.state, &expected, kWriting, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            continue;
        slot.path = p;
        slot.cacheable = cacheable(memory);
        slot.memory = memory;
        slot.address = address;
        slot.pa = pa;
        slot.className = name;
        __atomic_store_n(&slot.state, kHeld, __ATOMIC_RELEASE);
        return;
    }
    __atomic_add_fetch(&gUntracked, 1, __ATOMIC_RELAXED);
}

void heldUnmap(const IOMemoryDescriptor *memory, uint64_t address)
{
    for (Slot &slot : gSlots) {
        if (__atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) != kHeld || slot.memory != memory || slot.address != address)
            continue;
        const Path p = slot.path;
        const char *name = slot.className;
        uint32_t expected = kHeld;
        if (!__atomic_compare_exchange_n(&slot.state, &expected, kFree, false, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            continue;
        __atomic_add_fetch(&gUnmaps[p], 1, __ATOMIC_RELAXED);
        if (ClassCount *entry = classCount(name))
            __atomic_add_fetch(&entry->unmaps, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_add_fetch(&gUnknownUnmaps, 1, __ATOMIC_RELAXED);
    if (ClassCount *entry = classCount(memory->getMetaClass()->getClassName()))
        __atomic_add_fetch(&entry->unmaps, 1, __ATOMIC_RELAXED);
}

}  // namespace usb_dma_probe

void usb_dma_probe_summary(const char *when)
{
    uint32_t maps[kPaths] = {};
    uint32_t unmaps[kPaths] = {};
    uint32_t held[kPaths] = {};
    uint32_t wrong = 0;
    for (uint32_t p = 0; p < kPaths; ++p) {
        maps[p] = __atomic_load_n(&gMaps[p], __ATOMIC_RELAXED);
        unmaps[p] = __atomic_load_n(&gUnmaps[p], __ATOMIC_RELAXED);
    }
    for (Slot &slot : gSlots) {
        if (__atomic_load_n(&slot.state, __ATOMIC_ACQUIRE) != kHeld)
            continue;
        ++held[slot.path];
        wrong += wrongToHold(slot) ? 1 : 0;
    }
    uint32_t intstatus = 0;
    uint32_t status = 0;
    uint32_t address = 0;
    const uint32_t errors = tegra_mc_error_latest(&intstatus, &status, &address);
    IOLog("SwitchUSB: DMA probe summary (%s): %u held mappings that must be completed per transfer; maps/unmaps/held: "
          "own address %u/%u/%u, window %u/%u/%u, bounced %u/%u/%u; %u maps untracked (table full), %u unmaps of no "
          "tracked mapping; %u MC errors, the latest INTSTATUS %#x ERR_STATUS %#x ERR_ADR %#x\n",
          when, wrong, maps[kOwn], unmaps[kOwn], held[kOwn], maps[kWindow], unmaps[kWindow], held[kWindow],
          maps[kBounced], unmaps[kBounced], held[kBounced], __atomic_load_n(&gUntracked, __ATOMIC_RELAXED),
          __atomic_load_n(&gUnknownUnmaps, __ATOMIC_RELAXED), errors, intstatus, status, address);
    for (ClassCount &entry : gClasses) {
        const char *name = __atomic_load_n(&entry.name, __ATOMIC_ACQUIRE);
        if (!name)
            continue;
        const uint32_t classMaps = __atomic_load_n(&entry.maps, __ATOMIC_RELAXED);
        const uint32_t classUnmaps = __atomic_load_n(&entry.unmaps, __ATOMIC_RELAXED);
        IOLog("SwitchUSB: DMA probe summary (%s): %s: %u maps, %u unmaps, %d held; cache modes %#x (bit n: mode n)\n",
              when, name, classMaps, classUnmaps, static_cast<int>(classMaps - classUnmaps),
              __atomic_load_n(&entry.modes, __ATOMIC_RELAXED));
    }
    live(when);
}
#endif
