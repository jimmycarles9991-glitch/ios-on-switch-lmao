// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDMAMapper: identity IOMapper with cache maintenance and bouncing; see UsbDmaMapper.h.
#include "UsbDmaMapper.h"
#include "UsbDmaCache.h"
#include "UsbDmaProbe.h"
#include "UsbSmmuMapper.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include "../../kits/SwitchDMA/SwitchDcache.h"

namespace {
constexpr uint32_t kPoolBytes = 8u << 20;  // client transfers in flight that need bouncing
constexpr uint64_t kPage = 4096;           // bounce granule (the kernel's PAGE_SIZE is a variable)
constexpr uint32_t kPoolPages = static_cast<uint32_t>(kPoolBytes / kPage);
constexpr uint32_t kMaxMappings = 512;

// A bounced transfer. Its descriptor is not retained: IODMACommand holds it from prepare to complete and passes it to
// the unmap, and a reference of ours would keep Apple's family buffers from ever being freed (their taggedRelease
// starts the teardown, which unmaps them, only once nothing but their own command holds them).
struct Mapping {
    uint64_t address = 0, length = 0, offset = 0;
    const IOMemoryDescriptor *memory = nullptr;  // identity only
    uint32_t firstPage = 0, pages = 0;
    bool writable = false, used = false;
};

IOBufferMemoryDescriptor *gPool;  // switch_usb_dma_reserve(); for the boot

static_assert(kUsbDmaBufferCacheMask == kIOMemoryBufferCacheMask &&
              kUsbDmaBufferCacheShift == kIOMemoryBufferCacheShift);
static_assert(kUsbDmaDefaultCache == kIODefaultCache && kUsbDmaWriteThruCache == kIOWriteThruCache &&
              kUsbDmaCopybackCache == kIOCopybackCache && kUsbDmaCopybackInnerCache == kIOCopybackInnerCache);

bool cacheable(IOMemoryDescriptor *memory)
{
    return usbDmaCacheable(memory->getFlags());
}

// UsbDmaCache.h's operation on the descriptor's range, through its own pages.
void maintain(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, UsbDmaCacheOp op)
{
    if (op == UsbDmaCacheOp::Clean) {
        memory->performOperation(kIOMemoryIncoherentIOStore, offset, length);
    } else if (op == UsbDmaCacheOp::CleanAndInvalidate) {
        memory->performOperation(kIOMemoryIncoherentIOFlush, offset, length);
    }
}

bool deviceMayWrite(IOMemoryDescriptor *memory)
{
    return (memory->getFlags() & kIOMemoryPreparedReadOnly) == 0u;
}

UsbDmaPlan planFor(IOMemoryDescriptor *memory, uint64_t physical, uint64_t length)
{
    return usbDmaPlan(cacheable(memory), deviceMayWrite(memory),
                      usbDmaWholeLines(physical, length, SWITCH_DCACHE_LINE));
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class SwitchDMAMapper : public IOMapper {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(SwitchDMAMapper);

    IOLock *lock = nullptr;
    uint8_t *poolVA = nullptr;
    uint64_t poolPA = 0;
    uint8_t *usedPages = nullptr;  // kPoolPages entries
    Mapping *mappings = nullptr;   // kMaxMappings entries
    bool reportedFull = false;

    int allocatePages(uint32_t count, uint64_t alignment);
    void releasePages(uint32_t first, uint32_t pages);
    uint64_t directAddress(uint64_t physical, uint64_t length) const;
    IOReturn bounce(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, bool writable, uint64_t alignment,
                    uint64_t *mapAddress, uint64_t *mapLength);
    Mapping *slot();
    Mapping *find(uint64_t address, const IOMemoryDescriptor *memory);

protected:
    bool initHardware(IOService *provider) override;

public:
    uint64_t reach = 1ULL << 32;
    void free() override;

    uint64_t getPageSize() const override
    {
        return kPage;
    }

    IOReturn iovmMapMemory(IOMemoryDescriptor *memory, uint64_t descriptorOffset, uint64_t length, uint32_t mapOptions,
                           const IODMAMapSpecification *spec, IODMACommand *command, const IODMAMapPageList *pageList,
                           uint64_t *mapAddress, uint64_t *mapLength) override;
    IOReturn iovmUnmapMemory(IOMemoryDescriptor *memory, IODMACommand *command, uint64_t mapAddress,
                             uint64_t mapLength) override;

    IOReturn iovmInsert(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t) override
    {
        return kIOReturnUnsupported;
    }

    uint64_t mapToPhysicalAddress(uint64_t mappedAddress) override
    {
        return switch_usb_smmu_to_physical(mappedAddress);
    }

#if XNU_10063_141_1
    using IOMapper::callPlatformFunction;
    IOReturn callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1, void *param2,
                                  void *param3, void *param4) override;
#endif
};

OSDefineMetaClassAndStructors(SwitchDMAMapper, IOMapper);

bool SwitchDMAMapper::initHardware(IOService *)
{
    lock = IOLockAlloc();
    usedPages = IONewZero(uint8_t, kPoolPages);
    mappings = IONewZero(Mapping, kMaxMappings);
    if (!lock || !usedPages || !mappings || !gPool)
        return false;
    poolVA = static_cast<uint8_t *>(gPool->getBytesNoCopy());
    poolPA = gPool->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    return true;
}

void SwitchDMAMapper::free()
{
    if (usedPages)
        IODeleteData(usedPages, uint8_t, kPoolPages);
    if (mappings)
        IODelete(mappings, Mapping, kMaxMappings);
    if (lock)
        IOLockFree(lock);
    IOMapper::free();
}

namespace {
// UsbSmmuMapper.h: buffers at IOVAs; what it cannot take is bounced (the pool is at its own IOVA). Below the IONew and
// IODelete sites above, whose kalloc_type views XNU names by __LINE__.
bool gSmmu;
}  // namespace

#if XNU_10063_141_1
// iOS 17's AppleUSBHostController::start asks its mapper the granularity it protects memory at
// (callPlatformFunction("getProtectionGranularity", false, &granularity), a uint32_t read back, the return ignored).
// Left at a page, it keeps a pool of commands that bounce every transfer of a page or less through a cacheable buffer
// of their own, mapped once (AppleUSBHostBouncedDMACommand), out of the mapper's sight. At 1 every transfer gets a
// command that maps on setMemoryDescriptor and unmaps on clearMemoryDescriptor, so the mapper's maintenance runs per
// transfer. iOS 27 is not answered: there the value only shapes buffer and segment specifications.
IOReturn SwitchDMAMapper::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction, void *param1,
                                               void *param2, void *param3, void *param4)
{
    if (functionName && param1 && functionName->isEqualTo("getProtectionGranularity")) {
        *static_cast<uint32_t *>(param1) = 1;
        return kIOReturnSuccess;
    }
    return IOMapper::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}
#endif

int SwitchDMAMapper::allocatePages(uint32_t count, uint64_t alignment)
{
    const uint64_t firstAligned = (-poolPA & (alignment - 1)) / kPage;
    const uint64_t step = alignment > kPage ? alignment / kPage : 1;
    for (uint64_t first = firstAligned; first + count <= kPoolPages; first += step) {
        uint32_t n = 0;
        while (n < count && !usedPages[first + n])
            ++n;
        if (n == count) {
            for (uint32_t i = 0; i < count; ++i)
                usedPages[first + i] = 1;
            return static_cast<int>(first);
        }
    }
    return -1;
}

void SwitchDMAMapper::releasePages(uint32_t first, uint32_t pages)
{
    bzero(usedPages + first, pages);
    reportedFull = false;
}

Mapping *SwitchDMAMapper::slot()
{
    for (uint32_t i = 0; i < kMaxMappings; ++i) {
        if (!mappings[i].used)
            return &mappings[i];
    }
    return nullptr;
}

// By descriptor too: two descriptors over the same memory are at the same direct address.
Mapping *SwitchDMAMapper::find(uint64_t address, const IOMemoryDescriptor *memory)
{
    for (uint32_t i = 0; i < kMaxMappings; ++i) {
        if (mappings[i].used && mappings[i].address == address && mappings[i].memory == memory)
            return &mappings[i];
    }
    return nullptr;
}

// The address a buffer in one physical piece is at for the controller as it is: its own, when the SMMU maps RAM below
// 4 GiB to itself or, without the SMMU, when the controller reaches it. UINT64_MAX otherwise.
uint64_t SwitchDMAMapper::directAddress(uint64_t physical, uint64_t length) const
{
    if (physical == 0u)
        return UINT64_MAX;
    if (gSmmu)
        return switch_usb_smmu_fixed(physical, length);
    return physical < reach && length <= reach - physical ? physical : UINT64_MAX;
}

// A buffer at its own address takes no slot and no lock; one the window can take gets IOVAs; the rest is bounced.
IOReturn SwitchDMAMapper::iovmMapMemory(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length,
                                        uint32_t mapOptions, const IODMAMapSpecification *spec, IODMACommand *,
                                        const IODMAMapPageList *, uint64_t *mapAddress, uint64_t *mapLength)
{
    if (!memory || !length || !mapAddress || !mapLength)
        return kIOReturnBadArgument;
    const uint64_t alignment = spec && spec->alignment > 1 ? spec->alignment : 1;
    const bool writable = deviceMayWrite(memory);
    IOByteCount segment = 0;
    const uint64_t physical = memory->getPhysicalSegment(offset, &segment, kIOMemoryMapperNone);
    // A scattered buffer's pieces meet on page boundaries (UsbIova.h), so its end lies as far into a line as
    // physical + length does.
    const UsbDmaPlan plan = planFor(memory, physical, length);
    const uint64_t direct = segment >= length ? directAddress(physical, length) : UINT64_MAX;
    uint64_t translated = 0;
    if (!plan.bounce && direct != UINT64_MAX && (direct & (alignment - 1)) == 0u) {
        translated = direct;
    } else if (plan.bounce || !gSmmu ||
               !switch_usb_smmu_map(memory, offset, length, writable, alignment, &translated)) {
        const IOReturn bounced = bounce(memory, offset, length, writable, alignment, mapAddress, mapLength);
        if (bounced == kIOReturnSuccess) {
            usb_dma_probe_map(memory, offset, length, mapOptions, physical, *mapAddress, "bounced");
        }
        return bounced;
    }
    const char *path = translated == direct ? "own address" : "window";
    usb_dma_probe_map(memory, offset, length, mapOptions, physical, translated, path);
    maintain(memory, offset, length, plan.atMap);
    *mapAddress = translated;
    *mapLength = length;
    return kIOReturnSuccess;
}

IOReturn SwitchDMAMapper::bounce(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length, bool writable,
                                 uint64_t alignment, uint64_t *mapAddress, uint64_t *mapLength)
{
    if (length > kPoolBytes)
        return kIOReturnNoResources;
    const auto pages = static_cast<uint32_t>((length + kPage - 1) / kPage);
    IOLockLock(lock);
    Mapping *m = slot();
    const int first = m && pages <= kPoolPages ? allocatePages(pages, alignment) : -1;
    if (first < 0) {
        const bool report = !reportedFull;
        reportedFull = true;
        IOLockUnlock(lock);
        if (report)
            IOLog("SwitchUSB: DMA %s full (%llu bytes asked)\n", m ? "bounce pool" : "mapping table", length);
        return kIOReturnNoResources;
    }
    const uint64_t address = poolPA + static_cast<uint64_t>(first) * kPage;
    *m = Mapping{address, length, offset, memory, static_cast<uint32_t>(first), pages, writable, true};
    IOLockUnlock(lock);

    uint8_t *bounce = poolVA + static_cast<uint64_t>(first) * kPage;
    // IOMapper receives no actual byte count on unmap: the untouched suffix of a short or canceled IN must survive.
    if (memory->readBytes(offset, bounce, length) != length) {
        IOLockLock(lock);
        m->used = false;
        releasePages(static_cast<uint32_t>(first), pages);
        IOLockUnlock(lock);
        return kIOReturnUnderrun;
    }
    __asm__ volatile("dsb sy" ::: "memory");  // non-cacheable stores reach memory before the doorbell
    *mapAddress = address;
    *mapLength = length;
    return kIOReturnSuccess;
}

// A buffer at its own address left no record: it is the pa itself, and only a descriptor whose direction is out alone
// was mapped without write access (IODMACommand::walkAll; IOGeneralMemoryDescriptor::dmaMap grants write unless
// prepared read only), so every other one, which the controller may have written, loses its stale lines here.
IOReturn SwitchDMAMapper::iovmUnmapMemory(IOMemoryDescriptor *memory, IODMACommand *, uint64_t mapAddress,
                                          uint64_t mapLength)
{
    uint64_t translatedOffset = 0;
    uint64_t translatedLength = 0;
    bool translatedWritable = false;
    usb_dma_probe_unmap_call(memory, mapAddress);
    const uint32_t entry = usb_dma_probe_window_entry(mapAddress);
    if (gSmmu && switch_usb_smmu_unmap(memory, mapAddress, &translatedOffset, &translatedLength, &translatedWritable)) {
        IOByteCount segment = 0;
        const uint64_t physical = memory->getPhysicalSegment(translatedOffset, &segment, kIOMemoryMapperNone);
        const UsbDmaPlan plan = planFor(memory, physical, translatedLength);
        maintain(memory, translatedOffset, translatedLength, plan.atUnmap);
        usb_dma_probe_unmap(memory, translatedOffset, translatedLength, mapAddress, physical, "window", nullptr, entry);
        return kIOReturnSuccess;
    }
    if (mapLength && mapAddress - poolPA >= kPoolBytes && directAddress(mapAddress, mapLength) == mapAddress) {
        const UsbDmaPlan plan = planFor(memory, mapAddress, mapLength);
        if (plan.atUnmap == UsbDmaCacheOp::CleanAndInvalidate && switch_dma_pa_managed(mapAddress) &&
            switch_dma_pa_managed(mapAddress + mapLength - 1)) {
            switch_dcache_phys_lines(SWITCH_DCACHE_CLEAN_INVALIDATE, mapAddress, mapLength);
            switch_dcache_sync();
        }
        usb_dma_probe_physical_after(memory, mapAddress, mapLength);
        return kIOReturnSuccess;
    }
    IOLockLock(lock);
    Mapping *m = find(mapAddress, memory);
    if (!m) {
        IOLockUnlock(lock);
        // dmaUnmap ignores what this returns: without the line nothing would say a buffer was given back unmaintained.
        IOLog("SwitchUSB: DMA unmap of %#llx +%llu (%s) matches no mapping: no cache maintenance or copy back done\n",
              mapAddress, mapLength, memory->getMetaClass()->getClassName());
        return kIOReturnNotFound;
    }
    const Mapping done = *m;
    m->used = false;
    IOLockUnlock(lock);

    __asm__ volatile("dsb sy" ::: "memory");
    usb_dma_probe_unmap(memory, done.offset, done.length, done.address, 0, "bounced",
                        poolVA + static_cast<uint64_t>(done.firstPage) * kPage, 0);
    const bool copied =
        !done.writable || memory->writeBytes(done.offset, poolVA + static_cast<uint64_t>(done.firstPage) * kPage,
                                             done.length) == done.length;
    IOLockLock(lock);
    releasePages(done.firstPage, done.pages);
    IOLockUnlock(lock);
    if (!copied) {
        IOLog("SwitchUSB: incomplete DMA bounce copy back at %#llx (%llu bytes)\n", done.address, done.length);
        return kIOReturnUnderrun;
    }
    return kIOReturnSuccess;
}

int switch_usb_dma_reserve()
{
    if (gPool)
        return 0;
    IOBufferMemoryDescriptor *pool = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
            static_cast<IOOptionBits>(kIOMapWriteCombineCache),
        kPoolBytes, 0x00000000fffff000ULL);
    const uint64_t physical = pool ? pool->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone) : 0;
    if (!physical || physical + kPoolBytes > (1ULL << 32)) {
        OSSafeReleaseNULL(pool);
        return -1;
    }
    gPool = pool;
    return 0;
}

LIBKERN_RETURNS_NOT_RETAINED IOMapper *switch_usb_dma_mapper(uint64_t reach)
{
    static SwitchDMAMapper *mapper;
    if (mapper)
        return mapper;  // one caller: the host bring-up, on the USB role thread
    IOService *resources = IOService::getResourceService();
    SwitchDMAMapper *candidate = OSTypeAlloc(SwitchDMAMapper);
    if (!resources || !candidate || !candidate->init() || !candidate->attach(resources)) {
        OSSafeReleaseNULL(candidate);
        return nullptr;
    }
    candidate->reach = reach;
    if (!candidate->start(resources)) {
        candidate->detach(resources);
        candidate->release();
        return nullptr;
    }
    candidate->setName("SwitchDMAMapper");
    uint64_t firmwarePa = 0;
    uint64_t firmwareBytes = 0;
    char why[160] = "";
    const uint64_t poolPa = gPool ? gPool->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone) : 0;
    gSmmu = switch_xusb_firmware_range(&firmwarePa, &firmwareBytes) &&
            switch_usb_smmu_start(firmwarePa, firmwareBytes, poolPa, gPool ? kPoolBytes : 0, why, sizeof(why));
    if (!gSmmu) {
        IOLog("SwitchUSB: host DMA by physical address with bounces (SMMU off: %s)\n",
              why[0] ? why : "no firmware image");
    }
    mapper = candidate;
    return mapper;
}

void switch_usb_dma_session_start()
{
    usb_dma_probe_summary("host session start");
}

void switch_usb_dma_session_end()
{
    usb_dma_probe_summary("device role");
}

void switch_usb_dma_controller_off()
{
    usb_dma_probe_summary("controller off");
}
