// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchDMAMapper: identity IOMapper with cache maintenance and bouncing; see UsbDmaMapper.h.
#include "UsbDmaMapper.h"
#include "UsbSmmuMapper.h"
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>

namespace {
constexpr uint32_t kPoolBytes = 8u << 20;  // client transfers in flight that need bouncing
constexpr uint64_t kPage = 4096;           // bounce granule (the kernel's PAGE_SIZE is a variable)
constexpr uint32_t kPoolPages = static_cast<uint32_t>(kPoolBytes / kPage);
constexpr uint32_t kMaxMappings = 512;

struct Mapping {
    uint64_t address = 0, length = 0, offset = 0;
    IOMemoryDescriptor *memory = nullptr;
    uint32_t firstPage = 0, pages = 0;  // bounce pages (pages == 0: mapped as it is)
    bool writable = false, cached = false, used = false;
};

IOBufferMemoryDescriptor *gPool;  // switch_usb_dma_reserve(); for the boot

// Cache mode of an IOBufferMemoryDescriptor (IOMemoryDescriptor kIOMemoryBufferCacheMask); 0 for any other memory.
bool cacheable(IOMemoryDescriptor *memory)
{
    switch ((memory->getFlags() & kIOMemoryBufferCacheMask) >> kIOMemoryBufferCacheShift) {
    case kIODefaultCache:
    case kIOWriteThruCache:
    case kIOCopybackCache:
    case kIOCopybackInnerCache:
        return true;
    default:
        return false;
    }
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
    Mapping *slot();
    Mapping *find(uint64_t address);

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
// UsbSmmuMapper.h: buffers at IOVAs; what it cannot take is bounced (the pool is identity-mapped). Below the IONew and
// IODelete sites above, whose kalloc_type views XNU names by __LINE__.
bool gSmmu;
}  // namespace

int SwitchDMAMapper::allocatePages(uint32_t count, uint64_t alignment)
{
    const uint32_t step = alignment > kPage ? static_cast<uint32_t>(alignment / kPage) : 1;
    for (uint32_t first = 0; first + count <= kPoolPages; first += step) {
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

Mapping *SwitchDMAMapper::slot()
{
    for (uint32_t i = 0; i < kMaxMappings; ++i) {
        if (!mappings[i].used)
            return &mappings[i];
    }
    return nullptr;
}

Mapping *SwitchDMAMapper::find(uint64_t address)
{
    for (uint32_t i = 0; i < kMaxMappings; ++i) {
        if (mappings[i].used && mappings[i].address == address)
            return &mappings[i];
    }
    return nullptr;
}

IOReturn SwitchDMAMapper::iovmMapMemory(IOMemoryDescriptor *memory, uint64_t offset, uint64_t length,
                                        uint32_t mapOptions, const IODMAMapSpecification *spec, IODMACommand *,
                                        const IODMAMapPageList *, uint64_t *mapAddress, uint64_t *mapLength)
{
    if (!memory || !length || !mapAddress || !mapLength)
        return kIOReturnBadArgument;
    const uint64_t alignment = spec && spec->alignment > 1 ? spec->alignment : 1;
    uint64_t translated = 0;
    if (gSmmu && switch_usb_smmu_map(memory, offset, length, (mapOptions & kIODMAMapWriteAccess) != 0u, alignment,
                                     &translated)) {
        memory->retain();
        if (cacheable(memory))
            memory->performOperation(kIOMemoryIncoherentIOStore, offset, length);
        // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): the mapping holds it until iovmUnmapMemory
        *mapAddress = translated;
        *mapLength = length;
        return kIOReturnSuccess;
    }
    IOByteCount segment = 0;
    const uint64_t physical = memory->getPhysicalSegment(offset, &segment, kIOMemoryMapperNone);
    const bool direct = !gSmmu && (physical != 0u) && segment >= length && ((physical & (alignment - 1)) == 0u) &&
                        physical + length <= reach;
    const bool cached = direct && cacheable(memory);
    const bool writable = (mapOptions & kIODMAMapWriteAccess) != 0u;

    IOLockLock(lock);
    Mapping *m = slot();
    int first = -1;
    const uint32_t pages = direct ? 0 : static_cast<uint32_t>((length + kPage - 1) / kPage);
    if (m && !direct)
        first = pages <= kPoolPages ? allocatePages(pages, alignment) : -1;
    if (!m || (!direct && first < 0)) {
        const bool report = !reportedFull;
        reportedFull = true;
        IOLockUnlock(lock);
        if (report)
            IOLog("SwitchUSB: DMA %s full (%llu bytes asked)\n", m ? "bounce pool" : "mapping table", length);
        return kIOReturnNoResources;
    }
    const uint64_t address = direct ? physical : poolPA + static_cast<uint64_t>(first) * kPage;
    *m = Mapping{address, length,   offset, memory, direct ? 0 : static_cast<uint32_t>(first),
                 pages,   writable, cached, true};
    memory->retain();
    IOLockUnlock(lock);

    if (cached) {
        memory->performOperation(kIOMemoryIncoherentIOStore, offset, length);  // clean: the controller reads memory
    } else if (!direct) {
        uint8_t *bounce = poolVA + static_cast<uint64_t>(first) * kPage;
        if (memory->readBytes(offset, bounce, length) != length)
            bzero(bounce, length);
        __asm__ volatile("dsb sy" ::: "memory");  // non-cacheable stores reach memory before the doorbell
    }
    *mapAddress = address;
    *mapLength = length;
    return kIOReturnSuccess;
}

IOReturn SwitchDMAMapper::iovmUnmapMemory(IOMemoryDescriptor *memory, IODMACommand *, uint64_t mapAddress, uint64_t)
{
    uint64_t translatedOffset = 0;
    uint64_t translatedLength = 0;
    bool translatedWritable = false;
    if (gSmmu && switch_usb_smmu_unmap(memory, mapAddress, &translatedOffset, &translatedLength, &translatedWritable)) {
        if (translatedWritable && cacheable(memory))
            memory->performOperation(kIOMemoryIncoherentIOFlush, translatedOffset, translatedLength);
        // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): the reference iovmMapMemory took for the mapping
        memory->release();
        return kIOReturnSuccess;
    }
    IOLockLock(lock);
    Mapping *m = find(mapAddress);
    if (!m || m->memory != memory) {
        IOLockUnlock(lock);
        return kIOReturnNotFound;
    }
    const Mapping done = *m;
    m->used = false;
    IOLockUnlock(lock);

    if (done.cached && done.writable) {
        done.memory->performOperation(kIOMemoryIncoherentIOFlush, done.offset, done.length);  // drop stale lines
    } else if (done.pages) {
        __asm__ volatile("dsb sy" ::: "memory");
        if (done.writable)
            done.memory->writeBytes(done.offset, poolVA + static_cast<uint64_t>(done.firstPage) * kPage, done.length);
        IOLockLock(lock);
        for (uint32_t i = 0; i < done.pages; ++i)
            usedPages[done.firstPage + i] = 0;
        reportedFull = false;
        IOLockUnlock(lock);
    }
    done.memory->release();
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
