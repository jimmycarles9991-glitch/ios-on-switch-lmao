// SPDX-License-Identifier: BSD-2-Clause-Patent
// The unchanged System/App cryptex images from Preboot as read-only block devices: read from the SD card through a
// cache of 128 KiB windows (the direct read map), or through the original DiskImages hook if that map is refused; the
// APFS volume each one publishes; and the boot prefetch of the cryptex volumes' files.
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>
#include <IOKit/storage/IOStorageProtocolCharacteristics.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <mach/memory_object_types.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/ubc.h>
#include <sys/vnode.h>
#include <vm/pmap.h>
#include "SwitchClientCopy.h"
#include "SwitchCryptex.h"
#include "SwitchCryptexCache.h"
#include "SwitchCryptexMap.h"
#include "SwitchCryptexStats.h"
#include "../../kits/SwitchTime/SwitchTime.h"

// NOLINTNEXTLINE(misc-use-internal-linkage): the layout bsd/vfs/vfs_cluster.c writes, named in its C interface
struct switch_record_entry {
    vnode_t vp;
    uint64_t offset;
    uint32_t length;
    uint32_t vid;
    uint16_t index;
    uint16_t when;
    uint32_t reserved;
};

extern "C" {
int di_root_image_ext(const char *, char *, size_t, dev_t *, bool);
void switch_cluster_register_image(vnode_t);                                           // bsd/vfs/vfs_cluster.c
void switch_cluster_register_cryptex_mount(unsigned int index, mount_t);               // bsd/vfs/vfs_cluster.c
void switch_cluster_record_start(struct switch_record_entry *log, uint32_t capacity);  // bsd/vfs/vfs_cluster.c
uint32_t switch_cluster_record_stop(void);                                             // bsd/vfs/vfs_cluster.c
#if DEVELOPMENT || DEBUG
void switch_cluster_image_direct(uint64_t bytes, uint32_t cardReads);  // bsd/vfs/vfs_cluster.c
#endif
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
int vnode_pagein(vnode_t, upl_t, upl_offset_t, vm_object_offset_t, upl_size_t, int, int *);  // bsd/vm/vnode_pager.c
int VFS_VGET(mount_t, ino64_t, vnode_t *, vfs_context_t);                                    // bsd/vfs/kpi_vfs.c
LIBKERN_RETURNS_NOT_RETAINED IOBlockStorageDevice *
switch_block_device(IOService **controller, dev_t *whole, uint64_t *sectors,
                    uint32_t *maxReadSectors);  // NativeSwitchBlock.cpp
void qsort(void *, size_t, size_t, int (*)(const void *, const void *));
processor_t cpu_to_processor(int);
void processor_cpu_load_info(processor_t, natural_t ticks[CPU_STATE_MAX]);
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
int ml_get_max_cpu_number(void);
}

namespace {
const char *const paths[SWITCH_CRYPTEX_COUNT] = {SWITCH_SYSTEM_CRYPTEX_IMAGE, SWITCH_APP_CRYPTEX_IMAGE};
const char *const uuids[SWITCH_CRYPTEX_COUNT] = {SWITCH_SYSTEM_CRYPTEX_UUID, SWITCH_APP_CRYPTEX_UUID};
// Fixed development identifier consumed by the owner boot-manifest code.
const uint8_t systemIdentifier[48] = {0x13, 0x14, 0x8b, 0xb4, 0x2c, 0x8c, 0xe4, 0xb5, 0x6d, 0x83, 0x5d, 0x89,
                                      0xf9, 0x1a, 0x4c, 0xea, 0x1a, 0xc0, 0x48, 0x21, 0x11, 0xdc, 0x34, 0xa0,
                                      0x0c, 0xfd, 0xdf, 0xee, 0x36, 0x03, 0x7c, 0xe5, 0x04, 0x92, 0x3b, 0xaa,
                                      0xe4, 0xba, 0xe2, 0xba, 0xe6, 0xda, 0x7d, 0x82, 0x29, 0xc5, 0x3e, 0x54};
int results[SWITCH_CRYPTEX_COUNT] = {ENXIO, ENXIO};
bool attempted[SWITCH_CRYPTEX_COUNT];
IOService *directDevice[SWITCH_CRYPTEX_COUNT];  // the image's direct read map, when it is the one attached

bool below(const IORegistryEntry *media, const IORegistryEntry *ancestor)
{
    bool found = false;
    const IORegistryEntry *entry = media->copyParentEntry(gIOServicePlane);
    for (unsigned depth = 0; entry && depth < 32 && !found; ++depth) {
        found = entry == ancestor;
        const IORegistryEntry *parent = entry->copyParentEntry(gIOServicePlane);
        entry->release();
        entry = parent;
    }
    OSSafeReleaseNULL(entry);
    return found;
}

// The IOMedia carrying this volume UUID once IOMediaBSDClient has named it (below `ancestor` if one is given).
int findVolume(const char *uuid, const IORegistryEntry *ancestor, dev_t *device, char *path, size_t capacity)
{
    OSDictionary *match = IOService::serviceMatching("IOMedia");
    OSDictionary *properties = OSDictionary::withCapacity(1);
    const OSString *wanted = OSString::withCString(uuid);
    if (!match || !properties || !wanted) {
        OSSafeReleaseNULL(match);
        OSSafeReleaseNULL(properties);
        OSSafeReleaseNULL(wanted);
        return ENOMEM;
    }
    properties->setObject("UUID", wanted);
    match->setObject(gIOPropertyMatchKey, properties);
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    properties->release();
    wanted->release();
    if (!it)
        return EAGAIN;
    int error = EAGAIN;
    while (const auto *media = OSDynamicCast(IOService, it->getNextObject())) {
        if (ancestor && !below(media, ancestor))
            continue;
        const auto *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
        const auto *majorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
        const auto *minorNumber = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
        if (!name || !majorNumber || !minorNumber)
            continue;
        *device = makedev(majorNumber->unsigned32BitValue(), minorNumber->unsigned32BitValue());
        snprintf(path, capacity, "/dev/%s", name->getCStringNoCopy());
        error = 0;
        break;
    }
    it->release();
    return error;
}

int outputs(unsigned index, dev_t *device, char *path, size_t capacity)
{
    if (device)
        *device = NODEV;
    if (path && capacity)
        path[0] = 0;
    return index < SWITCH_CRYPTEX_COUNT && device && path && capacity >= 64 ? 0 : EINVAL;
}
}  // namespace

// ---- r17-launch: the direct read map ----
//
// The cryptex images used to be attached with DiskImages: each read of the image device (the cryptex APFS's) became a
// read of the image file on Preboot, through its page cache (r8-sdboot cleared VNOCACHE_DATA so the boot prefetch had a
// cache to fill), and was then copied page by page into the requesting buffer (bcopy_phys on DiskImages' kernel threads;
// with the per-read VNOP_READ, UPL and copy work those four threads used ~29 s of CPU per r7v boot, ~1.5 s of it the
// copy). The image is one unchanging file, so its extents are resolved once here instead: the file's blocks on Preboot
// (VNOP_BLOCKMAP) and where they sit on the SD card's APFS window (DKIOCGETPHYSICALEXTENT, as kern_symfile.c's
// direct-I/O files), checked by reading sampled blocks both ways. The cryptex device's reads then go to the SD device,
// one read per extent piece: no DiskImages thread, no VNOP_READ.
//
// The device reports itself as SD storage, not a virtual (disk image) interface, so the cryptex APFS mounts without
// MNTK_VIRTUALDEV. APFS still reads the cryptex files one 4 KiB block at a time, each after the last, whatever the
// page-in asks for, and a card command for 4 KiB costs nearly as much as one for 128 KiB. So reads go through a cache
// of 128 KiB windows (SwitchCryptexCache.h): a small read that misses reads its whole window in one command, the
// reads that follow copy their blocks out of it (copypv, as the SD driver's bounce path copies into requests), and the
// boot prefetch reads its runs ahead into windows. A large read the cache does not hold goes straight into the
// request's pages. APFS checks every block it gets against the seal's hashes, wherever it came from. The cache is
// checked once at the first attach (proveCache) and turned off with one line if a block differs from the image file.
// Completions go to the client from a thread call, never from the SD driver's worker, so the hash checks run beside
// the card's next command, and a completion that ever waited for another read of the same image could not block the
// thread that issues it.
//
// The image file stays open (its use count) while the device exists. A refused map (a hole, a compressed file, an
// extent outside the window, a sampled block that differs) logs one line and attaches through DiskImages as before.
namespace {
constexpr uint32_t kImageBlock = 4096;  // the cryptex device's block (APFS's), and the images' size multiple
constexpr uint32_t kSdSector = 512;
constexpr uint32_t kMaxExtents = 1u << 16;
constexpr unsigned kSampledExtents = 16;  // checked both ways at attach, besides the image's first and last block
static_assert(kImageBlock == kSwitchCacheBlock, "the cache's block is the device's");
// The cache's windows: 32 MiB until the boot's reads are done (bootIoDone), then 8 MiB: app launches page the
// cryptexes in one block at a time too.
constexpr uint32_t kBootWindows = 256;
constexpr uint32_t kSteadyWindows = 64;
constexpr uint32_t kMinWindows = 8;
constexpr uint32_t kBuckets = 1024;
constexpr uint64_t kAroundBytes = 16ULL * kImageBlock;  // reads up to 64 KiB read their windows around them
constexpr uint32_t kRequestReads = kSwitchAroundWindows * kSwitchWindowReads;
constexpr unsigned kWorkers = 4;  // prefetch threads: APFS pages a run in one block at a time

using Extent = SwitchExtent;  // bytes: image offset, offset on the SD APFS window, length

// The client's completion runs on the request's thread call, and APFS hashes the sealed volume's blocks in it.
// AppleDiskImages2 completes an in-kernel image's requests (attachDiskImages') on its DIO threads, which it starts with
// kernel_thread_start: the kernel task's BASEPRI_KERNEL (81), THREAD_CALL_PRIORITY_KERNEL's band. Kernel-high (92) would
// put the hashing above every IOWorkLoop, the SD driver's worker that feeds it included.
struct Pending {
    Pending *next;
    thread_call_t call;
    IOStorageCompletion client;
    uint64_t length, bytes;
    uint32_t remaining;  // pieces outstanding (or one while it waits on the cache), plus one while it is being issued
    IOReturn status;
    bool prefetch;
    bool inflight;  // its length is counted in gFlow.inflight (a prefetch read that went straight to the card)
    bool cached;    // finishRequest copies it out of `cache`'s windows into `buffer`
    IOMemoryDescriptor *buffer;  // retained while cached
    uint64_t start;              // the request's first image byte
    SwitchCacheRequest cache;
    SwitchCacheRead reads[kRequestReads];
};

// A read into a cache window, in pieces of one extent and one SD command each.
struct Job {
    Job *next;
    SwitchWindow *window;
    uint32_t first, count;  // blocks of the window
    uint32_t remaining;     // pieces outstanding, plus one while it is being issued
    IOReturn status;
    uint64_t ahead;  // bytes counted in gFlow.inflight (a prefetch read-ahead), 0 for a request's read
};

IOLock *gPoolLock;
Pending *gPool;
Job *gJobs;
const uint8_t gZeros[kImageBlock] = {};

// The boot prefetch threads, their reads in flight on the direct devices (bytes: reads straight to the card and reads
// ahead into the cache), and the foreground's reads while they run or the boot's window is open.
struct {
    IOLock *lock;
    thread_t threads[kWorkers];
    uint64_t inflight;
    bool waiting;
    volatile bool window;
    uint64_t foregroundReads, foregroundBytes;
} gFlow;

// The cache (SwitchCryptexCache.h) is used under its lock; each window's memory is one IOBufferMemoryDescriptor.
struct {
    IOLock *lock;
    SwitchCache cache;
    SwitchWindow *windows;
    SwitchWindow **buckets;
    uint32_t count;
    bool setUp, on;
    uint64_t unreported;  // read-ahead card reads not yet in the kprof image line (debug kernels)
} gCache;

bool isPrefetchThread(thread_t thread)
{
    for (thread_t &slot : gFlow.threads) {
        if (__atomic_load_n(&slot, __ATOMIC_RELAXED) == thread)
            return true;
    }
    return false;
}

bool prefetchRunning()
{
    for (thread_t &slot : gFlow.threads) {
        if (__atomic_load_n(&slot, __ATOMIC_RELAXED) != THREAD_NULL)
            return true;
    }
    return false;
}

void flowDone(uint64_t bytes)
{
    IOLockLock(gFlow.lock);
    gFlow.inflight -= bytes;
    const bool wake = gFlow.waiting;
    gFlow.waiting = false;
    IOLockUnlock(gFlow.lock);
    if (wake)
        IOLockWakeup(gFlow.lock, &gFlow.inflight, false);
}

// The kprof image line's direct reads (one per request) and the card reads they took, with the read-ahead's.
void reportDirect(uint64_t bytes, uint32_t cardReads)
{
#if DEVELOPMENT || DEBUG
    const uint64_t ahead = __atomic_exchange_n(&gCache.unreported, 0, __ATOMIC_RELAXED);
    switch_cluster_image_direct(bytes, cardReads + static_cast<uint32_t>(ahead));
#else
    (void)bytes;
    (void)cardReads;
#endif
}

void finishRequest(thread_call_param_t, thread_call_param_t);

Pending *takePending()
{
    IOLockLock(gPoolLock);
    Pending *p = gPool;
    if (p)
        gPool = p->next;
    IOLockUnlock(gPoolLock);
    if (p)
        return p;
    p = static_cast<Pending *>(IOMallocZero(sizeof(Pending)));
    if (!p)
        return nullptr;
    p->call = thread_call_allocate_with_options(finishRequest, p, THREAD_CALL_PRIORITY_KERNEL, 0);
    if (!p->call) {
        IOFree(p, sizeof(Pending));
        return nullptr;
    }
    return p;
}

void returnPending(Pending *p)
{
    IOLockLock(gPoolLock);
    p->next = gPool;
    gPool = p;
    IOLockUnlock(gPoolLock);
}

Job *takeJob()
{
    IOLockLock(gPoolLock);
    Job *j = gJobs;
    if (j)
        gJobs = j->next;
    IOLockUnlock(gPoolLock);
    return j ? j : static_cast<Job *>(IOMallocZero(sizeof(Job)));
}

void returnJob(Job *j)
{
    IOLockLock(gPoolLock);
    j->next = gJobs;
    gJobs = j;
    IOLockUnlock(gPoolLock);
}

// The request's blocks, out of its windows into its memory (the windows are held: their blocks do not change).
bool copyOut(const Pending *p)
{
    const uint64_t first = p->start / kImageBlock;
    const uint64_t end = first + p->length / kImageBlock;
    for (uint32_t i = 0; i < p->cache.windows; ++i) {
        const SwitchWindow *w = p->cache.window[i];
        const uint64_t from = first > w->tag ? first : w->tag;
        const uint64_t to = end < w->tag + kSwitchWindowBlocks ? end : w->tag + kSwitchWindowBlocks;
        const uint8_t *bytes = static_cast<uint8_t *>(w->bytes) + (from - w->tag) * kImageBlock;
        if (from >= to || !switchClientCopy(p->buffer, bytes, static_cast<uint32_t>((to - from) * kImageBlock), true,
                                            copypv, static_cast<uint32_t>((from - first) * kImageBlock)))
            return false;
    }
    return true;
}

// The request's first error is its status.
void failPending(Pending *p, IOReturn status)
{
    IOReturn expected = kIOReturnSuccess;
    (void)__atomic_compare_exchange_n(&p->status, &expected, status, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

// A thread call: the request's last piece has completed (or, cached, its windows hold every block it asked for).
void finishRequest(thread_call_param_t parameter, thread_call_param_t)
{
    auto *p = static_cast<Pending *>(parameter);
#if DEVELOPMENT || DEBUG
    const uint64_t copiedBytes = p->cached ? p->length : 0;
    uint64_t copyAbs = 0;
#endif
    if (p->cached) {
#if DEVELOPMENT || DEBUG
        const uint64_t copying = mach_absolute_time();
#endif
        const bool copied = p->status == kIOReturnSuccess && !p->cache.failed && copyOut(p);
#if DEVELOPMENT || DEBUG
        copyAbs = mach_absolute_time() - copying;
#endif
        if (!copied)
            failPending(p, kIOReturnIOError);
        IOLockLock(gCache.lock);
        gCache.cache.release(&p->cache, copied);
        IOLockUnlock(gCache.lock);
        p->buffer->release();
        p->buffer = nullptr;
        p->cached = false;
    }
    const IOStorageCompletion client = p->client;
    const IOReturn status = p->status;
    const uint64_t bytes = status == kIOReturnSuccess ? p->length : p->bytes;
    const uint64_t counted = p->inflight ? p->length : 0;
    p->inflight = false;
    returnPending(p);
    if (counted)
        flowDone(counted);
#if DEVELOPMENT || DEBUG
    const uint64_t completing = mach_absolute_time();
#endif
    // IOStorage::complete, without binding another original IOStorageFamily symbol (as NativeSwitchBlock).
    if (client.action)
        client.action(client.target, client.parameter, status, bytes);
#if DEVELOPMENT || DEBUG
    switch_cryptex_stats::note(copiedBytes, copyAbs, mach_absolute_time() - completing);
#endif
}

// One piece's completion, on the SD driver's worker (or one more of the cache's: the request's windows are ready).
void pieceDone(void *, void *parameter, IOReturn status, UInt64 actualByteCount)
{
    auto *p = static_cast<Pending *>(parameter);
    if (status != kIOReturnSuccess)
        failPending(p, status);
    __atomic_fetch_add(&p->bytes, actualByteCount, __ATOMIC_RELAXED);
    if (__atomic_sub_fetch(&p->remaining, 1, __ATOMIC_ACQ_REL) == 0)
        thread_call_enter(p->call);
}

// On the SD driver's worker: the requests a finished read made ready complete from their own thread calls.
void jobFinish(Job *j)
{
    IOLockLock(gCache.lock);
    const SwitchCacheRequest *ready =
        gCache.cache.finished(j->window, j->first, j->count, j->status == kIOReturnSuccess);
    IOLockUnlock(gCache.lock);
    const uint64_t ahead = j->ahead;
    returnJob(j);
    if (ahead)
        flowDone(ahead);
    while (ready) {
        const SwitchCacheRequest *next = ready->nextReady;  // (the request may be gone once told)
        pieceDone(nullptr, ready->owner, kIOReturnSuccess, 0);
        ready = next;
    }
}

void jobFail(Job *j, IOReturn status)
{
    IOReturn expected = kIOReturnSuccess;
    (void)__atomic_compare_exchange_n(&j->status, &expected, status, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
}

// One piece of a cache read, on the SD driver's worker.
void jobPieceDone(void *, void *parameter, IOReturn status, UInt64)
{
    auto *j = static_cast<Job *>(parameter);
    if (status != kIOReturnSuccess)
        jobFail(j, status);
    if (__atomic_sub_fetch(&j->remaining, 1, __ATOMIC_ACQ_REL) == 0)
        jobFinish(j);
}

void putString(OSDictionary *dictionary, const char *key, const char *value)
{
    if (const OSString *string = OSString::withCStringNoCopy(value)) {
        dictionary->setObject(key, string);
        string->release();
    }
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs external linkage (kalloc type view)
class SwitchCryptexDevice : public IOBlockStorageDevice {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(SwitchCryptexDevice)

    public : bool configure(unsigned image, Extent *map, uint32_t count, uint64_t bytes, IOBlockStorageDevice *sd,
                            uint64_t sdSectors, uint32_t sdMaxSectors);

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
        static char app[] = "App cryptex";
        static char base[] = "System cryptex";
        return index ? app : base;
    }

    char *getRevisionString() override
    {
        static char revision[] = "1.0";
        return revision;
    }

    char *getAdditionalDeviceInfoString() override
    {
        static char info[] = "cryptex image extents on SDMMC1";
        return info;
    }

    IOReturn reportBlockSize(UInt64 *value) override
    {
        *value = kImageBlock;
        return kIOReturnSuccess;
    }

    IOReturn reportMaxValidBlock(UInt64 *value) override
    {
        *value = blocks - 1;
        return kIOReturnSuccess;
    }

    IOReturn reportEjectability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }

    IOReturn reportRemovability(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }

    IOReturn reportWriteProtection(bool *value) override
    {
        *value = true;
        return kIOReturnSuccess;
    }

    IOReturn reportMediaState(bool *present, bool *changed) override
    {
        *present = true;
        if (changed)
            *changed = false;
        return kIOReturnSuccess;
    }

    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override
    {
        return kIOReturnSuccess;
    }

    IOReturn doSynchronizeCache() override
    {
        return kIOReturnSuccess;
    }

    IOReturn getWriteCacheState(bool *value) override
    {
        *value = false;
        return kIOReturnSuccess;
    }

    IOReturn setWriteCacheState(bool) override
    {
        return kIOReturnUnsupported;
    }

    IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64, IOStorageAttributes *,
                              IOStorageCompletion *) override;
    // Image bytes [start, start + length) read ahead into the cache; one block read through the device and waited for.
    void readAhead(uint64_t start, uint64_t length);
    IOReturn readBlock(IOMemoryDescriptor *buffer, uint64_t block);

    uint64_t imageBytes() const
    {
        return blocks * kImageBlock;
    }

private:
    unsigned index = 0;
    Extent *extents = nullptr;
    uint32_t extentCount = 0;
    uint64_t blocks = 0;
    IOBlockStorageDevice *sd = nullptr;
    uint64_t maxPiece = 0;  // bytes per SD read
    bool holes = false;     // some of the image reads as zeros (a sparse file)
    bool cacheRead(Pending *p, IOMemoryDescriptor *buffer, uint64_t block, uint64_t count);
    uint32_t issueRead(const SwitchCacheRead &read, bool ahead);
};

OSDefineMetaClassAndStructors(SwitchCryptexDevice, IOBlockStorageDevice)

    bool SwitchCryptexDevice::configure(unsigned image, Extent *map, uint32_t count, uint64_t bytes,
                                        IOBlockStorageDevice *sdDevice, uint64_t, uint32_t sdMaxSectors)
{
    if (!map)
        return false;
    index = image;
    extents = map;
    extentCount = count;
    blocks = bytes / kImageBlock;
    sd = sdDevice;
    maxPiece = static_cast<uint64_t>(sdMaxSectors) * kSdSector;
    for (uint32_t i = 0; i < count; ++i)
        holes |= map[i].disk == kSwitchHole;
    // A request larger than one SD read is split here anyway; IOBlockStorageDriver keeps them at 1 MiB.
    setProperty(kIOMaximumBlockCountReadKey, (1024 * 1024) / kImageBlock, 64);
    setProperty(kIOMaximumByteCountReadKey, 1024LL * 1024, 64);
    if (OSDictionary *protocol = OSDictionary::withCapacity(2)) {
        putString(protocol, kIOPropertyPhysicalInterconnectTypeKey, "SD");
        putString(protocol, kIOPropertyPhysicalInterconnectLocationKey, kIOPropertyInternalKey);
        setProperty(kIOPropertyProtocolCharacteristicsKey, protocol);
        protocol->release();
    }
    if (OSDictionary *device = OSDictionary::withCapacity(1)) {
        putString(device, kIOPropertyMediumTypeKey, kIOPropertyMediumTypeSolidStateKey);
        setProperty(kIOPropertyDeviceCharacteristicsKey, device);
        device->release();
    }
    setProperty("SwitchImage", paths[image]);
    setProperty("SwitchImage extents", count, 32);
    setName(image ? "SwitchAppCryptex" : "SwitchSystemCryptex");
    return maxPiece >= kImageBlock;
}

IOReturn SwitchCryptexDevice::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                                               IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !count || block >= blocks || count > blocks - block)
        return kIOReturnBadArgument;
    if (buffer->getDirection() != kIODirectionIn)
        return kIOReturnNotWritable;
    const uint64_t length = count * kImageBlock;
    const uint64_t start = block * kImageBlock;
    if (buffer->getLength() < length)
        return kIOReturnBadArgument;
    Pending *p = takePending();
    if (!p)
        return kIOReturnNoMemory;
    p->client = *completion;
    p->length = length;
    p->bytes = 0;
    p->status = kIOReturnSuccess;
    p->remaining = 1;
    p->inflight = p->cached = false;
    p->prefetch = isPrefetchThread(current_thread());
    if (!p->prefetch && (prefetchRunning() || gFlow.window)) {
        __atomic_fetch_add(&gFlow.foregroundReads, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&gFlow.foregroundBytes, length, __ATOMIC_RELAXED);
    }
    // The boot prefetch's card reads take the SD driver's high class, first come first served with the foreground's:
    // in a lower class they wait out its 100 ms aging behind a saturated foreground.
    IOStorageAttributes high = {};
    high.priority = kIOStoragePriorityHigh;
    IOStorageAttributes *issue = p->prefetch ? &high : attributes;
    if (cacheRead(p, buffer, block, count))
        return kIOReturnSuccess;
    if (p->prefetch) {
        p->inflight = true;
        IOLockLock(gFlow.lock);
        gFlow.inflight += length;
        IOLockUnlock(gFlow.lock);
    }
    uint32_t pieces = 0;
    uint32_t at = switchExtentAt(extents, extentCount, start);
    uint64_t done = 0;
    if (holes) {
        // A range the image file does not store reads as zeros: written before any card read is issued into the same
        // request, so the SD driver's cache maintenance around its reads never meets a dirty line of ours.
        for (uint32_t z = at; done < length;) {
            const SwitchPiece piece = switchNextPiece(extents, extentCount, &z, start + done, length - done, maxPiece);
            if (!piece.length)
                break;
            for (uint64_t k = 0; piece.disk == kSwitchHole && k < piece.length; k += kImageBlock) {
                if (buffer->writeBytes(done + k, gZeros, kImageBlock) != kImageBlock)
                    failPending(p, kIOReturnIOError);
            }
            done += piece.length;
        }
        done = 0;
    }
    while (done < length) {
        const SwitchPiece piece = switchNextPiece(extents, extentCount, &at, start + done, length - done, maxPiece);
        if (!piece.length)
            break;
        if (piece.disk == kSwitchHole) {
            done += piece.length;
            continue;
        }
        IOSubMemoryDescriptor *sub = IOSubMemoryDescriptor::withSubRange(buffer, done, piece.length, kIODirectionIn);
        if (!sub)
            break;
        __atomic_fetch_add(&p->remaining, 1, __ATOMIC_RELAXED);
        IOStorageCompletion pieceCompletion = {this, pieceDone, p};
        const IOReturn issued =
            sd->doAsyncReadWrite(sub, piece.disk / kSdSector, piece.length / kSdSector, issue, &pieceCompletion);
        sub->release();
        if (issued != kIOReturnSuccess)
            pieceDone(this, p, issued, 0);
        ++pieces;
        done += piece.length;
    }
    if (done < length)
        failPending(p, kIOReturnNoMemory);  // the rest was never issued
    reportDirect(length, pieces);
    pieceDone(this, p, kIOReturnSuccess, 0);  // the issuing reference
    return kIOReturnSuccess;
}

// True when the cache took the request (finishRequest completes it once its windows hold every block).
bool SwitchCryptexDevice::cacheRead(Pending *p, IOMemoryDescriptor *buffer, uint64_t block, uint64_t count)
{
    if (!__atomic_load_n(&gCache.on, __ATOMIC_ACQUIRE))
        return false;
    const auto mode = count * kImageBlock > kAroundBytes ? SwitchCache::kCoveredOnly : SwitchCache::kReadAround;
    // Set before the plan: a read already under way may make the request ready as soon as the lock is dropped.
    p->cached = true;
    p->buffer = buffer;
    p->start = block * kImageBlock;
    p->cache.owner = p;
    uint32_t n = 0;
    IOLockLock(gCache.lock);
    const bool taken = gCache.cache.plan(&p->cache, static_cast<uint8_t>(index), block, static_cast<uint32_t>(count),
                                         blocks, mode, p->reads, kRequestReads, &n);
    if (taken)
        p->remaining = p->cache.waiting ? 2 : 1;  // the issuing reference, and the wait for the windows
    IOLockUnlock(gCache.lock);
    if (!taken) {
        p->cached = false;
        p->buffer = nullptr;
        return false;
    }
    buffer->retain();
    uint32_t cardReads = 0;
    for (uint32_t i = 0; i < n; ++i)
        cardReads += issueRead(p->reads[i], false);
    reportDirect(p->length, cardReads);
    pieceDone(this, p, kIOReturnSuccess, 0);  // the issuing reference
    return true;
}

// Returns the card reads it issued. The window's blocks are marked being read: nothing else touches them until
// jobFinish. Every cache read goes in the SD driver's high class, whichever request started it: a foreground read may
// be waiting on it, and in a lower class it would wait out the SD driver's 100 ms aging behind a busy foreground.
uint32_t SwitchCryptexDevice::issueRead(const SwitchCacheRead &read, bool ahead)
{
    IOStorageAttributes high = {};
    high.priority = kIOStoragePriorityHigh;
    Job *j = takeJob();
    if (!j) {  // the read fails at once: its blocks are read again by the next request that wants them
        IOLockLock(gCache.lock);
        const SwitchCacheRequest *ready = gCache.cache.finished(read.window, read.first, read.count, false);
        IOLockUnlock(gCache.lock);
        for (const SwitchCacheRequest *next = nullptr; ready; ready = next) {
            next = ready->nextReady;
            pieceDone(nullptr, ready->owner, kIOReturnSuccess, 0);
        }
        return 0;
    }
    const uint64_t start = (read.window->tag + read.first) * kImageBlock;
    const uint64_t length = static_cast<uint64_t>(read.count) * kImageBlock;
    *j = {nullptr, read.window, read.first, read.count, 1, kIOReturnSuccess, ahead ? length : 0};
    if (j->ahead) {
        IOLockLock(gFlow.lock);
        gFlow.inflight += j->ahead;
        IOLockUnlock(gFlow.lock);
    }
    auto *memory = static_cast<IOBufferMemoryDescriptor *>(read.window->memory);
    uint8_t *bytes = static_cast<uint8_t *>(read.window->bytes) + static_cast<uint64_t>(read.first) * kImageBlock;
    uint32_t pieces = 0;
    uint32_t at = switchExtentAt(extents, extentCount, start);
    uint64_t done = 0;
    while (done < length) {
        const SwitchPiece piece = switchNextPiece(extents, extentCount, &at, start + done, length - done, maxPiece);
        if (!piece.length)
            break;
        if (piece.disk == kSwitchHole) {
            bzero(bytes + done, piece.length);
            done += piece.length;
            continue;
        }
        IOSubMemoryDescriptor *sub = IOSubMemoryDescriptor::withSubRange(
            memory, static_cast<uint64_t>(read.first) * kImageBlock + done, piece.length, kIODirectionIn);
        if (!sub)
            break;
        __atomic_fetch_add(&j->remaining, 1, __ATOMIC_RELAXED);
        IOStorageCompletion pieceCompletion = {this, jobPieceDone, j};
        const IOReturn issued =
            sd->doAsyncReadWrite(sub, piece.disk / kSdSector, piece.length / kSdSector, &high, &pieceCompletion);
        sub->release();
        if (issued != kIOReturnSuccess)
            jobPieceDone(this, j, issued, 0);
        ++pieces;
        done += piece.length;
    }
    if (done < length)
        jobFail(j, kIOReturnNoMemory);           // the rest was never issued
    jobPieceDone(this, j, kIOReturnSuccess, 0);  // the issuing reference
    return pieces;
}

void SwitchCryptexDevice::readAhead(uint64_t start, uint64_t length)
{
    if (!__atomic_load_n(&gCache.on, __ATOMIC_ACQUIRE) || !length)
        return;
    const uint64_t end = (start + length + kImageBlock - 1) / kImageBlock;
    uint32_t cardReads = 0;
    for (uint64_t at = start / kImageBlock; at < end;) {
        SwitchCacheRead reads[kSwitchWindowReads];
        uint32_t n = 0;
        IOLockLock(gCache.lock);
        const uint32_t covered = gCache.cache.readAhead(static_cast<uint8_t>(index), at, end - at, blocks, reads, &n);
        IOLockUnlock(gCache.lock);
        if (!covered)
            break;  // every window in use: the page-in reads the rest itself
        for (uint32_t i = 0; i < n; ++i)
            cardReads += issueRead(reads[i], true);
        at += covered;
    }
#if DEVELOPMENT || DEBUG
    __atomic_fetch_add(&gCache.unreported, cardReads, __ATOMIC_RELAXED);
#else
    (void)cardReads;
#endif
}

namespace {
// One synchronous read for the attach-time check: `bytes` at `sector` of the SD window into `buffer`.
struct SyncRead {
    IOLock *lock;
    bool done;
    IOReturn status;
};

void syncReadDone(void *, void *parameter, IOReturn status, UInt64)
{
    auto *wait = static_cast<SyncRead *>(parameter);
    IOLockLock(wait->lock);
    wait->status = status;
    wait->done = true;
    IOLockUnlock(wait->lock);
    IOLockWakeup(wait->lock, wait, false);
}

IOReturn readSd(IOBlockStorageDevice *sd, uint64_t sector, IOBufferMemoryDescriptor *buffer)
{
    SyncRead wait = {IOLockAlloc(), false, kIOReturnSuccess};
    if (!wait.lock)
        return kIOReturnNoMemory;
    IOStorageCompletion completion = {nullptr, syncReadDone, &wait};
    IOReturn status = sd->doAsyncReadWrite(buffer, sector, buffer->getLength() / kSdSector, nullptr, &completion);
    if (status == kIOReturnSuccess) {
        IOLockLock(wait.lock);
        while (!wait.done)
            IOLockSleep(wait.lock, &wait, THREAD_UNINT);
        IOLockUnlock(wait.lock);
        status = wait.status;
    }
    IOLockFree(wait.lock);
    return status;
}
}  // namespace

// Its reads complete from the SD driver's interrupts; one that never does is a lost completion: a one-line panic, not
// a hang.
IOReturn SwitchCryptexDevice::readBlock(IOMemoryDescriptor *buffer, uint64_t block)
{
    SyncRead wait = {IOLockAlloc(), false, kIOReturnSuccess};
    if (!wait.lock)
        return kIOReturnNoMemory;
    IOStorageCompletion completion = {nullptr, syncReadDone, &wait};
    IOReturn status = doAsyncReadWrite(buffer, block, 1, nullptr, &completion);
    if (status == kIOReturnSuccess) {
        uint64_t deadline = 0;
        clock_interval_to_deadline(10, NSEC_PER_SEC, &deadline);
        IOLockLock(wait.lock);
        while (!wait.done) {
            if (IOLockSleepDeadline(wait.lock, &wait, deadline, THREAD_UNINT) == THREAD_TIMED_OUT && !wait.done)
                panic("SwitchCryptex: a read of block %llu of %s never completed (10 s)", block, paths[index]);
        }
        IOLockUnlock(wait.lock);
        status = wait.status;
    }
    IOLockFree(wait.lock);
    return status;
}

namespace {
// Once, at the first direct attach: kBootWindows, or as many as there is memory for (below kMinWindows none, and reads
// go straight to the card). Allocated here, never in the read path, which page-outs may be waiting on.
bool setupCache()
{
    if (gCache.setUp)
        return gCache.count >= kMinWindows;
    gCache.setUp = true;
    gCache.lock = IOLockAlloc();
    gCache.windows = static_cast<SwitchWindow *>(IOMallocZero(kBootWindows * sizeof(SwitchWindow)));
    gCache.buckets = static_cast<SwitchWindow **>(IOMallocZero(kBuckets * sizeof(SwitchWindow *)));
    if (!gCache.lock || !gCache.windows || !gCache.buckets)
        return false;
    uint32_t n = 0;
    for (; n < kBootWindows; ++n) {
        IOBufferMemoryDescriptor *memory =
            IOBufferMemoryDescriptor::withOptions(kIODirectionIn, kSwitchWindowBytes, kImageBlock);
        if (!memory)
            break;
        if (memory->prepare() != kIOReturnSuccess) {
            memory->release();
            break;
        }
        gCache.windows[n].memory = memory;
        gCache.windows[n].bytes = memory->getBytesNoCopy();
    }
    gCache.count = n;
    if (n < kMinWindows) {
        for (uint32_t i = 0; i < n; ++i) {
            auto *memory = static_cast<IOBufferMemoryDescriptor *>(gCache.windows[i].memory);
            memory->complete();
            memory->release();
        }
        gCache.count = 0;
        return false;
    }
    gCache.cache.init(gCache.windows, n, gCache.buckets, kBuckets);
    return true;
}

// Blocks read through the cache must be the image file's own (vn_rdwr): a cold miss (its window read around it), a hit
// in that window, the image's last block, and the first block of extents spread over the image. nullptr, or what
// failed.
const char *proveCache(SwitchCryptexDevice *device, vnode_t image, const Extent *map, uint32_t count, uint64_t bytes)
{
    IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withOptions(kIODirectionIn, kImageBlock, kImageBlock);
    auto *file = static_cast<uint8_t *>(IOMallocData(kImageBlock));
    const bool prepared = (buffer != nullptr) && buffer->prepare() == kIOReturnSuccess;
    const char *why = prepared && file ? nullptr : "no memory";
    uint64_t offsets[kSampledExtents + 3];
    unsigned n = 0;
    offsets[n++] = 0;
    if (bytes > kImageBlock)
        offsets[n++] = kImageBlock;
    offsets[n++] = bytes - kImageBlock;
    for (unsigned k = 1; k <= kSampledExtents && count > 2; ++k)
        offsets[n++] = map[static_cast<uint64_t>(count - 1) * k / (kSampledExtents + 1)].file;
    vfs_context_t ctx = vfs_context_current();
    for (unsigned i = 0; i < n && !why; ++i) {
        int resid = 0;
        if (vn_rdwr(UIO_READ, image, reinterpret_cast<caddr_t>(file), kImageBlock, static_cast<off_t>(offsets[i]),
                    UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) ||
            resid) {
            why = "the image file could not be read";
        } else if (device->readBlock(buffer, offsets[i] / kImageBlock) != kIOReturnSuccess) {
            why = "a read through it failed";
        } else if (memcmp(buffer->getBytesNoCopy(), file, kImageBlock)) {
            why = "a block read through it differs from the image file";
        }
    }
    if (prepared)
        buffer->complete();
    OSSafeReleaseNULL(buffer);
    if (file)
        IOFreeData(file, kImageBlock);
    return why;
}

// Whether block `offset` of the image reads the same through the file and through the map.
bool sameBothWays(vnode_t image, const Extent &e, uint64_t offset, IOBlockStorageDevice *sd,
                  IOBufferMemoryDescriptor *direct, uint8_t *file)
{
    vfs_context_t ctx = vfs_context_current();
    int resid = 0;
    if (vn_rdwr(UIO_READ, image, reinterpret_cast<caddr_t>(file), kImageBlock, static_cast<off_t>(offset), UIO_SYSSPACE,
                IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) ||
        resid)
        return false;
    if (e.disk == kSwitchHole)
        return memcmp(gZeros, file, kImageBlock) == 0;
    if (readSd(sd, (e.disk + (offset - e.file)) / kSdSector, direct) != kIOReturnSuccess)
        return false;
    return memcmp(direct->getBytesNoCopy(), file, kImageBlock) == 0;
}

// The image's extents on the SD APFS window, or nullptr with *why set. `image` is open; `preboot` its volume's device.
Extent *resolveExtents(vnode_t image, uint64_t bytes, dev_t preboot, dev_t whole, uint64_t windowBytes, uint32_t *count,
                       const char **why)
{
    vfs_context_t ctx = vfs_context_current();
    vnode_t devvp = vfs_devvp(vnode_mount(image));
    if (!devvp)
        return *why = "no Preboot device vnode", nullptr;
    Extent *map = nullptr;
    uint32_t n = 0;
    uint32_t blockSize = 0;
    bool physical = true;
    bool asked = false;
    bool locked = false;
    if (vnode_specrdev(devvp) != preboot) {
        *why = "image not on the Preboot volume";
    } else if (VNOP_IOCTL(devvp, DKIOCGETBLOCKSIZE, reinterpret_cast<caddr_t>(&blockSize), 0, ctx) || !blockSize) {
        *why = "no Preboot block size";
    } else {
        map = static_cast<Extent *>(IOMallocData(kMaxExtents * sizeof(Extent)));
        if (!map)
            *why = "no memory";
    }
    if (map) {
        locked = (VNOP_IOCTL(devvp, DKIOCLOCKPHYSICALEXTENTS, nullptr, 0, ctx) == 0);
        for (uint64_t at = 0; at < bytes && !*why;) {
            daddr64_t blkno = -1;
            size_t run = 0;
            const size_t ask = bytes - at > (1ULL << 30) ? (1ULL << 30) : static_cast<size_t>(bytes - at);
            if (VNOP_BLOCKMAP(image, static_cast<off_t>(at), ask, &blkno, &run, nullptr,
                              VNODE_READ | VNODE_BLOCKMAP_NO_TRACK, nullptr) ||
                !run) {
                *why = "VNOP_BLOCKMAP failed";
                break;
            }
            if (run > bytes - at)
                run = static_cast<size_t>(bytes - at);
            if (blkno < 0) {  // a hole: zeros, nothing to read
                if (!switchAppendExtent(map, &n, kMaxExtents, at, kSwitchHole, run))
                    *why = "too many extents";
                at += run;
                continue;
            }
            const uint64_t logical = static_cast<uint64_t>(blkno) * blockSize;
            for (uint64_t part = 0; part < run && !*why;) {
                uint64_t disk = logical + part;
                uint64_t length = run - part;
                if (physical) {
                    dk_physical_extent_t request = {};
                    request.offset = disk;
                    request.length = length;
                    const int error =
                        VNOP_IOCTL(devvp, DKIOCGETPHYSICALEXTENT, reinterpret_cast<caddr_t>(&request), 0, ctx);
                    const bool first = !asked;
                    asked = true;
                    if (error == ENOTTY && first) {
                        // No physical-extent answer: the container is the whole window (one physical store at offset 0),
                        // so its block addresses are the window's; the sampled reads below check that.
                        physical = false;
                    } else if (error || !request.length || request.dev != whole) {
                        *why = error ? "DKIOCGETPHYSICALEXTENT failed" : "an extent is not on the SD APFS window";
                        break;
                    } else {
                        disk = request.offset;
                        if (request.length < length)
                            length = request.length;
                    }
                }
                if (disk % kSdSector || length % kSdSector || disk > windowBytes || length > windowBytes - disk ||
                    length % kImageBlock || (at + part) % kImageBlock) {
                    *why = "an extent outside the SD APFS window";
                    break;
                }
                if (!switchAppendExtent(map, &n, kMaxExtents, at + part, disk, length)) {
                    *why = "too many extents";
                    break;
                }
                part += length;
            }
            at += run;
        }
        if (locked)
            (void)VNOP_IOCTL(devvp, DKIOCUNLOCKPHYSICALEXTENTS, nullptr, 0, ctx);
    }
    vnode_put(devvp);
    if (!*why && !switchExtentsCover(map, n, bytes))
        *why = "extents do not cover the image";
    if (*why) {
        if (map)
            IOFreeData(map, kMaxExtents * sizeof(Extent));
        return nullptr;
    }
    *count = n;
    return map;
}

// The direct read map for image `index`: its device registered on the SD controller, or *why.
IOService *attachDirect(unsigned index, dev_t preboot, const char **why)
{
    vfs_context_t ctx = vfs_context_current();
    IOService *controller = nullptr;
    dev_t whole = NODEV;
    uint64_t windowSectors = 0;
    uint32_t maxSectors = 0;
    IOBlockStorageDevice *sd = switch_block_device(&controller, &whole, &windowSectors, &maxSectors);
    if (!sd || whole == NODEV || maxSectors * kSdSector < kImageBlock)
        return *why = "no SD device", nullptr;
    vnode_t image = NULLVP;
    if (vnode_open(paths[index], FREAD | O_NOFOLLOW, 0, 0, &image, ctx))
        return *why = "image open failed", nullptr;
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    VATTR_WANTED(&va, va_flags);
    uint32_t count = 0;
    Extent *map = nullptr;
    IOBufferMemoryDescriptor *direct = nullptr;
    uint8_t *file = nullptr;
    SwitchCryptexDevice *device = nullptr;
    if (!vnode_isreg(image) || vnode_getattr(image, &va, ctx) || !VATTR_IS_SUPPORTED(&va, va_data_size)) {
        *why = "image attributes";
    } else if (VATTR_IS_SUPPORTED(&va, va_flags) && (va.va_flags & UF_COMPRESSED)) {
        *why = "the image file is compressed";
    } else if (!va.va_data_size || va.va_data_size % kImageBlock) {
        *why = "image size not a multiple of 4 KiB";
    } else {
        map = resolveExtents(image, va.va_data_size, preboot, whole, windowSectors * kSdSector, &count, why);
    }
    if (map) {
        direct = IOBufferMemoryDescriptor::withOptions(kIODirectionIn, kImageBlock, kImageBlock);
        file = static_cast<uint8_t *>(IOMallocData(kImageBlock));
        if (!direct || !file || direct->prepare() != kIOReturnSuccess) {
            *why = "no memory";
        } else {
            // The first and last block, and the first block of extents spread over the image.
            const Extent &last = map[count - 1];  // (a hole is compared with zeros)
            bool same = sameBothWays(image, map[0], 0, sd, direct, file) &&
                        sameBothWays(image, last, va.va_data_size - kImageBlock, sd, direct, file);
            for (unsigned k = 1; same && k <= kSampledExtents && count > 2; ++k) {
                const Extent &e = map[static_cast<uint64_t>(count - 1) * k / (kSampledExtents + 1)];
                same = sameBothWays(image, e, e.file, sd, direct, file);
            }
            direct->complete();
            if (!same)
                *why = "a sampled block differs between the file and the map";
        }
    }
    OSSafeReleaseNULL(direct);
    if (file)
        IOFreeData(file, kImageBlock);
    if (!*why) {
        device = OSTypeAlloc(SwitchCryptexDevice);
        if (!device || !device->init(static_cast<OSDictionary *>(nullptr)) ||
            !device->configure(index, map, count, va.va_data_size, sd, windowSectors, maxSectors) ||
            !device->attach(controller)) {
            *why = "device setup";
        } else if (!device->start(controller)) {
            device->detach(controller);
            *why = "device start";
        }
    }
    if (*why) {
        OSSafeReleaseNULL(device);
        if (map)
            IOFreeData(map, kMaxExtents * sizeof(Extent));
        vnode_close(image, FREAD, ctx);
        return nullptr;
    }
    // Checked through this device before it is registered, so nothing else reads through the cache yet.
    const char *cacheOff = nullptr;
    if (!gCache.setUp) {
        if (!setupCache()) {
            cacheOff = "no memory for its windows";
        } else {
            __atomic_store_n(&gCache.on, true, __ATOMIC_RELEASE);
            cacheOff = proveCache(device, image, map, count, va.va_data_size);
            if (cacheOff)
                __atomic_store_n(&gCache.on, false, __ATOMIC_RELEASE);
        }
        if (cacheOff)
            IOLog("SwitchCryptex: read cache off (%s); every read goes to the card\n", cacheOff);
    }
    if (gCache.on) {
        IOLog("SwitchCryptex: %s read from the card through the read cache (%u windows of %u KiB, checked): %u "
              "extents, %llu MiB\n",
              paths[index], gCache.count, kSwitchWindowBytes >> 10, count, va.va_data_size >> 20);
    } else {
        IOLog("SwitchCryptex: %s read straight from the card: %u extents, %llu MiB\n", paths[index], count,
              va.va_data_size >> 20);
    }
    vnode_put(image);  // the open's use count keeps the file (and its extents) for the device's life
    device->registerService();
    return device;
}

// The original DiskImages hook (the map refused), as before r17-launch: the image's pages cached in Preboot's page
// cache (DiskImages opens it VNOCACHE_DATA; the image is read-only, so the flag is cleared and the cluster layer told).
int attachDiskImages(unsigned index)
{
    char bsd[64] = {};
    dev_t attached = NODEV;
    const int status = di_root_image_ext(paths[index], bsd, sizeof(bsd), &attached, false);
    if (status) {
        IOLog("SwitchCryptex: attach %s failed: %d\n", paths[index], status);
        return EIO;
    }
    vnode_t image = NULLVP;
    if (const int lookup = vnode_lookup(paths[index], 0, &image, vfs_context_current())) {
        IOLog("SwitchCryptex: %s stays uncached: lookup %d\n", paths[index], lookup);
    } else {
        vnode_clearnocache(image);
        switch_cluster_register_image(image);
        vnode_put(image);
    }
    return 0;
}

// The image's APFS container publishes its volume asynchronously.
int waitVolume(unsigned index, dev_t *device, char *path, size_t capacity)
{
    int error = EAGAIN;
    for (unsigned attempt = 0; attempt < 200; ++attempt) {
        error = findVolume(uuids[index], directDevice[index], device, path, capacity);
        if (error != EAGAIN)
            break;
        IOSleep(50);
    }
    if (error)
        IOLog("SwitchCryptex: %s attached but volume %s not found: %d\n", paths[index], uuids[index], error);
    return error;
}
}  // namespace

extern "C" int switch_cryptex_attach(unsigned index, dev_t preboot, dev_t *device, char *path, size_t capacity)
{
    int error = outputs(index, device, path, capacity);
    if (error)
        return error;
    if (attempted[index])
        return results[index] ? results[index] : findVolume(uuids[index], directDevice[index], device, path, capacity);
    attempted[index] = true;
    if (!gPoolLock)
        gPoolLock = IOLockAlloc();
    if (!gFlow.lock)
        gFlow.lock = IOLockAlloc();
    const char *why = gPoolLock && gFlow.lock ? nullptr : "no memory";
    if (!why)
        directDevice[index] = attachDirect(index, preboot, &why);
    if (!directDevice[index]) {
        IOLog("SwitchCryptex: %s direct read map refused (%s); attaching it through DiskImages\n", paths[index], why);
        error = attachDiskImages(index);
        if (error)
            return results[index] = error;
    }
    return results[index] = waitVolume(index, device, path, capacity);
}

// The mount of a direct-attached image's volume failed: detach the map and attach the image through DiskImages.
extern "C" int switch_cryptex_fallback(unsigned index, dev_t *device, char *path, size_t capacity)
{
    int error = outputs(index, device, path, capacity);
    if (error)
        return error;
    IOService *direct = directDevice[index];
    if (!direct)
        return EINVAL;  // already DiskImages: nothing else to try
    IOLog("SwitchCryptex: %s on its direct read map did not mount; attaching it through DiskImages\n", paths[index]);
    directDevice[index] = nullptr;
    direct->terminate(kIOServiceRequired | kIOServiceSynchronous);
    // The map's volume is gone once its container has terminated (the image file stays open: the map is never freed).
    for (unsigned attempt = 0; attempt < 100 && !findVolume(uuids[index], direct, device, path, capacity); ++attempt)
        IOSleep(50);
    error = attachDiskImages(index);
    if (error)
        return results[index] = error;
    return results[index] = waitVolume(index, device, path, capacity);
}

extern "C" int switch_cryptex_volume_status(unsigned index, dev_t *device, char *path, size_t capacity)
{
    const int error = outputs(index, device, path, capacity);
    if (error)
        return error;
    return results[index] ? results[index] : findVolume(uuids[index], directDevice[index], device, path, capacity);
}

extern "C" int switch_cryptex_manifest_sha384(dev_t device, const char *uuid, uint8_t digest[48])
{
    if (!digest)
        return 0;
    bzero(digest, 48);
    dev_t actual = NODEV;
    char path[64];
    if (device == NODEV || !uuid || strcmp(uuid, uuids[0]) || results[0] ||
        findVolume(uuids[0], directDevice[0], &actual, path, sizeof(path)) || actual != device)
        return 0;
    memcpy(digest, systemIdentifier, sizeof(systemIdentifier));
    return 1;
}

// ---- r8-sdboot, r17-launch: boot prefetch of the cryptex volumes' files ----
//
// Until the UI is up the SD card is the boot's bottleneck, and most of what it reads is the dyld shared cache on the
// System cryptex, paged in where processes first touch it. The same bytes read as large runs take a fraction of that
// card time. So:
//  - record: on a boot with no valid list, every read of a cryptex volume's file that goes to the device (vfs_cluster.c
//    cluster_io: page-in clusters, read misses, read-ahead) is logged with its first-read second, from the mounts until
//    kRecordAfterBootS after System Boot Complete (SpringBoard finished startup: SwitchPower's boot-done hint), and this
//    file saves the files' ranges (file id, pages) as runs of up to kMaxRun pages (through gaps of up to kMaxGap
//    pages), in first-read order, at most kListCapPages, next to the System image (kListSuffix; the header keys both
//    images' id, size and mtime). A boot that has not reached SpringBoard by kRecordBackstopS after the mounts saves
//    nothing: a crash loop's reads (ReportCrash symbolicating the shared cache) would be replayed on every boot after;
//  - replay: on every later boot, right after both cryptexes are mounted, kWorkers kernel threads page the runs in, in
//    order (vnode_pagein on the file: absent pages only, into the file's own pages, read and checked like any page-in).
//    APFS reads a page-in's blocks one at a time, so each thread first reads its run's absent pages ahead into the
//    cache as image ranges (VNOP_BLOCKMAP, then the volume's DKIOCGETPHYSICALEXTENT as resolveExtents does; the map is
//    checked once per image on a real page: mapProven), and the page-in then copies its blocks out of the cache. At
//    most kInFlightBytes of the threads' reads are on the card at once, in the SD driver's high class first come first
//    served with the foreground's: a foreground read waits behind at most that much, the prefetch keeps a large share
//    of the card, and it stays ahead of the foreground, whose page-ins then find their pages resident (or in flight,
//    and wait for them). It stops when the list is done, or at the end of the boot's window as a recording would.
// The volumes are read-only, so a stale list costs only wasted reads, but those are boot time: a changed image is
// re-recorded, and so is a list a replay boot shows no longer matches the boot (switchListStale). A list this build
// cannot read is a recording boot with no prefetch, so loadList says why in one line, and the format is frozen
// (SwitchCryptexMap.h): a new one must read the old.
namespace {
constexpr uint32_t kPage = 4096;   // the list's page and the image cache's block, whatever the VM page size
constexpr uint32_t kMaxRun = 128;  // pages: 512 KiB per page-in, as the old 512 KiB advisory reads
constexpr uint32_t kMaxGap = 8;    // read through up to 32 KiB of unrecorded pages rather than start another page-in
constexpr uint64_t kInFlightBytes = static_cast<uint64_t>(3 * kMaxRun) * kPage;
constexpr uint64_t kRecordAfterBootS = 5;
constexpr uint64_t kListCapPages = (768ULL << 20) / kPage;
constexpr uint32_t kLogEntries = 1u << 16;  // 2 MiB, allocated on recording boots only
// The 11-04 boots reached SpringBoard 110-135 s after the mounts.
constexpr uint64_t kRecordBackstopS = 180;
// A prefetch read on the card this long with no other finishing was lost (they complete from the SD driver's interrupts).
constexpr uint32_t kFlowTimeoutS = 30;
constexpr char kListSuffix[] = ".boot-prefetch";

using ListHeader = SwitchListHeader;
using Run = SwitchRun;
static_assert(kSwitchListImages == SWITCH_CRYPTEX_COUNT, "the list keys both cryptex images");

// How a cryptex volume's file blocks become image offsets, for the read-ahead.
enum MapState : uint8_t {
    kMapOff,
    kMapUnproven,
    kMapProving,
    kMapOn
};

struct VolumeMap {
    uint32_t blockSize;  // the volume device's: VNOP_BLOCKMAP's unit
    bool physical;       // DKIOCGETPHYSICALEXTENT answers (else the volume's offsets are the image's)
    dev_t store;         // the device it names: one of the image device's media
    uint64_t base;       // where that media starts in the image
    uint8_t state;
};

struct {
    ListHeader key;  // this boot's images
    mount_t mounts[SWITCH_CRYPTEX_COUNT];
    switch_record_entry *log = nullptr;  // never freed: vfs_cluster.c may still be writing an entry
    Run *runs = nullptr;
    uint32_t runCount = 0;
    uint64_t runCapacity = 0;  // the recording's runs allocation (saveRecording)
    thread_call_t backstopCall = nullptr, loadCall = nullptr, bootCall = nullptr;
    volatile uint32_t frame = 0, stop = 0, started = 0, bootComplete = 0;
    uint64_t listPages = 0;  // the replayed list's
    // ioLeft: the boot's read phases still running (the replay, the recording window); at 0 the cache shrinks.
    uint32_t next = 0, done = 0, files = 0, missing = 0, workers = 0, ioLeft = 0, windowDone = 0;
    uint64_t bytes = 0, startedMs = 0;
    VolumeMap map[SWITCH_CRYPTEX_COUNT] = {};
} gPrefetch;

void listPath(char *out, size_t capacity)
{
    snprintf(out, capacity, "%s%s", paths[0], kListSuffix);
}

bool imageKeys(ListHeader *key)
{
    bzero(key, sizeof(*key));
    memcpy(key->magic, kSwitchListMagic, sizeof(kSwitchListMagic));
    key->version = kSwitchListVersion;
    key->headerBytes = sizeof(*key);
    for (unsigned i = 0; i < SWITCH_CRYPTEX_COUNT; ++i) {
        vnode_t image = NULLVP;
        if (vnode_lookup(paths[i], 0, &image, vfs_context_current()))
            return false;
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_data_size);
        VATTR_WANTED(&va, va_fileid);
        VATTR_WANTED(&va, va_modify_time);
        const bool ok = (vnode_getattr(image, &va, vfs_context_current()) == 0) &&
                        VATTR_IS_SUPPORTED(&va, va_data_size) && VATTR_IS_SUPPORTED(&va, va_fileid) &&
                        VATTR_IS_SUPPORTED(&va, va_modify_time);
        vnode_put(image);
        if (!ok)
            return false;
        key->images[i] = {va.va_data_size, va.va_fileid, va.va_modify_time.tv_sec, va.va_modify_time.tv_nsec};
    }
    return true;
}

// The saved runs for these images, or nullptr with *why (none, another format, other images, torn or unreadable: the
// caller records afresh).
Run *loadList(const ListHeader &key, uint32_t *count, const char **why)
{
    char path[128];
    listPath(path, sizeof(path));
    vfs_context_t ctx = vfs_context_current();
    vnode_t vp = NULLVP;
    if (vnode_open(path, FREAD | O_NOFOLLOW, 0, 0, &vp, ctx))
        return *why = "none saved", nullptr;
    ListHeader header = {};
    int resid = 0;
    Run *runs = nullptr;
    if (vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(&header), sizeof(header), 0, UIO_SYSSPACE, IO_NOAUTH,
                vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) ||
        resid) {
        *why = "its header is unreadable";
    } else {
        *why = switchListRefusal(header, key.images, kLogEntries * 4u);
    }
    if (!*why) {
        const size_t bytes = header.runs * sizeof(Run);
        runs = static_cast<Run *>(IOMallocData(bytes));
        if (!runs) {
            *why = "no memory";
        } else if (vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(runs), static_cast<int>(bytes), sizeof(header),
                           UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) ||
                   resid || switchFnv(reinterpret_cast<const uint8_t *>(runs), bytes) != header.checksum) {
            IOFreeData(runs, bytes);
            runs = nullptr;
            *why = "torn (its runs do not match its checksum)";
        } else {
            *count = header.runs;
        }
    }
    vnode_close(vp, FREAD, ctx);
    return runs;
}

// Waits until fewer than `limit` bytes of the prefetch's reads are on the card (finishRequest and jobFinish wake it).
void waitFlowBelow(uint64_t limit)
{
    IOLockLock(gFlow.lock);
    while (gFlow.inflight >= limit) {
        gFlow.waiting = true;
        uint64_t deadline = 0;
        clock_interval_to_deadline(kFlowTimeoutS, NSEC_PER_SEC, &deadline);
        if (IOLockSleepDeadline(gFlow.lock, &gFlow.inflight, deadline, THREAD_UNINT) == THREAD_TIMED_OUT &&
            gFlow.inflight >= limit) {
            panic("SwitchBootPrefetch: %llu bytes of its reads still on the card after %u s with none finishing",
                  gFlow.inflight, kFlowTimeoutS);
        }
    }
    IOLockUnlock(gFlow.lock);
}

// Where BSD device `dev` (the device DKIOCGETPHYSICALEXTENT names) starts in the cryptex image, when it is one of the
// image device's media (its whole disk, or a partition of it: IOMedia's Base); false when it is not.
bool mediaBase(IOService *device, dev_t dev, uint64_t *base)
{
    IORegistryIterator *it = IORegistryIterator::iterateOver(device, gIOServicePlane, kIORegistryIterateRecursively);
    if (!it)
        return false;
    bool found = false;
    while (const IORegistryEntry *entry = it->getNextObject()) {
        const auto *majorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMajorKey));
        const auto *minorNumber = OSDynamicCast(OSNumber, entry->getProperty(kIOBSDMinorKey));
        if (!majorNumber || !minorNumber ||
            makedev(majorNumber->unsigned32BitValue(), minorNumber->unsigned32BitValue()) != dev)
            continue;
        const auto *at =
            OSDynamicCast(OSNumber, entry->getProperty("Base"));  // kIOMediaBaseKey (not in this storage ABI)
        *base = at ? at->unsigned64BitValue() : 0;
        found = true;
        break;
    }
    it->release();
    return found;
}

// Checked on first use (mapProven).
void prepareMap(unsigned index)
{
    VolumeMap &m = gPrefetch.map[index];
    m.state = kMapOff;
    auto *device = OSDynamicCast(SwitchCryptexDevice, directDevice[index]);
    if (!device || !__atomic_load_n(&gCache.on, __ATOMIC_ACQUIRE) || !gPrefetch.mounts[index])
        return;
    vnode_t devvp = vfs_devvp(gPrefetch.mounts[index]);
    if (!devvp)
        return;
    vfs_context_t ctx = vfs_context_kernel();
    const char *why = nullptr;
    dk_physical_extent_t request = {};
    request.length = kPage;
    int error = 0;
    if (VNOP_IOCTL(devvp, DKIOCGETBLOCKSIZE, reinterpret_cast<caddr_t>(&m.blockSize), 0, ctx) || !m.blockSize) {
        why = "no block size";
    } else {
        error = VNOP_IOCTL(devvp, DKIOCGETPHYSICALEXTENT, reinterpret_cast<caddr_t>(&request), 0, ctx);
        if (error) {
            if (error != ENOTTY)
                why = "DKIOCGETPHYSICALEXTENT failed";
            m.physical = false;  // ENOTTY: the volume's offsets are the image's (mapProven checks that)
            m.base = 0;
        } else if (!mediaBase(device, request.dev, &m.base)) {
            why = "its volume's store is not on the image device";
        } else {
            m.physical = true;
            m.store = request.dev;
        }
    }
    vnode_put(devvp);
    if (why) {
        IOLog("SwitchBootPrefetch: no read-ahead for %s (%s); its runs are paged in block by block\n", paths[index],
              why);
    } else {
        m.state = kMapUnproven;
    }
}

// A logical offset on the volume's device as an image offset; *length shrinks to what stays contiguous there.
bool toImage(vnode_t devvp, unsigned index, uint64_t logical, uint64_t *image, uint64_t *length)
{
    const VolumeMap &m = gPrefetch.map[index];
    uint64_t at = logical;
    if (m.physical) {
        dk_physical_extent_t request = {};
        request.offset = logical;
        request.length = *length;
        if (VNOP_IOCTL(devvp, DKIOCGETPHYSICALEXTENT, reinterpret_cast<caddr_t>(&request), 0, vfs_context_kernel()) ||
            !request.length || request.dev != m.store)
            return false;
        at = request.offset;
        if (request.length < *length)
            *length = request.length;
    }
    *image = m.base + at;
    return (*image % kPage) == 0u;
}

// The first read-ahead of each image checks its map on real bytes: the file's page at `offset` (vn_rdwr, through APFS
// and its checks) must be the image's block at `image` (through the device). Until one thread has checked, the others
// read nothing ahead; a map that fails turns the image's read-ahead off (one line).
bool mapProven(SwitchCryptexDevice *device, vnode_t vp, unsigned index, off_t offset, uint64_t image)
{
    VolumeMap &m = gPrefetch.map[index];
    uint8_t state = __atomic_load_n(&m.state, __ATOMIC_ACQUIRE);
    if (state != kMapUnproven)
        return state == kMapOn;
    if (!__atomic_compare_exchange_n(&m.state, &state, static_cast<uint8_t>(kMapProving), false, __ATOMIC_ACQ_REL,
                                     __ATOMIC_ACQUIRE))
        return false;
    IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::withOptions(kIODirectionIn, kPage, kPage);
    auto *file = static_cast<uint8_t *>(IOMallocData(kPage));
    const bool prepared = (buffer != nullptr) && buffer->prepare() == kIOReturnSuccess;
    const char *why = prepared && file ? nullptr : "no memory";
    vfs_context_t ctx = vfs_context_kernel();
    int resid = 0;
    if (!why && (vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(file), kPage, offset, UIO_SYSSPACE, IO_NOAUTH,
                         vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx)) ||
                 resid >= static_cast<int>(kPage))) {
        why = "its first file page could not be read";
    } else if (!why && image + kPage > device->imageBytes()) {
        why = "the map points past the image";
    } else if (!why && device->readBlock(buffer, image / kPage) != kIOReturnSuccess) {
        why = "the image could not be read";
    } else if (!why && memcmp(buffer->getBytesNoCopy(), file, kPage - static_cast<uint32_t>(resid))) {
        why = "a file page is not the image block its map names";
    }
    if (prepared)
        buffer->complete();
    OSSafeReleaseNULL(buffer);
    if (file)
        IOFreeData(file, kPage);
    __atomic_store_n(&m.state, static_cast<uint8_t>(why ? kMapOff : kMapOn), __ATOMIC_RELEASE);
    if (why) {
        IOLog("SwitchBootPrefetch: no read-ahead for %s (%s); its runs are paged in block by block\n", paths[index],
              why);
    }
    return why == nullptr;
}

// The file's absent pages [at, at + bytes), extent by extent, read ahead into the cache.
void mapAhead(SwitchCryptexDevice *device, vnode_t devvp, vnode_t vp, unsigned index, off_t at, off_t bytes)
{
    const VolumeMap &m = gPrefetch.map[index];
    for (off_t done = 0; done < bytes;) {
        daddr64_t blkno = -1;
        size_t run = 0;
        if (VNOP_BLOCKMAP(vp, at + done, static_cast<size_t>(bytes - done), &blkno, &run, nullptr, VNODE_READ,
                          nullptr) ||
            !run)
            return;
        if (run > static_cast<size_t>(bytes - done))
            run = static_cast<size_t>(bytes - done);
        uint64_t length = run;
        if (blkno >= 0) {  // (a hole reads as zeros: nothing to read ahead)
            uint64_t image = 0;
            if (!toImage(devvp, index, static_cast<uint64_t>(blkno) * m.blockSize, &image, &length) ||
                !mapProven(device, vp, index, at + done, image))
                return;
            device->readAhead(image, length);
        }
        done += static_cast<off_t>(length);
    }
}

// So that the page-in's block-at-a-time reads find the run in the cache instead of each going to the card.
void readAheadRun(vnode_t vp, unsigned index, off_t offset, off_t length)
{
    auto *device = OSDynamicCast(SwitchCryptexDevice, directDevice[index]);
    if (!device || !__atomic_load_n(&gCache.on, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&gPrefetch.map[index].state, __ATOMIC_ACQUIRE) == kMapOff)
        return;
    vnode_t devvp = vfs_devvp(vnode_mount(vp));
    if (!devvp)
        return;
    const off_t end = offset + length;
    for (off_t at = offset; at < end;) {
        int absent = 0;
        int present = 0;
        if (ubc_range_op(vp, at, end, UPL_ROP_ABSENT, &absent) != KERN_SUCCESS || absent < 0)
            break;
        if (absent) {
            mapAhead(device, devvp, vp, index, at, absent);
            at += absent;
        } else if (ubc_range_op(vp, at, end, UPL_ROP_PRESENT, &present) != KERN_SUCCESS || present <= 0) {
            break;
        } else {
            at += present;  // resident, or being paged in by someone else
        }
    }
    vnode_put(devvp);
}

// The boot's reads are done (the replay, and the boot's window up to endBootWindow): the cache gives back its idle
// windows beyond kSteadyWindows (all of them if it is off).
void bootIoDone()
{
    if (__atomic_sub_fetch(&gPrefetch.ioLeft, 1, __ATOMIC_ACQ_REL))
        return;
    if (!gCache.lock || !gCache.count)
        return;
    const uint32_t keep = __atomic_load_n(&gCache.on, __ATOMIC_ACQUIRE) ? kSteadyWindows : 0;
    for (uint32_t batch = 32; batch == 32;) {
        SwitchWindow *retired[32];
        batch = 0;
        IOLockLock(gCache.lock);
        while (batch < 32) {
            retired[batch] = gCache.cache.retire(keep);
            if (!retired[batch])
                break;
            ++batch;
        }
        IOLockUnlock(gCache.lock);
        for (uint32_t i = 0; i < batch; ++i) {
            auto *memory = static_cast<IOBufferMemoryDescriptor *>(retired[i]->memory);
            retired[i]->memory = retired[i]->bytes = nullptr;
            memory->complete();
            memory->release();
        }
    }
}

// The last prefetch thread out: the summary, once the last of its reads have arrived.
void prefetchDone()
{
    waitFlowBelow(1);
    const uint64_t started = gPrefetch.startedMs;
    const uint64_t now = switch_now_ms();
    SwitchCacheStats s = {};
    if (gCache.lock) {
        IOLockLock(gCache.lock);
        s = gCache.cache.stats();
        IOLockUnlock(gCache.lock);
    }
    IOLog("SwitchBootPrefetch: %llu MiB of the cryptexes' files (%u lookups) in %u of %u runs, %llu.%llu s to "
          "%llu.%llu s uptime%s; %llu MiB read ahead, %llu%% of it used; meanwhile the foreground read the direct maps "
          "%llu times (%llu MiB)%s; cache: %llu of %llu reads found every block there, %llu left to the card, %llu MiB "
          "in %llu card reads, %llu windows reused unused\n",
          gPrefetch.bytes >> 20, gPrefetch.files, gPrefetch.done, gPrefetch.runCount, started / 1000,
          started / 100 % 10, now / 1000, now / 100 % 10, gPrefetch.stop ? " (stopped)" : "",
          (s.aheadBlocks * kPage) >> 20, s.aheadBlocks ? s.aheadServed * 100 / s.aheadBlocks : 0, gFlow.foregroundReads,
          gFlow.foregroundBytes >> 20, gPrefetch.missing ? ", some files gone" : "", s.ready, s.requests, s.refused,
          (s.blocksRead * kPage) >> 20, s.reads, s.evicted);
    IOFreeData(gPrefetch.runs, gPrefetch.runCount * sizeof(Run));
    gPrefetch.runs = nullptr;
    bootIoDone();
}

// One of kWorkers prefetch threads: takes the next run in order, reads it ahead into the cache, pages it in; at most
// kInFlightBytes of the threads' reads on the card (on DiskImages nothing is counted and each page-in waits for its own).
// NOLINTNEXTLINE(misc-const-correctness): thread_continue_t fixes the signature
void prefetchWorker(void *parameter, wait_result_t)
{
    const auto slot = static_cast<unsigned>(reinterpret_cast<uintptr_t>(parameter));
    __atomic_store_n(&gFlow.threads[slot], current_thread(), __ATOMIC_RELEASE);
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    uint64_t vpFile = 0;
    unsigned vpIndex = SWITCH_CRYPTEX_COUNT;
    off_t vpSize = 0;
    while (!gPrefetch.stop) {
        const uint32_t i = __atomic_fetch_add(&gPrefetch.next, 1, __ATOMIC_RELAXED);
        if (i >= gPrefetch.runCount)
            break;
        const Run &run = gPrefetch.runs[i];
        waitFlowBelow(kInFlightBytes);
        __atomic_fetch_add(&gPrefetch.done, 1, __ATOMIC_RELAXED);
        if (run.index >= SWITCH_CRYPTEX_COUNT || !gPrefetch.mounts[run.index])
            continue;
        if (run.fileId != vpFile || run.index != vpIndex) {
            if (vp)
                vnode_put(vp);
            vp = NULLVP;
            vpFile = run.fileId;
            vpIndex = run.index;
            if (VFS_VGET(gPrefetch.mounts[run.index], static_cast<ino64_t>(run.fileId), &vp, ctx) || !vnode_isreg(vp)) {
                if (vp)
                    vnode_put(vp);
                vp = NULLVP;
                __atomic_fetch_add(&gPrefetch.missing, 1, __ATOMIC_RELAXED);
                continue;
            }
            vpSize = ubc_getsize(vp);
            __atomic_fetch_add(&gPrefetch.files, 1, __ATOMIC_RELAXED);
        }
        if (!vp)
            continue;
        SwitchPageIn in;
        if (!switchRunPageIn(run, kPage, PAGE_SIZE, static_cast<uint64_t>(vpSize), &in))
            continue;
        const off_t offset = static_cast<off_t>(in.offset);
        const off_t length = static_cast<off_t>(in.length);
        if (directDevice[run.index])
            readAheadRun(vp, run.index, offset, length);
        int error = 0;
        (void)vnode_pagein(vp, nullptr, 0, static_cast<vm_object_offset_t>(offset), static_cast<upl_size_t>(length),
                           directDevice[run.index] ? 0 : UPL_IOSYNC, &error);
        __atomic_fetch_add(&gPrefetch.bytes, static_cast<uint64_t>(length), __ATOMIC_RELAXED);
    }
    if (vp)
        vnode_put(vp);
    __atomic_store_n(&gFlow.threads[slot], THREAD_NULL, __ATOMIC_RELEASE);
    if (!__atomic_sub_fetch(&gPrefetch.workers, 1, __ATOMIC_ACQ_REL))
        prefetchDone();
    thread_terminate(current_thread());
}

using Range = SwitchRange;  // a logged read's file, pages [first, end) and first-read second

int compareEntries(const void *a, const void *b)
{
    const auto *x = static_cast<const switch_record_entry *>(a);
    const auto *y = static_cast<const switch_record_entry *>(b);
    if (x->vp != y->vp)
        return x->vp < y->vp ? -1 : 1;
    return x->vid < y->vid ? -1 : static_cast<int>(x->vid > y->vid);
}

int compareRanges(const void *a, const void *b)
{
    return switchCompareRanges(*static_cast<const Range *>(a), *static_cast<const Range *>(b));
}

int compareRuns(const void *a, const void *b)
{
    return switchCompareRuns(*static_cast<const Run *>(a), *static_cast<const Run *>(b));
}

// One logged vnode (its entries are log[first, end) after sorting) and the file it is, once named.
struct Group {
    vnode_t vp;
    uint32_t vid, first, end;
    uint64_t fileId;
};

struct Naming {
    Group *groups;
    uint32_t count;
};

// vnode_iterate on a cryptex mount: a live vnode that the log holds (same vid) gets its file id. The log's vnode
// pointers are never dereferenced: a vnode recycled since its read is simply not found, and its reads not kept.
int nameVnode(vnode_t vp, void *argument)
{
    const auto *naming = static_cast<Naming *>(argument);
    // The groups are sorted by (vp, vid): the first with this vnode pointer (it may appear with several vids).
    uint32_t lo = 0;
    uint32_t hi = naming->count;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (naming->groups[mid].vp < vp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    for (Group *hit = naming->groups + lo; hit < naming->groups + naming->count && hit->vp == vp; ++hit) {
        if (hit->vid != vnode_vid(vp) || !vnode_isreg(vp) || vnode_isnamedstream(vp))
            continue;
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        if (!vnode_getattr(vp, &va, vfs_context_kernel()) && VATTR_IS_SUPPORTED(&va, va_fileid))
            hit->fileId = va.va_fileid;
    }
    return VNODE_RETURNED;
}

// The log's reads as runs (in first-read order); returns their count, *files the distinct files, *out the runs.
uint32_t buildRuns(switch_record_entry *log, uint32_t entries, Run **out, uint32_t *files, uint32_t *dropped)
{
    *out = nullptr;
    *files = *dropped = 0;
    qsort(log, entries, sizeof(*log), compareEntries);
    auto *groups = static_cast<Group *>(IOMallocData((entries ? entries : 1) * sizeof(Group)));
    auto *ranges = static_cast<Range *>(IOMallocData((entries ? entries : 1) * sizeof(Range)));
    uint32_t groupCount = 0;
    uint32_t n = 0;
    uint32_t count = 0;
    uint64_t pages = 0;
    Run *runs = nullptr;
    if (groups && ranges) {
        for (uint32_t i = 0; i < entries;) {
            uint32_t j = i;
            while (j < entries && log[j].vp == log[i].vp && log[j].vid == log[i].vid)
                ++j;
            groups[groupCount++] = {log[i].vp, log[i].vid, i, j, 0};
            i = j;
        }
        Naming naming = {groups, groupCount};
        for (auto &mount : gPrefetch.mounts) {
            if (mount)
                (void)vnode_iterate(mount, 0, nameVnode, &naming);
        }
        for (uint32_t g = 0; g < groupCount; ++g) {
            for (uint32_t i = groups[g].first; i < groups[g].end; ++i) {
                const uint64_t first = log[i].offset / kPage;
                const uint64_t end = (log[i].offset + log[i].length + kPage - 1) / kPage;
                if (!groups[g].fileId || !log[i].when || end > UINT32_MAX) {
                    ++*dropped;
                    continue;
                }
                ranges[n++] = {groups[g].fileId, static_cast<uint32_t>(first), static_cast<uint32_t>(end), log[i].when,
                               static_cast<uint8_t>(log[i].index)};
                pages += end - first;
            }
        }
        qsort(ranges, n, sizeof(Range), compareRanges);
        const uint64_t capacity = switchRunCapacity(n, pages, kMaxRun, kMaxGap);
        runs = n && capacity <= kLogEntries * 4ULL ? static_cast<Run *>(IOMallocData(capacity * sizeof(Run))) : nullptr;
        if (runs)
            count = switchMergeRuns(ranges, n, runs, capacity, kMaxRun, kMaxGap, files);
        if (runs)
            qsort(runs, count, sizeof(Run), compareRuns);
        if (runs && !count) {
            IOFreeData(runs, capacity * sizeof(Run));
            runs = nullptr;
        }
        gPrefetch.runCapacity = runs ? capacity : 0;
    }
    if (groups)
        IOFreeData(groups, (entries ? entries : 1) * sizeof(Group));
    if (ranges)
        IOFreeData(ranges, (entries ? entries : 1) * sizeof(Range));
    *out = runs;
    return runs ? count : 0;
}

// A recording boot's log, saved as the next boots' list.
void saveRecording()
{
    switch_record_entry *log = __atomic_exchange_n(&gPrefetch.log, nullptr, __ATOMIC_ACQ_REL);
    if (!log)
        return;
    const uint32_t appended = switch_cluster_record_stop();
    const uint32_t entries = appended < kLogEntries ? appended : kLogEntries;
    Run *runs = nullptr;
    uint32_t files = 0;
    uint32_t dropped = 0;
    const uint32_t built = buildRuns(log, entries, &runs, &files, &dropped);
    const uint32_t count = switchCapRuns(runs, built, kListCapPages);
    ListHeader header = gPrefetch.key;
    header.runs = count;
    header.files = files;
    header.checksum = switchFnv(reinterpret_cast<const uint8_t *>(runs), count * sizeof(Run));
    uint64_t pages = 0;
    for (uint32_t i = 0; i < count; ++i)
        pages += runs[i].pages;
    char path[128];
    listPath(path, sizeof(path));
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int error = !count ? ENOENT : vnode_open(path, O_CREAT | O_TRUNC | FWRITE | O_NOFOLLOW, 0644, 0, &vp, ctx);
    if (!error) {
        int resid = 0;
        error = vn_rdwr(UIO_WRITE, vp, reinterpret_cast<caddr_t>(&header), sizeof(header), 0, UIO_SYSSPACE,
                        IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
        if (!error) {
            error = vn_rdwr(UIO_WRITE, vp, reinterpret_cast<caddr_t>(runs), static_cast<int>(count * sizeof(Run)),
                            sizeof(header), UIO_SYSSPACE, IO_NOAUTH | IO_UNIT, vfs_context_ucred(ctx), &resid,
                            vfs_context_proc(ctx));
        }
        vnode_close(vp, FWRITE, ctx);
    }
    if (runs)
        IOFreeData(runs, gPrefetch.runCapacity * sizeof(Run));
    if (error) {
        IOLog("SwitchBootPrefetch: %s not saved: %d (%u reads logged)\n", path, error, appended);
    } else {
        IOLog("SwitchBootPrefetch: recorded %u runs (%llu MiB) of %u cryptex files for the next boots (%u of %u "
              "reads not kept, %u runs past the %llu MiB cap)\n",
              count, (pages * kPage) >> 20, files, dropped + (appended - entries), appended, built - count,
              (kListCapPages * kPage) >> 20);
    }
}

// The next boots record afresh: the list is emptied, which loadList refuses.
void dropList(const char *why)
{
    char path[128];
    listPath(path, sizeof(path));
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    const int error = vnode_open(path, O_TRUNC | FWRITE | O_NOFOLLOW, 0, 0, &vp, ctx);
    if (!error)
        vnode_close(vp, FWRITE, ctx);
    IOLog("SwitchBootPrefetch: list of %llu MiB dropped (%s), the next boot records afresh%s\n",
          (gPrefetch.listPages * kPage) >> 20, why, error ? " (it could not be emptied)" : "");
}

// The boot's read window ends: kRecordAfterBootS after System Boot Complete (complete), or at the backstop. The replay
// stops; a recording is kept only from a boot that reached SpringBoard. Whichever comes first acts.
void endBootWindow(bool complete)
{
    gPrefetch.stop = 1;
    if (__atomic_exchange_n(&gPrefetch.windowDone, 1u, __ATOMIC_ACQ_REL))
        return;
    gFlow.window = false;
    if (complete) {
        saveRecording();
        const SwitchReplayResult replay = {gPrefetch.listPages, gFlow.foregroundBytes, gPrefetch.missing};
        if (const char *stale = gPrefetch.runCount ? switchListStale(replay, kListCapPages, kPage) : nullptr)
            dropList(stale);
    } else if (__atomic_exchange_n(&gPrefetch.log, nullptr, __ATOMIC_ACQ_REL)) {
        const uint32_t logged = switch_cluster_record_stop();
        IOLog("SwitchBootPrefetch: no System Boot Complete %llu s after the mounts: the %u reads logged are not kept\n",
              kRecordBackstopS, logged);
    }
    bootIoDone();
}

void afterBoot(thread_call_param_t, thread_call_param_t)
{
    endBootWindow(true);
}

void backstop(thread_call_param_t, thread_call_param_t)
{
    endBootWindow(false);
}

// Temporary (r8-sdboot, 11-04): CPU time per core from boot to the first frame, to tell what bounds the boot once the
// SD card no longer does. One line, once.
void cpuLoad(thread_call_param_t, thread_call_param_t)
{
    char line[256];
    int at = snprintf(line, sizeof(line), "SwitchBoot CPU to first frame (user/system/idle s per core):");
    for (int cpu = 0; cpu <= ml_get_max_cpu_number() && at > 0 && at < static_cast<int>(sizeof(line)); ++cpu) {
        processor_t processor = cpu_to_processor(cpu);
        if (!processor)
            continue;
        natural_t ticks[CPU_STATE_MAX] = {};
        processor_cpu_load_info(processor, ticks);
        at += snprintf(line + at, sizeof(line) - static_cast<size_t>(at), " %d: %u/%u/%u", cpu,
                       ticks[CPU_STATE_USER] / 100, ticks[CPU_STATE_SYSTEM] / 100, ticks[CPU_STATE_IDLE] / 100);
    }
    IOLog("%s\n", line);
}

// Both cryptexes are mounted: replay the list, or record one.
void prefetchStart()
{
    if (gPrefetch.started)
        return;
    gPrefetch.started = 1;
    if (!imageKeys(&gPrefetch.key)) {
        IOLog("SwitchBootPrefetch: off this boot (the images' attributes)\n");
        return;
    }
    gPrefetch.loadCall = thread_call_allocate_with_options(cpuLoad, nullptr, THREAD_CALL_PRIORITY_LOW, 0);
    gPrefetch.backstopCall = thread_call_allocate_with_options(backstop, nullptr, THREAD_CALL_PRIORITY_LOW, 0);
    gPrefetch.bootCall = thread_call_allocate_with_options(afterBoot, nullptr, THREAD_CALL_PRIORITY_LOW, 0);
    if (!gPrefetch.backstopCall || !gPrefetch.bootCall)
        return;
    uint32_t count = 0;
    const char *why = nullptr;
    Run *runs = loadList(gPrefetch.key, &count, &why);
    // The boot's read phases: the replay (if any) and the boot's window, both before endBootWindow can run.
    gPrefetch.ioLeft = runs ? 2 : 1;
    uint64_t deadline = 0;
    clock_interval_to_deadline(static_cast<uint32_t>(kRecordBackstopS), NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(gPrefetch.backstopCall, deadline);
    gFlow.foregroundReads = gFlow.foregroundBytes = 0;
    gFlow.window = true;
    if (runs) {
        gPrefetch.runs = runs;
        gPrefetch.runCount = count;
        for (uint32_t i = 0; i < count; ++i)
            gPrefetch.listPages += runs[i].pages;
        for (unsigned i = 0; i < SWITCH_CRYPTEX_COUNT; ++i)
            prepareMap(i);
        gPrefetch.startedMs = switch_now_ms();
        gPrefetch.workers = kWorkers;
        for (unsigned i = 0; i < kWorkers; ++i) {
            thread_t thread = THREAD_NULL;
            if (kernel_thread_start(prefetchWorker, reinterpret_cast<void *>(static_cast<uintptr_t>(i)), &thread) ==
                KERN_SUCCESS) {
                thread_deallocate(thread);
            } else if (!__atomic_sub_fetch(&gPrefetch.workers, 1, __ATOMIC_ACQ_REL)) {
                prefetchDone();
            }
        }
        return;
    }
    IOLog("SwitchBootPrefetch: no list to replay (%s); this boot records one\n", why);
    auto *log = static_cast<switch_record_entry *>(IOMallocZeroData(kLogEntries * sizeof(switch_record_entry)));
    if (!log)
        return;
    gPrefetch.log = log;
    switch_cluster_record_start(log, kLogEntries);
}
}  // namespace

// SwitchRawMount.h, right after each cryptex mount: the cluster layer's cryptex mounts (the recording, the kprof
// waits line's "cx"), and once both are mounted, the boot prefetch.
extern "C" void switch_cryptex_mounted(unsigned index, mount_t mount)
{
    if (index >= SWITCH_CRYPTEX_COUNT || !mount)
        return;
    switch_cluster_register_cryptex_mount(index, mount);
    gPrefetch.mounts[index] = mount;
    if (gPrefetch.mounts[0] && gPrefetch.mounts[SWITCH_CRYPTEX_COUNT - 1])
        prefetchStart();
}

// NativeLegacyDisplay calls this (weakly) at each frame until the first one shows, in any context, interrupts possibly
// disabled.
extern "C" void switch_boot_io_report(void)
{
    if (__atomic_exchange_n(&gPrefetch.frame, 1u, __ATOMIC_RELAXED) || !gPrefetch.loadCall)
        return;
    thread_call_enter(gPrefetch.loadCall);
}

// SwitchPower, at System Boot Complete (IOPMrootDomain's boot-done hint, on its work loop): the boot's window ends
// kRecordAfterBootS later.
extern "C" void switch_boot_io_complete(void)
{
    if (__atomic_exchange_n(&gPrefetch.bootComplete, 1u, __ATOMIC_RELAXED) || !gPrefetch.bootCall)
        return;
    uint64_t deadline = 0;
    clock_interval_to_deadline(static_cast<uint32_t>(kRecordAfterBootS), NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(gPrefetch.bootCall, deadline);
}
