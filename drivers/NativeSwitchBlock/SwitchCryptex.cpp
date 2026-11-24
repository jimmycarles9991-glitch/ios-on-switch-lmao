// SPDX-License-Identifier: BSD-2-Clause-Patent
// The unchanged System/App cryptex images from Preboot as read-only block devices: read from the SD card through a
// cache of 128 KiB windows (the direct read map), or through the original DiskImages hook if that map is refused, and
// the APFS volume each one publishes.
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
#include <kern/thread_call.h>
#include <mach/memory_object_types.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <vm/pmap.h>
#include "SwitchClientCopy.h"
#include "SwitchCryptex.h"
#include "SwitchCryptexCache.h"
#include "SwitchCryptexMap.h"
#include "SwitchCryptexStats.h"
#include "../../kits/SwitchTime/SwitchTime.h"

extern "C" {
int di_root_image_ext(const char *, char *, size_t, dev_t *, bool);
void switch_cluster_register_image(vnode_t);  // bsd/vfs/vfs_cluster.c
#if DEVELOPMENT || DEBUG
void switch_cluster_image_direct(uint64_t bytes, uint32_t cardReads);      // bsd/vfs/vfs_cluster.c
void switch_diag_cryptex_cache(size_t (*counts)(char *out, size_t size));  // kernel/switch/diag/pagein.c
#endif
LIBKERN_RETURNS_NOT_RETAINED IOBlockStorageDevice *
switch_block_device(IOService **controller, dev_t *whole, uint64_t *sectors,
                    uint32_t *maxReadSectors);  // NativeSwitchBlock.cpp
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
// direct-I/O files). The cryptex device's reads then go to the SD device, one read per extent piece: no DiskImages
// thread, no VNOP_READ.
//
// The device reports itself as SD storage, not a virtual (disk image) interface, so the cryptex APFS mounts without
// MNTK_VIRTUALDEV. Small requests go through a cache of 128 KiB windows (SwitchCryptexCache.h): a small read that
// misses reads its whole window in one command, and the reads that follow copy their blocks out of it (copypv, as the
// SD driver's bounce path copies into requests). A large read the cache does not hold goes straight into the request's
// pages. APFS checks every block it gets against the seal's hashes, wherever it came from.
// Completions go to the client from a thread call, never from the SD driver's worker, so the hash checks run beside
// the card's next command, and a completion that ever waited for another read of the same image could not block the
// thread that issues it.
//
// The image file stays open (its use count) while the device exists. A refused map (a compressed file, an extent
// outside the window) logs one line and attaches through DiskImages as before.
namespace {
constexpr uint32_t kImageBlock = 4096;  // the cryptex device's block (APFS's), and the images' size multiple
constexpr uint32_t kSdSector = 512;
constexpr uint32_t kMaxExtents = 1u << 16;
static_assert(kImageBlock == kSwitchCacheBlock, "the cache's block is the device's");
// The cache's windows stay at the steady size (8 MiB) from the first attach; boot prefetch pages into the files' own
// VM cache rather than driver windows.
constexpr uint32_t kSteadyWindows = 64;
constexpr uint32_t kMinWindows = 8;
constexpr uint32_t kBuckets = 1024;
constexpr uint64_t kAroundBytes = 16ULL * kImageBlock;  // reads up to 64 KiB read their windows around them
constexpr uint32_t kRequestReads = kSwitchAroundWindows * kSwitchWindowReads;

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
    bool cached;                 // finishRequest copies it out of `cache`'s windows into `buffer`
    bool waited;                 // (debug kernels' counts) a window read was issued or under way for it
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
};

IOLock *gPoolLock;
Pending *gPool;
Job *gJobs;
const uint8_t gZeros[kImageBlock] = {};

// The cache (SwitchCryptexCache.h) is used under its lock; each window's memory is one IOBufferMemoryDescriptor.
struct {
    IOLock *lock;
    SwitchCache cache;
    SwitchWindow *windows;
    SwitchWindow **buckets;
    uint32_t count;
    bool setUp, on;
} gCache;

// The kprof image line's direct reads (one per request) and the card reads they took.
void reportDirect(uint64_t bytes, uint32_t cardReads)
{
#if DEVELOPMENT || DEBUG
    switch_cluster_image_direct(bytes, cardReads);
#else
    (void)bytes;
    (void)cardReads;
#endif
}

void finishRequest(thread_call_param_t, thread_call_param_t);

#if DEVELOPMENT || DEBUG
// The 10 s scheduling report's "SwitchSched cryptex cache" line: the cache's counts since its call before (one caller).
size_t cacheCounts(char *out, size_t size)
{
    static SwitchCacheStats seen;
    IOLockLock(gCache.lock);
    const SwitchCacheStats s = gCache.cache.stats();
    IOLockUnlock(gCache.lock);
    const uint64_t kib = kSwitchCacheBlock >> 10;
    const int n =
        snprintf(out, size,
                 "%llu requests (%llu all held, %llu left to the card); %llu card reads of %llu KiB, %llu KiB "
                 "of it read ahead; %llu KiB served the first time (%llu KiB of the read-ahead); %llu windows "
                 "reused holding blocks never served; %llu reads failed",
                 s.requests - seen.requests, s.ready - seen.ready, s.refused - seen.refused, s.reads - seen.reads,
                 (s.blocksRead - seen.blocksRead) * kib, (s.aheadBlocks - seen.aheadBlocks) * kib,
                 (s.served - seen.served) * kib, (s.aheadServed - seen.aheadServed) * kib, s.evicted - seen.evicted,
                 s.failed - seen.failed);
    seen = s;
    return n < 0 ? 0 : static_cast<size_t>(n) < size ? static_cast<size_t>(n) : size - 1;
}
#endif

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
    const bool waited = p->waited;
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
    returnPending(p);
#if DEVELOPMENT || DEBUG
    const uint64_t completing = mach_absolute_time();
#endif
    // IOStorage::complete, without binding another original IOStorageFamily symbol (as NativeSwitchBlock).
    if (client.action)
        client.action(client.target, client.parameter, status, bytes);
#if DEVELOPMENT || DEBUG
    switch_cryptex_stats::note(copiedBytes, waited, copyAbs, mach_absolute_time() - completing);
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
    returnJob(j);
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
    // `raised`: the card read goes in the SD driver's high class although no client asked for it there (debug kernels'
    // counts).
    bool cacheRead(Pending *p, IOMemoryDescriptor *buffer, uint64_t block, uint64_t count, bool raised);
    uint32_t issueRead(const SwitchCacheRead &read, bool raised);
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
    p->cached = p->waited = false;
    const bool lower = !attributes || (attributes->priority >> 6) != 0;  // the client asked below high (none: default)
    if (cacheRead(p, buffer, block, count, lower))
        return kIOReturnSuccess;
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
            sd->doAsyncReadWrite(sub, piece.disk / kSdSector, piece.length / kSdSector, attributes, &pieceCompletion);
        sub->release();
        if (issued != kIOReturnSuccess)
            pieceDone(this, p, issued, 0);
        ++pieces;
        done += piece.length;
    }
    if (done < length)
        failPending(p, kIOReturnNoMemory);  // the rest was never issued
#if DEVELOPMENT || DEBUG
    switch_cryptex_stats::add(&switch_cryptex_stats::Bytes::direct, done);
#endif
    reportDirect(length, pieces);
    pieceDone(this, p, kIOReturnSuccess, 0);  // the issuing reference
    return kIOReturnSuccess;
}

// True when the cache took the request (finishRequest completes it once its windows hold every block).
bool SwitchCryptexDevice::cacheRead(Pending *p, IOMemoryDescriptor *buffer, uint64_t block, uint64_t count, bool raised)
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
    if (taken) {
        p->remaining = p->cache.waiting ? 2 : 1;  // the issuing reference, and the wait for the windows
        p->waited = p->cache.waiting != 0;
    }
    IOLockUnlock(gCache.lock);
    if (!taken) {
        p->cached = false;
        p->buffer = nullptr;
        return false;
    }
    buffer->retain();
    uint32_t cardReads = 0;
    for (uint32_t i = 0; i < n; ++i)
        cardReads += issueRead(p->reads[i], raised);
    reportDirect(p->length, cardReads);
    pieceDone(this, p, kIOReturnSuccess, 0);  // the issuing reference
    return true;
}

// Returns the card reads it issued. The window's blocks are marked being read: nothing else touches them until
// jobFinish. Every cache read goes in the SD driver's high class, whichever request started it: a foreground read may
// be waiting on it, and in a lower class it would wait out the SD driver's 100 ms aging behind a busy foreground.
uint32_t SwitchCryptexDevice::issueRead(const SwitchCacheRead &read, bool raised)
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
    *j = {nullptr, read.window, read.first, read.count, 1, kIOReturnSuccess};
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
        jobFail(j, kIOReturnNoMemory);  // the rest was never issued
#if DEVELOPMENT || DEBUG
    switch_cryptex_stats::add(&switch_cryptex_stats::Bytes::card, done);
    if (raised)
        switch_cryptex_stats::add(&switch_cryptex_stats::Bytes::raised, done);
#else
    (void)raised;
#endif
    jobPieceDone(this, j, kIOReturnSuccess, 0);  // the issuing reference
    return pieces;
}

namespace {
// Once, at the first direct attach: kSteadyWindows, or as many as there is memory for (below kMinWindows none, and
// reads go straight to the card). Allocated here, never in the read path, which page-outs may be waiting on.
bool setupCache()
{
    if (gCache.setUp)
        return gCache.count >= kMinWindows;
    gCache.setUp = true;
    gCache.lock = IOLockAlloc();
    gCache.windows = static_cast<SwitchWindow *>(IOMallocZero(kSteadyWindows * sizeof(SwitchWindow)));
    gCache.buckets = static_cast<SwitchWindow **>(IOMallocZero(kBuckets * sizeof(SwitchWindow *)));
    if (!gCache.lock || !gCache.windows || !gCache.buckets)
        return false;
    uint32_t n = 0;
    for (; n < kSteadyWindows; ++n) {
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
#if DEVELOPMENT || DEBUG
    switch_diag_cryptex_cache(cacheCounts);
#endif
    return true;
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
                        // so its block addresses are the window's.
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
    if (!gCache.setUp) {
        if (setupCache()) {
            __atomic_store_n(&gCache.on, true, __ATOMIC_RELEASE);
        } else {
            IOLog("SwitchCryptex: read cache off (no memory for its windows); every read goes to the card\n");
        }
    }
    if (gCache.on) {
        IOLog("SwitchCryptex: %s read from the card through the read cache (%u windows of %u KiB): %u extents, %llu "
              "MiB\n",
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

#if DEVELOPMENT || DEBUG
extern "C" size_t switch_cryptex_io_totals(char *out, size_t size)
{
    return switch_cryptex_stats::totals(out, size);
}
#endif

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
    const char *why = gPoolLock ? nullptr : "no memory";
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
