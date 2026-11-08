// SPDX-License-Identifier: BSD-2-Clause-Patent
// Disposable RAM Data for the native root candidate. Never writes a transport.
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <IOKit/IOBSD.h>
#include <sys/param.h>
#include <libkern/crypto/sha2.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/boot.h>
#include "../../kits/SwitchDT/SwitchDTProperty.h"
#include "SparseDisk.h"
#include <NativeDataSeed.h>

static constexpr uint64_t kResidentLimit = 128ULL << 20;
static constexpr uint64_t kMaximumLogical = 1ULL << 30;
static constexpr uint64_t kMaximumRequest = 65536;
static_assert((kNativeDataSeedLogicalBytes != 0u) && kNativeDataSeedLogicalBytes <= kMaximumLogical &&
                  ((kNativeDataSeedLogicalBytes & 4095) == 0u),
              "bounded page-aligned logical disk");
static_assert(kNativeDataSeedEncodedBytes >= 32 && kNativeDataSeedEncodedBytes <= (32ULL << 20), "bounded seed input");
static_assert(kNativeDataSeedNonzeroPages <= kResidentLimit / 4096, "seed fits resident cap");

extern "C" {
extern unsigned long gPhysBase, gPhysSize;
volatile uint32_t native_ram_block_ready = 0;
volatile uint32_t native_ram_block_stage = 0, native_ram_block_error = 0;
volatile uint64_t native_ram_block_bytes = 0;
volatile uint64_t native_ram_block_reads = 0, native_ram_block_writes = 0, native_ram_block_failures = 0;
volatile uint64_t native_ram_block_resident_bytes = 0;
volatile uint32_t native_ram_block_sparse = 0;
uint8_t native_ram_block_seed_sha256[32];
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class NativeRamBlock : public IOBlockStorageDevice {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(NativeRamBlock) IOLock *gate = nullptr;
    RamSparseDisk sparse = {};
    uint8_t *scratch = nullptr;
    bool online = false;
    bool claimed = false;
    void releaseBacking();

    bool failStart(uint32_t error)
    {
        native_ram_block_error = error;
        releaseBacking();
        return false;
    }

public:
    bool available()
    {
        if (!gate)
            return false;
        IOLockLock(gate);
        const bool value = online;
        IOLockUnlock(gate);
        return value;
    }

    bool seedInfo(uint64_t *base, uint64_t *bytes, uint8_t hash[32])
    {
        if (!gate || !base || !bytes || !hash)
            return false;
        IOLockLock(gate);
        const bool value = online;
        if (value) {
            *base = 0;
            *bytes = sparse.bytes;
            memcpy(hash, kNativeDataSeedLogicalSHA256, 32);
        }
        IOLockUnlock(gate);
        return value;
    }

    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;

    IOReturn doEjectMedia() override
    {
        return kIOReturnUnsupported;
    }

    IOReturn doFormatMedia(UInt64) override
    {
        return kIOReturnUnsupported;
    }

    UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override
    {
        return 0;
    }

    IOReturn doUnmap(IOBlockStorageDeviceExtent *, UInt32, IOStorageUnmapOptions) override
    {
        return kIOReturnUnsupported;
    }

    char *getVendorString() override
    {
        static char vendor[] = "Switch";
        return vendor;
    }

    char *getProductString() override
    {
        static char product[] = "Disposable RAM Data";
        return product;
    }

    char *getRevisionString() override
    {
        static char revision[] = "2.0";
        return revision;
    }

    char *getAdditionalDeviceInfoString() override
    {
        static char info[] = "volatile; 128 MiB resident page cap; no persistent writes";
        return info;
    }

    IOReturn reportBlockSize(UInt64 *value) override
    {
        if (!value)
            return kIOReturnBadArgument;
        *value = 512;
        return kIOReturnSuccess;
    }

    IOReturn reportMaxValidBlock(UInt64 *value) override
    {
        if (!value || !sparse.bytes)
            return kIOReturnBadArgument;
        *value = sparse.bytes / 512 - 1;
        return kIOReturnSuccess;
    }

    IOReturn reportEjectability(bool *value) override
    {
        if (!value)
            return kIOReturnBadArgument;
        *value = false;
        return kIOReturnSuccess;
    }

    IOReturn reportRemovability(bool *value) override
    {
        return reportEjectability(value);
    }

    IOReturn reportWriteProtection(bool *value) override
    {
        return reportEjectability(value);
    }

    IOReturn reportMediaState(bool *present, bool *changed) override
    {
        if (!present)
            return kIOReturnBadArgument;
        *present = available();
        if (changed)
            *changed = false;
        return kIOReturnSuccess;
    }

    // Writes commit to RAM while holding the same lock. Flush waits behind all
    // accepted writes; it promises RAM visibility only, never reboot durability.
    IOReturn doSynchronizeCache() override
    {
        return available() ? kIOReturnSuccess : kIOReturnNotReady;
    }

    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override
    {
        return doSynchronizeCache();
    }

    IOReturn getWriteCacheState(bool *value) override
    {
        return reportEjectability(value);
    }

    IOReturn setWriteCacheState(bool) override
    {
        return kIOReturnUnsupported;
    }

    IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *,
                              IOStorageCompletion *) override;
};

OSDefineMetaClassAndStructors(NativeRamBlock, IOBlockStorageDevice)

    bool NativeRamBlock::start(IOService *provider)
{
    // Native SD root boots (every boot of this port) publish persistent Data through the shared SD owner.
    // Refuse before claiming the seed or allocating its duplicate-UUID RAM disk.
    constexpr bool kNativeRootBoot = true;
    if (kNativeRootBoot)
        return false;
    // One Data seed/provider per boot. A failed first attempt is not retried.
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&native_ram_block_stage, &expected, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return false;
    claimed = true;
    if (!IOBlockStorageDevice::start(provider))
        return failStart(1);
    const IORegistryEntry *map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
    if (!map)
        return failStart(2);
    // Legacy control RAMDisk/PackedRAMDisk properties may coexist, but are never
    // read by this driver and cannot substitute for NativeDataSeed.
    uint64_t range[2] = {};
    switch_dt_value(map, "NativeDataSeed", &range);
    map->release();
    const uint64_t physical = range[0];
    const uint64_t encoded = range[1];
    const uint64_t reserved = (kNativeDataSeedEncodedBytes + 4095) & ~4095ULL;
    const boot_args *boot = static_cast<const boot_args *>(PE_state.bootArgs);
    if (encoded != kNativeDataSeedEncodedBytes || (physical & 4095) || !boot || physical < gPhysBase ||
        reserved > gPhysSize || physical - gPhysBase > gPhysSize - reserved || boot->topOfKernelData < physical ||
        reserved > boot->topOfKernelData - physical)
        return failStart(3);
    const uint8_t *input = reinterpret_cast<const uint8_t *>(ml_static_ptovirt(physical));
    if (!input)
        return failStart(3);
    uint8_t encodedHash[32];
    SHA256_CTX hash;
    SHA256_Init(&hash);
    SHA256_Update(&hash, input, encoded);
    SHA256_Final(encodedHash, &hash);
    if (memcmp(encodedHash, kNativeDataSeedEncodedSHA256, 32))
        return failStart(4);
    uint64_t logical = 0;
    if (!ramPackedLayout(input, encoded, &logical) || logical != kNativeDataSeedLogicalBytes)
        return failStart(5);
    native_ram_block_stage = 2;
    gate = IOLockAlloc();
    if (!gate)
        return failStart(6);
    sparse.bytes = logical;
    sparse.residentLimit = kResidentLimit;
    sparse.pages = static_cast<uint8_t **>(IOMallocZero((logical / 4096) * sizeof(uint8_t *)));
    scratch = static_cast<uint8_t *>(IOMallocZero(kMaximumRequest));
    if (!sparse.pages || !scratch)
        return failStart(6);

    struct ImportOps {
        static uint8_t *allocate()
        {
            return static_cast<uint8_t *>(IOMallocZero(4096));
        }
    } ops;

    native_ram_block_stage = 3;
    if (!ramSparseImport(sparse, input, encoded, ops) || sparse.resident != kNativeDataSeedNonzeroPages * 4096)
        return failStart(6);
    if (!setProperty(kIOMaximumBlockCountReadKey, static_cast<UInt64>(kMaximumRequest / 512), 64) ||
        !setProperty(kIOMaximumByteCountReadKey, static_cast<UInt64>(kMaximumRequest), 64) ||
        !setProperty(kIOMaximumBlockCountWriteKey, static_cast<UInt64>(kMaximumRequest / 512), 64) ||
        !setProperty(kIOMaximumByteCountWriteKey, static_cast<UInt64>(kMaximumRequest), 64))
        return failStart(7);
    // The build helper computes the full logical digest from this exact encoded
    // input. Runtime authentication of encoded bytes plus checked geometry binds
    // that compiled digest; it does not rehash 1 GiB of logical zeros on device.
    memcpy(native_ram_block_seed_sha256, kNativeDataSeedLogicalSHA256, 32);
    native_ram_block_bytes = logical;
    native_ram_block_sparse = 1;
    native_ram_block_resident_bytes = sparse.resident;
    IOLockLock(gate);
    online = true;
    IOLockUnlock(gate);
    native_ram_block_stage = 4;
    native_ram_block_ready = 1;
    registerService();
    return true;
}

IOReturn NativeRamBlock::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count, IOStorageAttributes *,
                                          IOStorageCompletion *completion)
{
    if (!buffer || !completion || !completion->action || !count || count > kMaximumRequest / 512 || !sparse.bytes ||
        block >= sparse.bytes / 512 || count > sparse.bytes / 512 - block || buffer->getLength() < count * 512 ||
        (buffer->getDirection() != kIODirectionIn && buffer->getDirection() != kIODirectionOut))
        return kIOReturnBadArgument;
    if (!gate)
        return kIOReturnNotReady;
    IOLockLock(gate);
    if (!online) {
        IOLockUnlock(gate);
        return kIOReturnNotReady;
    }
    const uint64_t bytes = count * 512;
    uint64_t actual = 0;
    IOReturn status = kIOReturnSuccess;
    if (buffer->getDirection() == kIODirectionOut) {
        __atomic_fetch_add(&native_ram_block_writes, 1, __ATOMIC_RELAXED);
        if (buffer->readBytes(0, scratch, bytes) != bytes) {
            status = kIOReturnIOError;
        } else {
            struct WriteOps {
                static uint8_t *allocate()
                {
                    return static_cast<uint8_t *>(IOMallocZero(4096));
                }

                static void release(uint8_t *page)
                {
                    IOFree(page, 4096);
                }
            } ops;

            if (ramSparseWriteAtomic(sparse, block * 512, scratch, bytes, ops)) {
                actual = bytes;
            } else {
                status = kIOReturnNoMemory;
            }
        }
    } else {
        __atomic_fetch_add(&native_ram_block_reads, 1, __ATOMIC_RELAXED);

        struct ReadOps {
            IOMemoryDescriptor *buffer;

            static uint8_t *allocate()
            {
                return nullptr;
            }

            static uint64_t read(uint64_t, void *, uint64_t)
            {
                return 0;
            }

            uint64_t copy(uint64_t offset, const void *data, uint64_t n) const
            {
                return buffer->writeBytes(offset, data, n);
            }
        } ops = {buffer};

        bool unused = false;
        actual = ramSparseIO(sparse, false, block * 512, bytes, ops, unused);
        if (actual != bytes)
            status = kIOReturnIOError;
    }
    native_ram_block_resident_bytes = sparse.resident;
    if (status)
        __atomic_fetch_add(&native_ram_block_failures, 1, __ATOMIC_RELAXED);
    IOLockUnlock(gate);
    // Completed exactly once, outside the lock: callbacks may reenter the driver.
    completion->action(completion->target, completion->parameter, status, actual);
    return kIOReturnSuccess;
}

void NativeRamBlock::stop(IOService *provider)
{
    if (gate) {
        IOLockLock(gate);
        online = false;
        IOLockUnlock(gate);
    }
    if (claimed)
        native_ram_block_ready = 0;
    IOBlockStorageDevice::stop(provider);
}

void NativeRamBlock::releaseBacking()
{
    if (sparse.pages) {
        for (uint64_t i = 0; i < sparse.bytes / 4096; ++i) {
            if (sparse.pages[i])
                IOFree(sparse.pages[i], 4096);
        }
        IOFree(sparse.pages, (sparse.bytes / 4096) * sizeof(uint8_t *));
    }
    sparse = {};
    if (scratch) {
        IOFree(scratch, kMaximumRequest);
        scratch = nullptr;
    }
    if (gate) {
        IOLockFree(gate);
        gate = nullptr;
    }
    online = false;
    if (claimed)
        native_ram_block_resident_bytes = 0;
}

void NativeRamBlock::free()
{
    // IOKit owns this object's request lifetime. stop drains the serialized
    // request before final release; the loader's immutable seed is never freed.
    releaseBacking();
    if (claimed)
        native_ram_block_ready = 0;
    IOBlockStorageDevice::free();
}

static bool validBSDName(const char *name, size_t n)
{
    if (!name || !n || n > 57)
        return false;
    for (size_t i = 0; i < n; ++i) {
        if ((name[i] < 'a' || name[i] > 'z') && (name[i] < 'A' || name[i] > 'Z') && (name[i] < '0' || name[i] > '9'))
            return false;
    }
    return name[n] == 0;
}

static int selectVolumeSeed(const char *uuid, dev_t *device, char *path, size_t size, uint64_t *seedBase,
                            uint64_t *seedBytes, uint8_t seedDigest[32])
{
    if (device)
        *device = NODEV;
    if (path && size)
        path[0] = 0;
    if (!uuid || strcmp(uuid, kNativeDataSeedUUID) || !device || !path || size < 6)
        return 0;
    OSDictionary *matching = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    const OSString *wanted = OSString::withCString(uuid);
    if (!matching || !properties || !wanted) {
        OSSafeReleaseNULL(wanted);
        OSSafeReleaseNULL(properties);
        OSSafeReleaseNULL(matching);
        return 0;
    }
    const bool ready = properties->setObject("UUID", wanted) && matching->setObject("IOPropertyMatch", properties);
    wanted->release();
    properties->release();
    OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
    matching->release();
    if (!iterator)
        return 0;
    bool selected = false;
    unsigned matches = 0;
    dev_t candidate = NODEV;
    char candidatePath[64] = {};
    uint64_t base = 0;
    uint64_t bytes = 0;
    uint8_t hash[32] = {};
    while (const OSObject *object = iterator->getNextObject()) {
        const IOService *media = OSDynamicCast(IOService, object);
        if (!media)
            continue;
        if (++matches != 1) {
            selected = false;
            break;
        }
        const OSString *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
        const OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
        const OSNumber *majorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
        const OSNumber *minorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
        if (media->isInactive() || !identity || strcmp(identity->getCStringNoCopy(), uuid) || !name ||
            !validBSDName(name->getCStringNoCopy(), name->getLength()) || name->getLength() + 6 > size ||
            !majorNumber || !minorNumber || majorNumber->unsigned64BitValue() > 255 ||
            minorNumber->unsigned64BitValue() > 0xffffff)
            continue;
        unsigned depth = 0;
        const IORegistryEntry *entry = media;
        entry->retain();
        while (entry && depth++ < 64) {
            NativeRamBlock *ram = OSDynamicCast(NativeRamBlock, entry);
            if (ram && !ram->isInactive() && ram->seedInfo(&base, &bytes, hash)) {
                candidate = makedev(static_cast<uint32_t>(majorNumber->unsigned64BitValue()),
                                    static_cast<uint32_t>(minorNumber->unsigned64BitValue()));
                snprintf(candidatePath, sizeof(candidatePath), "/dev/%s", name->getCStringNoCopy());
                selected = true;
                break;
            }
            const IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
            entry->release();
            entry = parent;
        }
        if (entry)
            entry->release();
    }
    iterator->release();
    if (!selected || matches != 1)
        return 0;
    *device = candidate;
    strlcpy(path, candidatePath, size);
    if (seedBase)
        *seedBase = base;
    if (seedBytes)
        *seedBytes = bytes;
    if (seedDigest)
        memcpy(seedDigest, hash, 32);
    return 1;
}

extern "C" int native_ram_block_volume_device(const char *uuid, dev_t *device, char *path, size_t size)
{
    return selectVolumeSeed(uuid, device, path, size, nullptr, nullptr, nullptr);
}

extern "C" int native_ram_block_volume_seed(const char *uuid, dev_t *device, char *path, size_t size, uint64_t *base,
                                            uint64_t *bytes, uint8_t digest[32])
{
    if (base)
        *base = 0;
    if (bytes)
        *bytes = 0;
    if (digest)
        memset(digest, 0, 32);
    if (!base || !bytes || !digest) {
        if (device)
            *device = NODEV;
        if (path && size)
            path[0] = 0;
        return 0;
    }
    return selectVolumeSeed(uuid, device, path, size, base, bytes, digest);
}
