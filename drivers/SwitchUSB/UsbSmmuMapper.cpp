// SPDX-License-Identifier: BSD-2-Clause-Patent
// The XUSB host's DMA through the MC SMMU: AppleUSBXHCI's scatter-gather buffers get one contiguous IOVA range over
// their own pages, so large transfers need no bounce copy. See UsbSmmuMapper.h.
//
// The swgroup XUSB_HOST (TegraSMMU.h; Linux drivers/memory/tegra/tegra210.c) goes from bypass to translation once, when
// the mapper starts, after the falcon firmware runs and before AppleUSBXHCI maps anything; TegraSMMU programs it again
// after every SC7, before the host comes back (XusbHost.cpp powerOn accepts its own ASID). What the controller reached
// by physical address until then stays where it was, identity-mapped: the firmware image (the falcon reads it at
// ILOAD_BASE again after every SC7) and the bounce pool (UsbDmaMapper.cpp keeps physical addresses for what it
// bounces). The SMMU does not snoop the CPU's caches:
// table entries are cleaned to memory, then the ASID's PTC and TLB entries flushed (TegraSMMU.h), before an address is
// handed out and after one is taken back.
#include "UsbSmmuMapper.h"
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "../TegraSMMU/TegraSMMU.h"
#include "../TegraSMMU/TegraSMMUDomain.h"
#include "UsbIova.h"
#include "XudcHardware.h"

namespace {
constexpr uint32_t kAsid = TEGRA_SMMU_ASID_XUSB_HOST;
constexpr uint32_t kTranslationBits = (1u << 10) | (1u << 11);
// 64 MiB of transfers in flight from 1 MiB up: below every RAM physical address (0x80000000 on), so the identity maps
// never meet it.
constexpr uint64_t kWindowBase = 1ULL << 20, kWindowBytes = 64ULL << 20;
constexpr uint32_t kWindowPages = static_cast<uint32_t>(kWindowBytes / UsbIovaWindow::kPage);
constexpr uint32_t kTablePages = 1 + static_cast<uint32_t>(kWindowBytes / TegraSMMUDomain::kSpan) + 8;

struct Slot {
    IOMemoryDescriptor *memory;
    uint64_t offset;
    uint32_t length, pages;
    bool writable;
};

IOLock *gLock;
xudc::DmaBuffer gTables;
TegraSMMUDomain gDomain;
UsbIovaWindow gWindow;
uint8_t *gUsed;
Slot *gSlots;  // by the window index of a mapping's first page
bool gAttached;

void publishTables()
{
    uintptr_t low = 0;
    uintptr_t high = 0;
    if (gDomain.takeDirty(&low, &high))
        switch_dcache_clean_invalidate(reinterpret_cast<const void *>(low), high - low);
    tegra_smmu_flush(kAsid);
}

auto segments(IOMemoryDescriptor *memory, uint64_t offset)
{
    return [memory, offset](uint64_t done, uint64_t *bytes) {
        IOByteCount segment = 0;
        const uint64_t pa = memory->getPhysicalSegment(offset + done, &segment, kIOMemoryMapperNone);
        *bytes = segment;
        return pa;
    };
}
}  // namespace

bool switch_usb_smmu_start(uint64_t firmwarePa, uint64_t firmwareBytes, uint64_t poolPa, uint64_t poolBytes, char *why,
                           size_t whySize)
{
    if (gAttached)
        return true;
    gLock = IOLockAlloc();
    gUsed = IONewZero(uint8_t, kWindowPages);
    gSlots = IONewZero(Slot, kWindowPages);
    if (!gLock || !gUsed || !gSlots || !gTables.allocate(kTablePages * static_cast<uint32_t>(TegraSMMUDomain::kPage)) ||
        !gDomain.init(gTables.va, gTables.pa, kTablePages)) {
        snprintf(why, whySize, "no memory for the SMMU tables");
        return false;
    }
    gWindow.init(gUsed, kWindowPages, kWindowBase);
    if (!gDomain.reserve(kWindowBase, kWindowBytes) ||
        (firmwareBytes && !gDomain.identity(firmwarePa, firmwareBytes, false)) ||
        (poolBytes && !gDomain.identity(poolPa, poolBytes, true))) {
        snprintf(why, whySize, "the SMMU tables cannot hold the window and the identity maps");
        return false;
    }
    switch_dcache_clean_invalidate(gTables.va, gTables.size);
    uintptr_t low = 0;
    uintptr_t high = 0;
    (void)gDomain.takeDirty(&low, &high);
    if (tegra_smmu_attach(kAsid, gDomain.ptb(), TEGRA_SMMU_XUSB_HOST_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_2,
                          kTranslationBits, why, whySize))
        return false;
    gAttached = true;
    return true;
}

bool switch_usb_smmu_map(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, bool writable,
                         uint64_t alignment, uint64_t *address)
{
    uint64_t firstPa = 0;
    if (!gAttached || !length ||
        !usb_iova_page_aligned(segments(memory, offset), length, TegraSMMUDomain::kLimit * 4, &firstPa))
        return false;
    const uint64_t inPage = firstPa & (UsbIovaWindow::kPage - 1);
    if (alignment > 1 && (inPage & (alignment - 1)))
        return false;
    const uint32_t pages = UsbIovaWindow::pagesFor(inPage, length);
    const uint32_t alignPages =
        alignment > UsbIovaWindow::kPage ? static_cast<uint32_t>(alignment / UsbIovaWindow::kPage) : 1;
    IOLockLock(gLock);
    const int64_t index = gWindow.take(pages, alignPages);
    if (index < 0) {
        IOLockUnlock(gLock);
        return false;
    }
    const uint64_t iova = gWindow.iova(static_cast<uint32_t>(index));
    uint32_t page = 0;
    const bool ok = usb_iova_map_pages(
        segments(memory, offset), length, pages,
        [iova](uint32_t index, uint64_t pa) {
            return gDomain.map(iova + static_cast<uint64_t>(index) * UsbIovaWindow::kPage, &pa, 1, true);
        },
        &page);
    if (!ok) {
        gDomain.unmap(iova, page);
        gWindow.give(static_cast<uint32_t>(index), pages);
        IOLockUnlock(gLock);
        return false;
    }
    publishTables();
    gSlots[index] = {memory, offset, static_cast<uint32_t>(length), pages, writable};
    IOLockUnlock(gLock);
    *address = iova + inPage;
    return true;
}

bool switch_usb_smmu_owns(uint64_t address)
{
    return gAttached && gWindow.contains(address);
}

bool switch_usb_smmu_unmap(const IOMemoryDescriptor *memory, uint64_t address, uint64_t *offset, uint64_t *length,
                           bool *writable)
{
    if (!switch_usb_smmu_owns(address))
        return false;
    IOLockLock(gLock);
    const uint32_t index = gWindow.index(address);
    const Slot slot = gSlots[index];
    if (slot.memory != memory || !slot.pages) {
        IOLockUnlock(gLock);
        return false;
    }
    gDomain.unmap(gWindow.iova(index), slot.pages);
    publishTables();
    gWindow.give(index, slot.pages);
    gSlots[index] = {};
    IOLockUnlock(gLock);
    *offset = slot.offset;
    *length = slot.length;
    *writable = slot.writable;
    return true;
}

uint64_t switch_usb_smmu_to_physical(uint64_t address)
{
    if (!switch_usb_smmu_owns(address))
        return address;
    IOLockLock(gLock);
    const uint64_t pa = gDomain.translate(address);
    IOLockUnlock(gLock);
    return pa;
}
