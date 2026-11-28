// SPDX-License-Identifier: BSD-2-Clause-Patent
// The XUSB host's DMA through the MC SMMU: RAM below 4 GiB at its own address (UsbStaticMap.h host()), so a buffer in
// one piece there needs nothing per request, and AppleUSBXHCI's scatter-gather buffers, and buffers above 4 GiB, get
// one contiguous IOVA range over their own pages in the 64 MiB window below RAM, so large transfers need no bounce
// copy. See UsbSmmuMapper.h.
//
// The swgroup XUSB_HOST (TegraSMMU.h; Linux drivers/memory/tegra/tegra210.c) goes from bypass to translation once, when
// the mapper starts, after the falcon firmware runs and before AppleUSBXHCI maps anything; TegraSMMU programs it again
// after every SC7, before the host comes back (XusbHost.cpp powerOn accepts its own ASID). What the controller reached
// by physical address until then stays at its own address: the firmware image, read only (the falcon reads it at
// ILOAD_BASE again after every SC7), and the bounce pool (UsbDmaMapper.cpp keeps physical addresses for what it
// bounces). The tables, the window and their flushes are UsbSmmuSpace.h's, as the XUDC's are.
#include "UsbSmmuMapper.h"
#include <IOKit/IOLib.h>
#include "../TegraSMMU/TegraSMMU.h"
#include "UsbSmmuSpace.h"

namespace {
constexpr uint32_t kAsid = TEGRA_SMMU_ASID_XUSB_HOST;
constexpr uint32_t kTranslationBits = (1u << 10) | (1u << 11);

struct Slot {
    IOMemoryDescriptor *memory;
    uint64_t offset;
    uint32_t length, pages;
    bool writable;
};

UsbSmmuSpace gSpace;
Slot *gSlots;  // by the window index of a mapping's first page, under gSpace.lock()

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
    if (gSpace.attached())
        return true;
    UsbRam ram = {};
    if (!UsbSmmuSpace::ram(&ram, why, whySize) ||
        !gSpace.init(kAsid, UsbStaticMap::host(ram, firmwarePa, firmwareBytes), why, whySize))
        return false;
    if ((firmwareBytes && gSpace.fixedIova(firmwarePa, firmwareBytes) != firmwarePa) ||
        (poolBytes && gSpace.fixedIova(poolPa, poolBytes) != poolPa)) {
        snprintf(why, whySize, "the firmware image or the bounce pool is not at its own IOVA");
        return false;
    }
    gSlots = IONewZero(Slot, gSpace.windowPages());
    if (!gSlots) {
        snprintf(why, whySize, "no memory for the window's slots");
        return false;
    }
    if (!gSpace.attach(TEGRA_SMMU_XUSB_HOST_ASID, TEGRA_SMMU_TRANSLATION_ENABLE_2, kTranslationBits, why, whySize))
        return false;
#if DEVELOPMENT || DEBUG
    IOLog("SwitchUSB: host DMA through the SMMU, RAM below 4 GiB at its own address, a %u-page window\n",
          gSpace.windowPages());
#endif
    return true;
}

bool switch_usb_smmu_map(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, bool writable,
                         uint64_t alignment, uint64_t *address)
{
    uint64_t firstPa = 0;
    if (!gSpace.attached() || !length ||
        !usb_iova_page_aligned(segments(memory, offset), length, TegraSMMUDomain::kLimit * 4, &firstPa))
        return false;
    const uint64_t inPage = firstPa & (UsbIovaWindow::kPage - 1);
    if (alignment > 1 && (inPage & (alignment - 1)))
        return false;
    const uint32_t pages = UsbIovaWindow::pagesFor(inPage, length);
    const uint32_t alignPages =
        alignment > UsbIovaWindow::kPage ? static_cast<uint32_t>(alignment / UsbIovaWindow::kPage) : 1;
    gSpace.lock();
    const int64_t index = gSpace.mapUnlocked(pages, alignPages, [memory, offset, length, pages](auto mapPage) {
        uint32_t mapped = 0;
        return usb_iova_map_pages(segments(memory, offset), length, pages, mapPage, &mapped);
    });
    if (index >= 0)
        gSlots[index] = {memory, offset, static_cast<uint32_t>(length), pages, writable};
    gSpace.unlock();
    if (index < 0)
        return false;
    *address = gSpace.iova(static_cast<uint32_t>(index)) + inPage;
    return true;
}

uint64_t switch_usb_smmu_fixed(uint64_t pa, uint64_t length)
{
    return gSpace.attached() ? gSpace.fixedIova(pa, length) : UINT64_MAX;
}

#if SWITCH_STATS_IO
uint32_t switch_usb_smmu_entry(uint64_t address)
{
    return gSpace.attached() ? gSpace.entry(address) : 0;
}
#endif

bool switch_usb_smmu_owns(uint64_t address)
{
    return gSpace.owns(address);
}

bool switch_usb_smmu_unmap(const IOMemoryDescriptor *memory, uint64_t address, uint64_t *offset, uint64_t *length,
                           bool *writable)
{
    if (!switch_usb_smmu_owns(address))
        return false;
    const uint32_t index = gSpace.index(address);
    gSpace.lock();
    const Slot slot = gSlots[index];
    if (slot.memory != memory || !slot.pages) {
        gSpace.unlock();
        return false;
    }
    gSlots[index] = {};
    gSpace.unmapUnlocked(index, slot.pages);
    gSpace.unlock();
    *offset = slot.offset;
    *length = slot.length;
    *writable = slot.writable;
    return true;
}

uint64_t switch_usb_smmu_to_physical(uint64_t address)
{
    return switch_usb_smmu_owns(address) ? gSpace.translate(address) : address;
}
