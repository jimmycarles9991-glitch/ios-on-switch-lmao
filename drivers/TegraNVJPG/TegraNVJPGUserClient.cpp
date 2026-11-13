// SPDX-License-Identifier: BSD-2-Clause-Patent
// J3: the AppleJPEGDriver service. CMPhoto (ImageIO's JPEG decodes, UIKit's eager decompression, Photos and its
// thumbnails) and JPEGH1.videodecoder (VideoToolbox 'jpeg') open IOServiceMatching("AppleJPEGDriver") and talk to its
// AppleJPEGDriverUserClient; this puts NVJPG behind that pair. The contract (drivers/TegraNVJPG/CONTRACT.md) was read
// from 21H461: the callers in the dyld cache and Apple's com.apple.driver.AppleJPEGDriver 6.6.2 in the kernelcache.
//
// The classes are ours and carry Apple's names: com.apple.driver.AppleJPEGDriver is not in this kernel's prelinked set
// (nothing among its roots depends on it, and no prelinked kext's OSBundleLibraries names it), so the names are free.
// Matching (IOServiceMatching is by class) and the sandbox (iokit-open filters by user-client class) go by them, so
// every process Apple's own profiles let open the client gets it, and no other. The service's registry name is
// TegraNVJPG, not its class name: JPEGH1.videoencoder registers VideoToolbox's hardware 'jpeg' encoders only if
// IOServiceNameMatching("AppleJPEGDriver") finds a service, and NVJPG's encoder is not driven here.
//
//   sel 1  decode (0x58 bytes in and out), sel 5 decode with Apple's codec extensions (0x1000), sel 7 the 2024 decode
//          (0xda0; every decode on 24B119): the JPEG file in the
//          source IOSurface (looked up in the caller's task, like Apple's driver), the picture into the destination
//          IOSurface in its pixel format ('420f', '422f', '444f', 'yuvf', 'BGRA', 'RGBA', 32ARGB), at the request's DCT
//          scale and offset. The
//          kernel parses the file (TegraNVJPGJpeg.h; Apple's hardware parses the stream itself, so userland sends no
//          tables) and plans the decode (TegraNVJPGRequest.h) in the calling thread: a refusal is the call's own
//          result, kIOReturnUnsupported for what NVJPG does not do (TegraNVJPGTraits) and
//          kIOReturnBadArgument for a request that contradicts the file or the surfaces; CMPhoto then decodes that
//          image on the CPU, JPEGH1 that frame in software. Accepted requests are queued for the engine thread in
//          order. Without a callback (+0x30) the call sleeps until its decode is done and returns its status; with one
//          it returns once queued and the result comes back as an IOKit async completion on the port registered with
//          IOConnectSetNotificationPort: callback(refcon +0x38, status, context +0x40).
//   sel 0, 2  no-ops (Apple's). sel 3, 4, 6 encode: kIOReturnUnsupported (CMPhoto's unified
//          encoder falls back to its software encoder, __encodeBuffer 0x1aaeebef4). sel 8, 9 power on/off: Apple's
//          entitlement com.apple.applejpegdriver.poweron, else kIOReturnError, as Apple's; with it a no-op.
//
// Per decode, in the calling thread: the source wired (IOSurface::prepare), its headers read and parsed, its scan
// copied into a kernel buffer (the only CPU work proportional to the file: compressed bytes, a few per cent of the
// picture's); on the engine thread: the destination wired and mapped page by page into NVJPG's output window (past its
// last page a sink page takes the MCU padding the plan allowed), the scan's buffer into the input window, the
// destination's cache lines cleaned and invalidated, the decode run (host1x OP_DONE syncpoint, by interrupt), the
// destination invalidated again for CPU readers, the windows emptied, the surfaces completed and released.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <os/log.h>
#include <sys/proc.h>
#include <string.h>
#include "../TegraVIC/TegraVICSurface.h"
#include "TegraNVJPG.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../../kits/SwitchDMA/SwitchHwPage.h"
#include "../../kits/SwitchMath/SwitchMath.h"
#include "../../kits/SwitchTime/SwitchTime.h"
#include "TegraNVJPGRequest.h"

// TegraGM20B: a surface the GPU left block-linear after its last import (TegraGM20BRecord.h) has its texels moved back
// to rows first, as that import's release did before the move was deferred: NVJPG reads and writes the pages as rows
extern "C" void tegra_gm20b_surface_engine_rows(OSObject *surface) __attribute__((weak));

extern "C" {
vm_offset_t phystokv(uint64_t pa);
}

class AppleJPEGDriverUserClient;

namespace {
constexpr uint32_t kMaxConnections = 1000;  // Apple's limit (newUserClient: opens minus closes >= 1001 refused)
constexpr uint32_t kQueueMax = 16;
constexpr uint64_t kSinkBytes = 1ULL << 20;   // MCU padding past a destination's last page (15 rows of 64 KiB)
constexpr uint32_t kHeaderChunk = 128 << 10;  // headers read first (APP segments: EXIF, ICC, XMP)
constexpr uint32_t kHeaderMax = 4 << 20;      // ... and at most this far, if the first chunk ends before SOS
const uint32_t kOutPages =
    static_cast<uint32_t>(kTegraNVJPGWindowBytes[TEGRA_NVJPG_WINDOW_OUT] >> SWITCH_HW_PAGE_SHIFT);
const uint32_t kInPages = static_cast<uint32_t>(kTegraNVJPGWindowBytes[TEGRA_NVJPG_WINDOW_IN] >> SWITCH_HW_PAGE_SHIFT);

uint64_t *gOutList, *gInList;  // the engine thread's page lists
TegraNVJPGBuffer gSink;        // one arena page, mapped for every sink entry
volatile uint32_t gConnections;
IOLock *gQueueLock;

IOReturn ioreturn(int err)
{
    switch (err) {
    case 0:
        return kIOReturnSuccess;
    case -12:
        return kIOReturnNoMemory;
    case -22:
        return kIOReturnBadArgument;
    case -60:
        return kIOReturnTimeout;
    case -19:
        return kIOReturnNotReady;
    default:
        return kIOReturnIOError;
    }
}

#if TEGRA_NVJPG_STATS
// Stats for the unified log: one line per 5 s window with work (boot console stays quiet).
struct {
    uint64_t windowStart, decodes, refused, failed, engineUs, cpuUs, pixels, bytes;
} gStats;

// Decodes NVJPG finished for the processes that asked last, for HW Test's image.jpeg_* route: a selector 7 call with
// kRouteMagic at +0xcac and no source surface answers the caller's count and the output formats those decodes wrote,
// through the selector CMPhoto's own decodes use, which the app's sandbox admits where it may deny a registry or
// sysctl read. gStatsLock.
constexpr uint32_t kRouteMagic = 0x5050;  // 'PP', as TegraGM20BMethodPaths

struct {
    int pid;
    uint64_t decodes, formats;  // formats: bit n, a decode into TEGRA_NVJPG_OUT_n
} gByProcess[8];

void countDecode(int pid, uint32_t format)
{
    auto *slot = &gByProcess[0];
    for (auto &entry : gByProcess) {
        if (entry.pid == pid) {
            slot = &entry;
            break;
        }
        if (entry.decodes < slot->decodes)
            slot = &entry;
    }
    if (slot->pid != pid)
        *slot = {pid, 0, 0};
    slot->decodes++;
    slot->formats |= 1ull << format;
}

void countsOf(int pid, uint64_t *decodes, uint64_t *formats)
{
    for (const auto &entry : gByProcess) {
        if (entry.pid == pid) {
            *decodes = entry.decodes;
            *formats = entry.formats;
        }
    }
}
#endif

IOLock *gStatsLock;
uint32_t gFirstLogged;  // bit n: a decode into TEGRA_NVJPG_OUT_n has been logged

// `why` is a literal: it is the refusal's key as well as its text.
void refused(const char *why)
{
#if TEGRA_NVJPG_STATS
    IOLockLock(gStatsLock);
    gStats.refused++;
    IOLockUnlock(gStatsLock);
#endif
    switch_refusal("TegraNVJPG", why, "decode refused, the caller decodes on the CPU: %s", why);
}

#if TEGRA_NVJPG_STATS
void statsLine()
{
    IOLockLock(gStatsLock);
    const uint64_t now = switch_now_us();
    if (!gStats.windowStart)
        gStats.windowStart = now;
    if (now - gStats.windowStart < 5000000 || !(gStats.decodes + gStats.refused)) {
        IOLockUnlock(gStatsLock);
        return;
    }
    char line[220];
    const uint64_t n = gStats.decodes ? gStats.decodes : 1;
    snprintf(line, sizeof(line),
             "TegraNVJPG: %llus decodes=%llu refused=%llu failed=%llu engine=%lluus/decode cpu=%lluus/decode "
             "pixels=%lluK/decode scan=%lluKB/decode",
             (now - gStats.windowStart) / 1000000, gStats.decodes, gStats.refused, gStats.failed, gStats.engineUs / n,
             gStats.cpuUs / n, gStats.pixels / n / 1000, gStats.bytes / n / 1024);
    bzero(&gStats, sizeof(gStats));
    gStats.windowStart = now;
    IOLockUnlock(gStatsLock);
    os_log(OS_LOG_DEFAULT, "%s", line);
}
#endif

struct Job {
    Job *next;
    AppleJPEGDriverUserClient *client;  // retained by async jobs until their completion is sent
    TegraVICSurface dst;
    TegraNVJPGPlan plan;
    TegraNVJPGJob engine;
    IOBufferMemoryDescriptor *bits;  // the scan, copied
    uint32_t bitPages;
    uint64_t cpuUs;
    bool async;
    OSAsyncReference64 ref;
    uint64_t context;
    IOReturn status;
    uint32_t bytesUsed, cycles;
    uint64_t engineUs;
    int pid;
    bool done;

    union {
        TegraNVJPGDecodeRequest base;
        TegraNVJPGDecodeRequestExt ext;
        TegraNVJPGDecodeRequest2024 v2024;
    } request;  // the caller's copy, in the job's one allocation
};

enum RequestKind {
    kSelector1,
    kSelector5,
    kSelector7
};

Job *gHead, *gTail;
uint32_t gQueued;

void dropJob(Job *job)
{
    tegra_vic_surface_release(&job->dst);
    OSSafeReleaseNULL(job->bits);
    IODelete(job, Job, 1);
}

// The CPU's part of a grey picture into '420f': neutral chroma over the picture's rows, written through the physical
// aperture of the destination's pages (wired, listed in gOutList), cleaned to DRAM for readers that do not snoop.
void fillChroma(const TegraNVJPGPlan &p, uint32_t pages)
{
    const auto fill = [&] {
        for (uint64_t row = 0; row < p.fillRows; row++) {
            for (uint64_t at = p.fillStart + row * p.fillPitch, end = at + p.fillRowBytes; at < end;) {
                const uint32_t page = static_cast<uint32_t>(at >> SWITCH_HW_PAGE_SHIFT);
                const uint64_t stop = SWITCH_MIN(switch_align_down(at, SWITCH_HW_PAGE_SIZE) + SWITCH_HW_PAGE_SIZE, end);
                if (page >= pages || !switch_dma_pa_managed(gOutList[page]))
                    return;  // the plan bounded it
                uint8_t *va = reinterpret_cast<uint8_t *>(phystokv(gOutList[page])) + (at & SWITCH_HW_PAGE_MASK);
                memset(va, 0x80, stop - at);
                switch_dcache_clean_lines(va, stop - at);
                at = stop;
            }
        }
    };
    fill();
    switch_dcache_sync();
}

// Engine thread: one decode.
IOReturn runJob(Job *job)
{
    const TegraNVJPGPlan &p = job->plan;
    uint32_t pages = 0;
    const uint64_t surfaceBytes = switch_align_up(job->dst.desc.length, SWITCH_HW_PAGE_SIZE);
    IOReturn ret = tegra_vic_surface_prepare(&job->dst, job->dst.desc.length, gOutList, kOutPages, &pages);
    if (ret)
        return ret == kIOReturnNotAligned ? kIOReturnIOError : ret;
    // Sink entries for the MCU padding the plan allowed past the surface's pages.
    uint32_t mapped = pages;
    while ((static_cast<uint64_t>(mapped) << SWITCH_HW_PAGE_SHIFT) < p.writeEnd && mapped < kOutPages)
        gOutList[mapped++] = gSink.pa;
    if ((static_cast<uint64_t>(mapped) << SWITCH_HW_PAGE_SHIFT) < p.writeEnd ||
        surfaceBytes > (static_cast<uint64_t>(pages) << SWITCH_HW_PAGE_SHIFT)) {
        tegra_vic_surface_complete(&job->dst);
        return kIOReturnBadArgument;
    }
    for (uint32_t i = 0; i < job->bitPages; i++) {
        IOByteCount length = 0;
        gInList[i] = job->bits->getPhysicalSegment(static_cast<IOByteCount>(i) << SWITCH_HW_PAGE_SHIFT, &length,
                                                   kIOMemoryMapperNone) &
                     ~SWITCH_HW_PAGE_MASK;
    }
    gInList[job->bitPages] = gSink.pa;  // the engine may read a little past the scan's end
    tegra_nvjpg_lock();
    const uint64_t out = tegra_nvjpg_window_map(TEGRA_NVJPG_WINDOW_OUT, gOutList, mapped, true);
    const uint64_t in = tegra_nvjpg_window_map(TEGRA_NVJPG_WINDOW_IN, gInList, job->bitPages + 1, false);
    if (!out || !in)
        ret = kIOReturnNoSpace;
    // NVJPG does not snoop the CPU's caches: no dirty line of the destination may be written back over the picture.
    if (!ret) {
        switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, gOutList, pages);
        switch_dcache_sync();
    }
    nvjpg_dec_status status = {};
    uint64_t busy = 0;
    if (!ret) {
        job->engine.bitstream = in;
        for (uint32_t i = 0; i < p.planes; i++)
            job->engine.plane[i] = out + p.planeStart[i];
        ret = ioreturn(tegra_nvjpg_run(&job->engine, &status, &busy));
        if (!ret && (status.error_status || !status.bytes_offset))
            ret = kIOReturnIOError;
    }
    // The CPU may have pulled destination lines in while NVJPG wrote: readers must see DRAM.
    switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, gOutList, pages);
    switch_dcache_sync();
    tegra_nvjpg_window_clear(TEGRA_NVJPG_WINDOW_OUT);
    tegra_nvjpg_window_clear(TEGRA_NVJPG_WINDOW_IN);
    tegra_nvjpg_unlock();
    if (!ret && p.fillChroma)
        fillChroma(p, pages);
    tegra_vic_surface_complete(&job->dst);
    // AppleJPEGDriver ends its destination's write with IOSurface's device write end, as VIC does: the seed moves, so a
    // surface a decode wrote never reads as one nothing wrote (TegraGM20BRecord.h) and seed-keyed caches see the picture
    if (!ret)
        tegra_vic_surface_written(&job->dst);
    job->engineUs = busy;
    job->bytesUsed = status.bytes_offset;
    job->cycles = status.reserved0;  // the cycle count (DUMP_CYCLE_COUNT)
    if (ret == kIOReturnIOError && status.error_status) {
        IOLog("TegraNVJPG: decode error %#x after %u bytes (MCU %u,%u) of a %ux%u picture\n", status.error_status,
              status.bytes_offset, status.mcu_x, status.mcu_y, static_cast<unsigned>(job->engine.setup.frame_width),
              static_cast<unsigned>(job->engine.setup.frame_height));
    }
    return ret;
}

void finish(Job *job);

void engineThread(void *, wait_result_t)
{
    for (;;) {
        IOLockLock(gQueueLock);
        while (!gHead)
            IOLockSleep(gQueueLock, &gHead, THREAD_UNINT);
        Job *job = gHead;
        gHead = job->next;
        if (!gHead)
            gTail = nullptr;
        gQueued--;
        IOLockWakeup(gQueueLock, &gQueued, false);
        IOLockUnlock(gQueueLock);
        job->status = runJob(job);
        IOLockLock(gStatsLock);
#if TEGRA_NVJPG_STATS
        gStats.decodes++;
        if (job->status)
            gStats.failed++;
        else
            countDecode(job->pid, job->plan.target.format);
        gStats.engineUs += job->engineUs;
        gStats.cpuUs += job->cpuUs;
        gStats.pixels += static_cast<uint64_t>(job->plan.extent.width) * job->plan.extent.height;
        gStats.bytes += job->engine.setup.bitstream_size;
#endif
        const uint32_t format = 1u << job->plan.target.format;
        const bool first = (job->status == 0) && (gFirstLogged & format) == 0u;
        if (first)
            gFirstLogged |= format;
        IOLockUnlock(gStatsLock);
        if (first) {
            IOLog("TegraNVJPG: first hardware decode done: %ux%u to %ux%u (format %u, 1/%u) in %llu us engine (%u "
                  "cycles), %llu us CPU\n",
                  static_cast<unsigned>(job->engine.setup.frame_width),
                  static_cast<unsigned>(job->engine.setup.frame_height), job->plan.extent.width,
                  job->plan.extent.height, job->plan.target.format, 1u << job->plan.target.scale, job->engineUs,
                  job->cycles, job->cpuUs);
        }
#if TEGRA_NVJPG_STATS
        statsLine();
#endif
        finish(job);
    }
}

void queueJob(Job *job)
{
    IOLockLock(gQueueLock);
    while (gQueued >= kQueueMax)
        IOLockSleep(gQueueLock, &gQueued, THREAD_UNINT);
    job->next = nullptr;
    if (gTail) {
        gTail->next = job;
    } else {
        gHead = job;
    }
    gTail = job;
    gQueued++;
    IOLockWakeup(gQueueLock, &gHead, false);
    IOLockUnlock(gQueueLock);
}

// The source: wired for the copy, its headers parsed from a first chunk, the scan copied into a kernel buffer whose
// pages the input window maps (EOI appended when the file has none). The scan runs from the SOS header to EOI or the
// data's end (the source's size may be its whole allocation: trailing bytes are cut at EOI).
const char *readSource(TegraVICSurface *src, uint64_t size, TegraNVJPGImage *img, Job *job, IOReturn *ret)
{
    *ret = kIOReturnBadArgument;
    if (size < 4 || size > src->desc.length)
        return "source size";
    const uint32_t pageCount = static_cast<uint32_t>(switch_div_round_up(size, SWITCH_HW_PAGE_SIZE));
    uint64_t *pages = IONewData(uint64_t, pageCount);
    if (!pages) {
        *ret = kIOReturnNoMemory;
        return "no memory";
    }
    uint32_t count = 0;
    *ret = tegra_vic_surface_prepare(src, size, pages, pageCount, &count);
    IODeleteData(pages, uint64_t, pageCount);
    if (*ret == kIOReturnNotAligned)
        *ret = kIOReturnUnsupported;  // never NotAligned: CMPhoto would copy and retry
    if (*ret)
        return "source not wired";
    const char *why = nullptr;
    uint32_t chunk = static_cast<uint32_t>(size < kHeaderChunk ? size : kHeaderChunk);
    uint8_t *head = nullptr;
    for (;;) {
        head = IONewData(uint8_t, chunk);
        if (!head || src->memory->readBytes(0, head, chunk) != chunk) {
            *ret = kIOReturnNoMemory;
            why = "source read";
            break;
        }
        why = tegra_nvjpg_parse_headers(head, chunk, img);
        if (!why || chunk == size || chunk >= kHeaderMax)
            break;
        IODeleteData(head, uint8_t, chunk);
        head = nullptr;
        // headers past the first chunk: read further
        chunk = static_cast<uint32_t>(size < kHeaderMax ? size : kHeaderMax);
    }
    if (head)
        IODeleteData(head, uint8_t, chunk);
    if (why) {
        if (*ret != kIOReturnNoMemory)
            *ret = kIOReturnUnsupported;
        tegra_vic_surface_complete(src);
        return why;
    }
    const uint64_t scan = size - img->scanOffset;
    if (scan + 2 > kTegraNVJPGWindowBytes[TEGRA_NVJPG_WINDOW_IN] - SWITCH_HW_PAGE_SIZE) {
        tegra_vic_surface_complete(src);
        *ret = kIOReturnUnsupported;
        return "scan larger than the 64 MiB input window";
    }
    job->bits =
        IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionOut, scan + 2, SWITCH_HW_PAGE_SIZE);
    if (!job->bits || job->bits->prepare() != kIOReturnSuccess ||
        src->memory->readBytes(img->scanOffset, job->bits->getBytesNoCopy(), scan) != scan) {
        tegra_vic_surface_complete(src);
        *ret = kIOReturnNoMemory;
        return "scan copy";
    }
    tegra_vic_surface_complete(src);
    uint8_t *bytes = static_cast<uint8_t *>(job->bits->getBytesNoCopy());
    TegraNVJPGImage scanned = *img;
    scanned.scanOffset = 0;
    why = tegra_nvjpg_scan_end(bytes, scan, &scanned);
    if (why) {
        *ret = kIOReturnUnsupported;
        return why;
    }
    uint32_t used = scanned.scanEnd;
    if (!scanned.eoi)
        bytes[used++] = 0xff, bytes[used++] = 0xd9;
    switch_dcache_clean(bytes, used);
    img->scanEnd = img->scanOffset + scanned.scanEnd;
    img->eoi = scanned.eoi;
    job->engine.setup.bitstream_size = static_cast<int>(used);
    job->bitPages = static_cast<uint32_t>(switch_div_round_up(used, SWITCH_HW_PAGE_SIZE));
    *ret = kIOReturnSuccess;
    return nullptr;
}

TegraNVJPGAbility ability()
{
    const TegraNVJPGTraits *t = tegra_nvjpg_traits();
    return TegraNVJPGAbility{t->formats, t->chroma, t->scales, t->clipped ? 1u : 0u, t->greyRgb ? 1u : 0u};
}
}  // namespace

class AppleJPEGDriverUserClient : public IOUserClient {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(AppleJPEGDriverUserClient)

        IOLock *lock_ = nullptr;
    task_t task_ = nullptr;
    mach_port_t port_ = MACH_PORT_NULL;
    uint32_t pending_ = 0;
    bool counted_ = false, powerEntitled_ = false;

    IOReturn decode(const void *in, uint32_t inSize, void *out, uint32_t *outSize, RequestKind kind);

public:
    bool setUp(task_t task);
    void finished(Job *job);
    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleJPEGDriverUserClient, IOUserClient)

    namespace
{
    void finish(Job * job)
    {
        job->client->finished(job);
    }
}  // namespace

bool AppleJPEGDriverUserClient::setUp(task_t task)
{
    task_ = task;
    counted_ = true;
    // Apple's client (init 0x52d1c) reads this entitlement for selectors 8 and 9 only.
    const OSObject *entitled = IOUserClient::copyClientEntitlement(task, "com.apple.applejpegdriver.poweron");
    powerEntitled_ = entitled == kOSBooleanTrue;
    OSSafeReleaseNULL(entitled);
    return (lock_ = IOLockAlloc()) != nullptr;
}

// Engine thread: a job is done. Async: the completion to the registered port; sync: wake the caller.
void AppleJPEGDriverUserClient::finished(Job *job)
{
    if (job->async) {
        io_user_reference_t argument = job->context;
        IOUserClient::sendAsyncResult64(job->ref, job->status, &argument, 1);
        dropJob(job);
        IOLockLock(lock_);
        pending_--;
        IOLockWakeup(lock_, &pending_, false);
        IOLockUnlock(lock_);
        release();
        return;
    }
    IOLockLock(lock_);
    job->done = true;
    IOLockWakeup(lock_, job, false);
    IOLockUnlock(lock_);
}

IOReturn AppleJPEGDriverUserClient::decode(const void *in, uint32_t inSize, void *out, uint32_t *outSize,
                                           RequestKind kind)
{
    const uint64_t began = switch_now_us();
    Job *job = IONewZero(Job, 1);
    if (!job)
        return kIOReturnNoMemory;
    memcpy(&job->request, in, inSize);
    const auto *req = &job->request;
    const TegraNVJPGDecodeRequest &r = req->base;
    IOReturn ret = kIOReturnSuccess;
    const char *why = nullptr;
    TegraVICSurface src = {};
    TegraNVJPGImage img;
    if ((kind == kSelector5 && req->ext.crop) || (kind == kSelector7 && req->v2024.crop)) {
        ret = kIOReturnUnsupported;
        why = "cropped decode (crop rectangle)";
    }
    if (!ret && kind == kSelector7 &&
        (req->v2024.header_bytes || req->v2024.strip_index || req->v2024.strip_entropy_bytes)) {
        ret = kIOReturnUnsupported;
        why = "strip decode (the service publishes one core)";
    }
    // Surfaces in the caller's task, as Apple's driver looks them up (not the task that opened the connection).
    if (!ret && (tegra_vic_surface_lookup(r.src_id, current_task(), &src) ||
                 tegra_vic_surface_lookup(r.dst_id, current_task(), &job->dst))) {
        ret = kIOReturnBadArgument;
        why = "surface ID not found for this task";
    }
    if (!ret && tegra_gm20b_surface_engine_rows) {
        tegra_gm20b_surface_engine_rows(src.surface);
        tegra_gm20b_surface_engine_rows(job->dst.surface);
    }
    if (!ret && src.desc.protection) {
        ret = kIOReturnNotPermitted;  // Apple's 0xe00002e2 for a protected source
        why = "protected source";
    }
    if (!ret) {
        uint64_t size = r.src_size;
        if (src.desc.length < size)
            size = src.desc.length;
        why = readSource(&src, size, &img, job, &ret);
    }
    tegra_vic_surface_release(&src);
    if (!ret && job->dst.desc.length + kSinkBytes > kTegraNVJPGWindowBytes[TEGRA_NVJPG_WINDOW_OUT]) {
        ret = kIOReturnUnsupported;
        why = "destination larger than the 255 MiB output window";
    }
    if (!ret) {
        TegraNVJPGSurface d = {};
        const TegraVICSurfaceDesc &v = job->dst.desc;
        d.fourcc = v.fourcc, d.width = v.width, d.height = v.height, d.planes = v.planes;
        for (uint32_t i = 0; i < 2; i++) {
            d.planeOffset[i] = v.planeOffset[i];
            d.planeBytesPerRow[i] = v.planeBytesPerRow[i];
            d.planeHeight[i] = v.planeHeight[i];
        }
        d.length = v.length;
        d.protection = v.protection;
        const TegraNVJPGAbility a = ability();
        uint32_t kind = 0;
        why = tegra_nvjpg_plan(&r, &img, &d, &a, kSinkBytes, &job->plan, &kind);
        if (why)
            ret = kind == TEGRA_NVJPG_PLAN_UNSUPPORTED ? kIOReturnUnsupported : kIOReturnBadArgument;
    }
    if (!ret) {
        const int bytes = job->engine.setup.bitstream_size;
        tegra_nvjpg_setup_image(&img, static_cast<uint32_t>(bytes), &job->engine.setup);
        const char *outWhy = tegra_nvjpg_setup_output(&img, &job->plan.target, ability().clipped == 0,
                                                      &job->engine.setup, &job->plan.extent);
        if (outWhy) {
            ret = kIOReturnUnsupported;
            why = outWhy;
        }
        job->engine.planes = job->plan.planes;
        job->engine.pixels = static_cast<uint64_t>(img.mcusX) * img.mcuW * img.mcusY * img.mcuH;
        job->engine.selector = kind == kSelector1 ? 1 : kind == kSelector5 ? 5 : 7;
        job->engine.target = job->plan.target.format;
    }
    if (ret) {
        if (why)
            refused(why);
#if TEGRA_NVJPG_STATS
        statsLine();
#endif
        dropJob(job);
        return ret;
    }
    job->cpuUs = switch_now_us() - began;
    job->pid = proc_selfpid();
    job->async = r.callback != 0;
    job->context = r.context;
    IOLockLock(lock_);
    const mach_port_t port = port_;
    pending_++;
    IOLockUnlock(lock_);
    if (job->async) {
        IOUserClient::setAsyncReference64(job->ref, port, r.callback, r.refcon, task_);
        retain();  // the job holds the connection until its completion is sent
    }
    job->client = this;
    const bool async = job->async;
    queueJob(job);
    if (async)
        return kIOReturnSuccess;  // the job is the engine thread's now
    IOLockLock(lock_);
    while (!job->done)
        IOLockSleep(lock_, job, THREAD_UNINT);
    pending_--;
    IOLockWakeup(lock_, &pending_, false);
    IOLockUnlock(lock_);
    ret = job->status;
    // The output: the request back (CMPhoto reads Ext out+0x14/+0x18), with Apple's result words (startDecoder
    // 0x46330: +0x4c 0 for a decode, +0x50/+0x54 codec registers userland does not read); for selector 5 the engine's
    // time and cycles (startDecoderExt: +0x70 microseconds, +0x74 cycles); for selector 7 the region decoded at
    // +0x4c/+0x50 and the time at +0xc94 (startDecoder2024).
    if (out && *outSize >= inSize) {
        memcpy(out, req, inSize);
        uint32_t *words = static_cast<uint32_t *>(out);
        words[0x4c / 4] = kind == kSelector7 ? r.width : 0;
        words[0x50 / 4] = kind == kSelector7 ? r.height : 0;
        words[0x54 / 4] = 0;
        if (kind == kSelector5) {
            words[0x70 / 4] = static_cast<uint32_t>(job->engineUs);
            words[0x74 / 4] = job->cycles;
        } else if (kind == kSelector7) {
            words[0xc94 / 4] = static_cast<uint32_t>(job->engineUs);
        }
        *outSize = inSize;
    }
    dropJob(job);
    return ret;
}

IOReturn AppleJPEGDriverUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *a,
                                                   IOExternalMethodDispatch *dispatch, OSObject *target,
                                                   void *reference)
{
    (void)dispatch, (void)target, (void)reference;
    if (!a || a->structureInputDescriptor || a->structureOutputDescriptor || a->scalarInputCount ||
        a->scalarOutputCount)
        return kIOReturnBadArgument;
    // Apple's IOExternalMethodDispatch2022 table: exact struct sizes, in == out.
    static const uint32_t kSize[10] = {0, 0x58, 0, 0x58, 0x1000, 0x1000, 0xda0, 0xda0, 0, 0};
    if (selector >= 10)
        return kIOReturnBadArgument;
    if (a->structureInputSize != kSize[selector] ||
        (a->structureOutputSize != kSize[selector] && a->structureOutputSize != 0))
        return kIOReturnBadArgument;
    switch (selector) {
    case 0:
    case 2:
        return kIOReturnSuccess;
    case 1:
    case 5:
    case 7: {
#if TEGRA_NVJPG_STATS
        const auto *route = static_cast<const TegraNVJPGDecodeRequest2024 *>(a->structureInput);
        if (selector == 7 && route->rsvdcac == kRouteMagic && !route->base.src_id) {
            if (a->structureOutputSize != kSize[7])
                return kIOReturnBadArgument;
            auto *out = static_cast<uint64_t *>(a->structureOutput);
            bzero(out, kSize[7]);
            IOLockLock(gStatsLock);
            countsOf(proc_selfpid(), &out[0], &out[1]);
            IOLockUnlock(gStatsLock);
            return kIOReturnSuccess;
        }
#endif
        if (!tegra_nvjpg_ready())
            return kIOReturnNotReady;
        uint32_t outSize = a->structureOutputSize;
        const RequestKind kind = selector == 1 ? kSelector1 : selector == 5 ? kSelector5 : kSelector7;
        const IOReturn ret =
            decode(a->structureInput, a->structureInputSize, outSize ? a->structureOutput : nullptr, &outSize, kind);
        a->structureOutputSize = outSize;
        return ret;
    }
    case 8:
    case 9:
        return powerEntitled_ ? kIOReturnSuccess : kIOReturnError;
    default:
        return kIOReturnUnsupported;  // 3, 4, 6 encode
    }
}

// Apple's (0x531f4): the port is kept whatever the type; a second, different port is refused.
IOReturn AppleJPEGDriverUserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
    (void)type, (void)refCon;
    IOLockLock(lock_);
    const bool busy = port_ != MACH_PORT_NULL && port_ != port;
    const bool kept = port_ == port;
    if (!busy)
        port_ = port;
    IOLockUnlock(lock_);
    // A refused port's right is the MIG server's to destroy: it does so when this returns an error.
    if (busy)
        return kIOReturnError;
    // Each registration brings its own right: a second one to the port already kept is released here
    if (kept)
        IOUserClient::releaseNotificationPort(port);
    return kIOReturnSuccess;
}

// Queued decodes hold the connection; closing waits for them (each is bounded by the engine), so no completion
// outlives the port it answers on. Sync callers close right after their call returns.
IOReturn AppleJPEGDriverUserClient::clientClose()
{
    IOLockLock(lock_);
    while (pending_)
        IOLockSleep(lock_, &pending_, THREAD_UNINT);
    IOLockUnlock(lock_);
    terminate();
    return kIOReturnSuccess;
}

void AppleJPEGDriverUserClient::free()
{
    if (port_ != MACH_PORT_NULL)
        IOUserClient::releaseNotificationPort(port_);
    port_ = MACH_PORT_NULL;
    if (lock_)
        IOLockFree(lock_);
    lock_ = nullptr;
    if (counted_)
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
    IOUserClient::free();
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage (its kalloc type view)
class AppleJPEGDriver : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(AppleJPEGDriver)

    public : IOReturn newUserClient(task_t task, void *securityID, UInt32 type, OSDictionary *properties,
                                    IOUserClient **handler) APPLE_KEXT_OVERRIDE;

    IOReturn setProperties(OSObject *properties) APPLE_KEXT_OVERRIDE
    {
        (void)properties;
        return kIOReturnUnsupported;
    }
};

OSDefineMetaClassAndStructors(AppleJPEGDriver, IOService)

    // Apple's (0x43f08) ignores `type`, securityID and properties and checks no entitlement: any process the sandbox
    // lets open the client gets one, up to 1000 open at a time.
    IOReturn AppleJPEGDriver::newUserClient(task_t task, void *securityID, UInt32 type, OSDictionary *properties,
                                            IOUserClient **handler)
{
    if (!tegra_nvjpg_ready())
        return kIOReturnNotReady;
    if (__atomic_add_fetch(&gConnections, 1, __ATOMIC_SEQ_CST) > kMaxConnections) {
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
        return kIOReturnNoResources;
    }
    AppleJPEGDriverUserClient *client = OSTypeAlloc(AppleJPEGDriverUserClient);
    if (!client || !client->initWithTask(task, securityID, type, properties)) {
        if (client)
            client->release();
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
        return kIOReturnNoMemory;
    }
    if (!client->setUp(task) || !client->attach(this)) {  // from setUp on, free() gives the slot back
        client->release();
        return kIOReturnNoMemory;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

namespace {
void number(IOService *s, const char *key, uint32_t value)
{
    // 32-bit: JPEGH1 reads AppleJPEGNumCores with size 4
    s->setProperty(key, static_cast<unsigned long long>(value), 32);
}
}  // namespace

extern "C" int tegra_nvjpg_publish(void)
{
    const TegraNVJPGTraits *t = tegra_nvjpg_traits();
    gQueueLock = IOLockAlloc();
    gStatsLock = IOLockAlloc();
    gOutList = IONewData(uint64_t, kOutPages);
    gInList = IONewData(uint64_t, kInPages);
    if (!gQueueLock || !gStatsLock || !gOutList || !gInList || tegra_nvjpg_buffer(SWITCH_HW_PAGE_SIZE, &gSink) ||
        !tegra_vic_surface_start()) {
        IOLog("TegraNVJPG: J3 FAIL no memory for the service, or no IOCoreSurfaceRoot\n");
        return -12;
    }
    thread_t thread = nullptr;
    if (kernel_thread_start(engineThread, nullptr, &thread) != KERN_SUCCESS) {
        IOLog("TegraNVJPG: J3 FAIL could not start the engine thread\n");
        return -12;
    }
    thread_deallocate(thread);
    IOService *resources = IOService::getResourceService();
    AppleJPEGDriver *service = OSTypeAlloc(AppleJPEGDriver);
    if (!resources || !service || !service->init()) {
        OSSafeReleaseNULL(service);
        IOLog("TegraNVJPG: J3 FAIL could not create the service\n");
        return -12;
    }
    // Apple's properties (start 0x433b8), as NVJPG has them: one core (CMPhoto's two-core strip decode needs two and
    // RST logging), DCT scaling as NVJPG decodes it, missing EOI (the copy appends one), the "interchange" formats (BGRA, RGBA
    // and 4:2:2/4:4:4 input; CMPhoto's automatic mode, which ImageIO uses, takes the hardware only with them), no
    // 12-bit or compressed formats, no MCU alignment requirement (read by the encoder only).
    const bool rgb =
        ((t->formats >> TEGRA_NVJPG_OUT_BGRA & 1) != 0u) && ((t->formats >> TEGRA_NVJPG_OUT_RGBA & 1) != 0u);
    number(service, "AppleJPEGNumCores", 1);
    number(service, "AppleJPEGRequiresMCUAlignment", 0);
    number(service, "AppleJPEGSupportsDCTScaling", t->scales == 0xf ? 1 : 0);
    number(service, "AppleJPEGSupportsRSTLogging", 0);
    number(service, "AppleJPEGSupportsMissingEOI", 1);
    number(service, "AppleJPEGSupportsAppleInterchangeFormats", rgb ? 1 : 0);
    number(service, "AppleJPEGSupports12BitsFormat", 0);
    number(service, "AppleJPEGSupportsCompressedInterchangeFormats", 0);
    number(service, "TegraNVJPGFormats", t->formats);
    number(service, "TegraNVJPGSampling", t->chroma);
    number(service, "TegraNVJPGScales", t->scales);
    service->setProperty("TegraNVJPGWritesClipped", t->clipped);
    service->setProperty("IOUserClientClass", "AppleJPEGDriverUserClient");
    service->setName("TegraNVJPG");  // not "AppleJPEGDriver": JPEGH1.videoencoder name-matches it (see the top)
    if (!service->attach(resources)) {
        service->release();
        IOLog("TegraNVJPG: J3 FAIL could not attach the service\n");
        return -12;
    }
    if (!service->start(resources)) {
        service->detach(resources);
        service->release();
        IOLog("TegraNVJPG: J3 FAIL could not start the service\n");
        return -12;
    }
    service->registerService();
    IOLog("TegraNVJPG: J3 ok AppleJPEGDriver published (user client AppleJPEGDriverUserClient; formats %#x, sampling "
          "%#x, scales %#x, interchange formats %u, writes %s)\n",
          t->formats, t->chroma, t->scales, rgb ? 1u : 0u, t->clipped ? "clipped" : "whole MCUs");
    service->release();  // the attachment keeps it
    return 0;
}
