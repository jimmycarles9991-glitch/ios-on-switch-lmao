// SPDX-License-Identifier: BSD-2-Clause-Patent
// The development probe's prepare and complete lines; see UsbDmaProbe.h.
#include "UsbDmaProbe.h"

#if DEVELOPMENT || DEBUG
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include "../TegraSMMU/TegraSMMU.h"
#include "UsbDmaProbeInternal.h"
#include "UsbSmmuMapper.h"

namespace {
using usb_dma_probe::Aperture;
using usb_dma_probe::cacheable;
using usb_dma_probe::firstBytes;
using usb_dma_probe::Hex;
using usb_dma_probe::readCached;

constexpr uint32_t kProbeLines = 48;      // each of prepare and complete, from boot: the control transfers of an attach
constexpr uint32_t kSummaryEvery = 1024;  // device-writable mappings between summaries once the lines are used up
uint32_t gMapLines, gUnmapLines, gSequence, gWritable;

bool watched(IOMemoryDescriptor *memory)
{
    return (memory->getFlags() & kIOMemoryPreparedReadOnly) == 0u;
}

bool linesLeft()
{
    return __atomic_load_n(&gUnmapLines, __ATOMIC_RELAXED) < kProbeLines;
}

bool takeMapLine()
{
    return __atomic_add_fetch(&gMapLines, 1, __ATOMIC_RELAXED) <= kProbeLines;
}

bool takeUnmapLine()
{
    return __atomic_add_fetch(&gUnmapLines, 1, __ATOMIC_RELAXED) <= kProbeLines;
}

uint64_t lineBytes()
{
    return 4ULL << ((__builtin_arm_rsr64("CTR_EL0") >> 16) & 0xfu);
}
}  // namespace

void usb_dma_probe_map(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, uint32_t mapOptions,
                       uint64_t physical, uint64_t address, const char *path)
{
    const bool writable = watched(memory);
    usb_dma_probe::heldMap(memory, address, path[0] == 'b' ? 0 : physical, path);
    if (!writable)
        return;
    const uint32_t total = __atomic_add_fetch(&gWritable, 1, __ATOMIC_RELAXED);
    if (!takeMapLine()) {
        if (total == kProbeLines + 1 || total % kSummaryEvery == 0)
            usb_dma_probe_summary("lines used up");
        return;
    }
    const uint64_t line = lineBytes();
    const IOBufferMemoryDescriptor *buffer = OSDynamicCast(IOBufferMemoryDescriptor, memory);
    const uint32_t cacheBits = (memory->getFlags() & kIOMemoryBufferCacheMask) >> kIOMemoryBufferCacheShift;
    const Aperture aperture(path[0] == 'b' ? 0 : physical);  // a bounce is copied, never maintained
    IOLog("SwitchUSB: DMA probe #%u prepare %s on CPU %d: %s %p at %#llx pa %#llx (offset %llu) +%llu of %llu, line "
          "offset %llu, %s, options %#x, direction %u, cache mode %u (%s), maintenance va %#llx -> pa %#llx\n",
          __atomic_add_fetch(&gSequence, 1, __ATOMIC_RELAXED), path, cpu_number(),
          memory->getMetaClass()->getClassName(), static_cast<void *>(memory), address, physical, offset, length,
          static_cast<uint64_t>(buffer ? buffer->getCapacity() : memory->getLength()), physical & (line - 1),
          ((physical | (physical + length)) & (line - 1)) == 0 ? "whole lines" : "shares a line", mapOptions,
          static_cast<unsigned>(memory->getDirection()), cacheBits, cacheable(memory) ? "maintained" : "not maintained",
          aperture.va, aperture.back);
}

void usb_dma_probe_unmap_call(const IOMemoryDescriptor *memory, uint64_t address)
{
    usb_dma_probe::heldUnmap(memory, address);
}

uint32_t usb_dma_probe_window_entry(uint64_t address)
{
    return linesLeft() ? switch_usb_smmu_entry(address) : 0;
}

void usb_dma_probe_unmap(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, uint64_t address, uint64_t pa,
                         const char *path, const uint8_t *bounce, uint32_t entry)
{
    if (!watched(memory) || !takeUnmapLine())
        return;
    uint8_t first[8] = {};
    if (bounce) {
        memcpy(first, bounce, firstBytes(length));
    } else {
        (void)memory->readBytes(offset, first, firstBytes(length));
    }
    const Hex bytes(first);
    IOLog("SwitchUSB: DMA probe #%u complete %s on CPU %d: %p at %#llx pa %#llx (offset %llu) +%llu; %s %s; window "
          "entry %#x, MC XUSB_HOST_ASID %#x TRANSLATION_ENABLE_2 %#x\n",
          __atomic_add_fetch(&gSequence, 1, __ATOMIC_RELAXED), path, cpu_number(), static_cast<void *>(memory), address,
          pa, offset, length, bounce ? "pool page" : "after maintenance", bytes.text, entry,
          tegra_smmu_read(TEGRA_SMMU_XUSB_HOST_ASID), tegra_smmu_read(TEGRA_SMMU_TRANSLATION_ENABLE_2));
}

void usb_dma_probe_physical_after(IOMemoryDescriptor *memory, uint64_t pa, uint64_t length)
{
    if (!watched(memory) || !takeUnmapLine())
        return;
    uint8_t first[8] = {};
    const bool throughCache = cacheable(memory);
    if (throughCache)
        readCached(pa, length, first);
    const Hex bytes(first);
    IOLog("SwitchUSB: DMA probe #%u complete own address on CPU %d: %p pa %#llx +%llu; after maintenance %s%s\n",
          __atomic_add_fetch(&gSequence, 1, __ATOMIC_RELAXED), cpu_number(), static_cast<void *>(memory), pa, length,
          bytes.text, throughCache ? "" : " (not read: not cacheable)");
}
#endif
