// SPDX-License-Identifier: BSD-2-Clause-Patent
// V3: the IOSurfaceAccelerator service. IOSurfaceAccelerator.framework (Core Animation's display clone for Screen
// Recording and AirPlay, VideoToolbox's pixel transfers, ImageIO, WebKit, ...) opens IOServiceMatching(
// "AppleM2ScalerCSCDriver") and talks to its IOSurfaceAcceleratorClient; this puts VIC behind that pair. The contract
// (drivers/TegraVIC/CONTRACT.md) was read from 21H461: the framework's request builder, Apple's driver (sizes, sync and
// async model, rectangles, completion) and its callers.
//
// Apple's classes, not look-alikes: com.apple.driver.AppleM2ScalerCSCDriver is prelinked here (IOMobileGraphicsFamily
// links against it, so it loads with the display), and its classes own the names AppleM2ScalerCSCDriver and
// IOSurfaceAcceleratorClient: a second class of either name would make that kext, and the display family with it, fail
// to load. Matching and the sandbox go by those names, so the service is an instance of Apple's AppleM2ScalerCSCDriver
// and each connection an instance of Apple's IOSurfaceAcceleratorClient (OSMetaClass::allocClassWithName), each
// pointed at a copy of its class's vtable in which the slots that would reach Apple's scaler hardware are ours or the
// IOService/IOUserClient defaults (never start/probe/stop/setPowerState/setProperties of the real driver; never its
// client's method table, port, close or free). Nothing of Apple's driver runs but its constructors and getMetaClass.
// The slots are found by the IOService/IOUserClient functions they hold in the kernel's own vtables; that each one the
// copy replaces holds an override of Apple's is kits/SwitchABI's AppleM2ScalerCSCDriver and IOSurfaceAcceleratorClient
// tables, checked against each IPSW kind's kernelcache by //kits/SwitchABI:verify.
//
//   sel 1  transform (0x1b0 bytes in): surfaces looked up by ID in the caller's task, the request translated and every
//          job checked before the call returns (TegraVICRequest.h, TegraVICConfig.h); a refusal is the call's result
//          (kIOReturnUnsupported for what VIC does not do, kIOReturnBadArgument for a bad request), so callers fall
//          back at once. Accepted requests are queued for the engine thread in order. With no callback the call
//          sleeps until its jobs are done and returns their status; with one it returns at once and the result comes
//          back as an IOKit async completion on the port registered with IOConnectSetNotificationPort, with one
//          argument, the request's third completion word (Core Animation's msr_transfer_complete reads it), unless
//          SkipAsyncCallback is set.
//   sel 5  GetID; sel 10 client properties (priority, duty cycle: kept, unused); sel 4 custom filter (accepted, VIC
//          scales with its own 2-tap filter); sel 0, 2, 3, 7, 8, 9 kIOReturnUnsupported as on Apple's hardware or for
//          features VIC lacks (histogram, diagnostics, estimation); sel 6 not permitted.
//
// Per job (engine thread, one at a time): both surfaces wired (IOSurface::prepare) and mapped page by page into VIC's
// two SMMU windows (the source read-only), the source's lines cleaned and the destination's cleaned and invalidated,
// the jobs run (each completed by its syncpoint interrupt), the destination invalidated again for CPU readers, the
// windows emptied and the surfaces completed and released. The cache maintenance is the only CPU cost (about 80 us
// per MiB) and it is paid by the scaler's callers only: nothing on the normal display path uses this service.
//
// Present (TegraVICPresent.cpp): TegraNVDEC's decoders hand each decoded picture to VIC on their own thread, under the
// same engine lock.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/OSKextLib.h>
#include <os/log.h>
#include <string.h>
#include "TegraVIC.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
#include "../../kits/SwitchTime/SwitchTime.h"
#include "TegraVICBounce.h"
#include "TegraVICBounceEngine.h"
#include "TegraVICPresent.h"
#include "TegraVICSurface.h"
#include "../TegraGM20B/TegraGM20BLayout.h"

// The kernel's IOService/IOUserClient implementations, called directly (they are the defaults the vtable copies
// keep or fall back to), and their vtables, where the slots are found.
extern "C" {
extern const uintptr_t tegra_vic_ioservice_vtable[] __asm__("__ZTV9IOService");
extern const uintptr_t tegra_vic_iouserclient_vtable[] __asm__("__ZTV12IOUserClient");
IOReturn
ios_new_user_client(IOService *, task_t, void *, UInt32, OSDictionary *,
                    IOUserClient **) __asm__("__ZN9IOService13newUserClientEP4taskPvjP12OSDictionaryPP12IOUserClient");
bool ios_start(IOService *, IOService *) __asm__("__ZN9IOService5startEPS_");
void ios_stop(IOService *, IOService *) __asm__("__ZN9IOService4stopEPS_");
IOService *ios_probe(IOService *, IOService *, SInt32 *) __asm__("__ZN9IOService5probeEPS_Pi");
IOReturn ios_set_power_state(IOService *, unsigned long, IOService *) __asm__("__ZN9IOService13setPowerStateEmPS_");
IOReturn ire_set_properties(IORegistryEntry *, OSObject *) __asm__("__ZN15IORegistryEntry13setPropertiesEP8OSObject");
void iouc_free(IOUserClient *) __asm__("__ZN12IOUserClient4freeEv");
IOReturn iouc_client_close(IOUserClient *) __asm__("__ZN12IOUserClient11clientCloseEv");
IOReturn iouc_external_method(IOUserClient *, uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *,
                              OSObject *, void *) __asm__("__"
                                                          "ZN12IOUserClient14externalMethodEjP25IOExternalMethodArgumen"
                                                          "tsP24IOExternalMethodDispatchP8OSObjectPv");
IOReturn iouc_register_port(IOUserClient *, mach_port_t, UInt32,
                            UInt32) __asm__("__ZN12IOUserClient24registerNotificationPortEP8ipc_portjj");
IOReturn
iouc_client_memory(IOUserClient *, UInt32, IOOptionBits *,
                   IOMemoryDescriptor **) __asm__("__ZN12IOUserClient19clientMemoryForTypeEjPjPP18IOMemoryDescriptor");
IOExternalMethod *
iouc_target_and_method(IOUserClient *, IOService **,
                       UInt32) __asm__("__ZN12IOUserClient26getTargetAndMethodForIndexEPP9IOServicej");
IOReturn iouc_send_async_result64(OSAsyncReference64, IOReturn, io_user_reference_t *,
                                  UInt32) __asm__("__ZN12IOUserClient17sendAsyncResult64EPyiS0_j");
void iouc_set_async_reference64(OSAsyncReference64, mach_port_t, mach_vm_address_t, io_user_reference_t,
                                task_t) __asm__("__ZN12IOUserClient19setAsyncReference64EPyP8ipc_portyyP4task");
void iouc_release_port(mach_port_t) __asm__("__ZN12IOUserClient23releaseNotificationPortEP8ipc_port");
}

namespace {
constexpr uint32_t kQueueMax = 16, kMaxConnections = 64;
constexpr uint32_t kWindowPages = static_cast<uint32_t>(kTegraVICWindowBytes >> 12);
constexpr uint32_t kVtableSlots = 200;  // both classes have under 192; copying a little more reads only data

// One connection's state, beside Apple's IOSurfaceAcceleratorClient object it belongs to.
struct Conn {
    IOUserClient *client;  // not retained: set in newUserClient, cleared in its free
    task_t task;
    mach_port_t port;  // registered with IOConnectSetNotificationPort; queued replies carry it, a replaced one's
                       // replies fail harmlessly (the port right is released when replaced)
    IOLock *lock;
    uint32_t pending;  // jobs queued or running
};

Conn gConns[kMaxConnections];
IOLock *gConnLock;

Conn *connOf(const IOUserClient *client)
{
    Conn *found = nullptr;
    IOLockLock(gConnLock);
    for (uint32_t i = 0; i < kMaxConnections && !found; i++) {
        if (gConns[i].client == client)
            found = &gConns[i];
    }
    IOLockUnlock(gConnLock);
    return found;
}

// The vtable copies (their address point is index 2 of each array).
uintptr_t gServiceVtable[kVtableSlots], gClientVtable[kVtableSlots];

int slotOf(const uintptr_t *vtable, const void *function)
{
    for (int i = 0; i < 192; i++) {
        if (vtable[2 + i] == reinterpret_cast<uintptr_t>(function))
            return i;
    }
    return -1;
}

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

struct Job;
IOLock *gQueueLock;
Job *gHead, *gTail;
uint32_t gQueued;
uint64_t *gPages[TEGRA_VIC_WINDOWS];

#if TEGRA_VIC_STATS
// Stats for the unified log (one line per 5 s window with work). `pitch`: refusals for a row pitch VIC cannot address,
// which CoreVideo's buffers sized by the display pipe's stride requirement never have.
struct {
    uint64_t windowStart, jobs, ops, refused, pitch, failed, engineUs, mapUs, bytes;
} gStats;
#endif

// `why` is a literal: the refusal's key as well as its text.
void refused(IOReturn ret, const char *why)
{
#if TEGRA_VIC_STATS
    __atomic_add_fetch(&gStats.refused, 1, __ATOMIC_RELAXED);
    if (!strncmp(why, "row pitch", 9))
        __atomic_add_fetch(&gStats.pitch, 1, __ATOMIC_RELAXED);
#endif
    const char *kind = "surface lookup";
    if (ret == kIOReturnUnsupported) {
        kind = "not on VIC";
    } else if (ret == kIOReturnBadArgument) {
        kind = "bad request";
    }
    switch_refusal("TegraVIC", why, "IOSurfaceAccelerator transform refused (%s), the caller falls back", kind);
}

#if TEGRA_VIC_STATS
extern "C" void proc_selfname(char *buf, int size);
extern "C" int proc_selfpid(void);

// Who allocates surfaces VIC cannot address, once a boot: each one costs the copy engine two copies a job, and the
// fix is that allocator's row alignment.
void firstBounce(const TegraVICSurfaceDesc *d, const char *side)
{
    static int logged;
    if (__atomic_exchange_n(&logged, 1, __ATOMIC_RELAXED))
        return;
    char name[32];
    proc_selfname(name, sizeof(name));
    os_log(OS_LOG_DEFAULT,
           "TegraVIC: first bounce this boot: %s[%d] %s '%c%c%c%c' %ux%u, row pitch %llu (VIC takes multiples of 256)",
           name, proc_selfpid(), side, static_cast<char>(d->fourcc >> 24), static_cast<char>(d->fourcc >> 16),
           static_cast<char>(d->fourcc >> 8), static_cast<char>(d->fourcc), d->width, d->height,
           static_cast<unsigned long long>(d->planeBytesPerRow[0]));
}
#endif

struct Job {
    Job *next;
    IOUserClient *client;  // retained
    struct Conn *conn;
    TegraVICSurface src, dst;
    TegraVICPlan plan;
    uint32_t chromaSite;
    bool bounceSrc, bounceDst;  // through the copy engine's scratch (TegraVICBounce.h)
    TegraVICBounce *backFrom;   // a destination's copy back in flight: its scratch, and the destination's pages
    uint64_t *dstPages;
    uint32_t dstPageCount;
    bool async, reply;
    OSAsyncReference64 ref;
    uint64_t info;
    IOReturn status;
    bool done;
    TegraVICJob check;  // the submitting thread's check of each operation
};

// The pages the window maps for a side, and the geometry VIC reads them with: the surface's own, or its bounce scratch.
struct Side {
    uint64_t *pages;
    uint32_t count;
    const TegraVICSurfaceDesc *desc;
};

Side sideOf(const TegraVICSurface *s, bool bounced, const TegraVICBounce *b, uint64_t *pages, uint32_t count)
{
    return bounced ? Side{b->scratch.pages, b->pages, &b->desc} : Side{pages, count, &s->desc};
}

uint32_t pitchAlign()
{
    return tegra_vic_traits()->pitchAlign ? tegra_vic_traits()->pitchAlign : 256;
}

// How VIC will see a surface: as it is, or (rows it cannot address) as its bounce scratch.
const char *staged(const TegraVICSurfaceDesc *d, TegraVICSurfaceDesc *stage, const TegraVICSurfaceDesc **as,
                   bool *bounced)
{
    const uint32_t align = pitchAlign();
    const int needed = tegra_vic_bounce_needed(d, align);
    *bounced = needed == 0;
    if (needed < 0)
        return tegra_vic_format(d->fourcc) ? "plane offset the copy engine cannot take" : nullptr;
    if (*bounced) {
        tegra_vic_bounce_desc(d, align, stage);
        *as = stage;
    }
    return nullptr;
}

void finishJob(Job *job, IOReturn status);

// A destination's copy back has landed (the copy engine's completion, a thread call): the job finishes here.
void copiedBack(void *arg, int err)
{
    Job *job = static_cast<Job *>(arg);
    tegra_vic_bounce_back_landed(job->backFrom);
    // The CPU may have pulled destination lines in while the engines wrote: readers must see DRAM.
    switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, job->dstPages, job->dstPageCount);
    switch_dcache_sync();
    IODelete(job->dstPages, uint64_t, job->dstPageCount);
    job->dstPages = nullptr;
    tegra_vic_surface_complete(&job->dst);
    tegra_vic_surface_complete(&job->src);
    finishJob(job, err ? kIOReturnIOError : kIOReturnSuccess);
}

// Engine thread side: one job, all its operations, under the engine lock. A side whose rows VIC cannot address goes
// through its bounce scratch, and the engines order each other: the copy engine fills the scratches (a destination's
// with its own pixels, for those VIC leaves), VIC's channel waits for the fills on the hardware, and a destination's
// copy back waits on the hardware for VIC's fence. The thread never waits for a copy: a job with a copy back is
// finished by that copy's completion (false), any other here (true, *status).
bool runJob(Job *job, IOReturn *status)
{
    static TegraVICJob vj;
    static TegraVICBounce bounce[TEGRA_VIC_WINDOWS];
    [[maybe_unused]] const uint64_t t0 = switch_now_us();
    uint32_t pages[TEGRA_VIC_WINDOWS] = {};
    TegraVICImage dst;
    TegraVICImage src;
    const char *why = nullptr;
    const bool readsSource = job->plan.readsSource != 0u;
    const uint32_t align = pitchAlign();
    IOReturn ret = tegra_vic_surface_prepare(&job->dst, tegra_vic_surface_extent(&job->dst.desc),
                                             gPages[TEGRA_VIC_WINDOW_DST], kWindowPages, &pages[1]);
    if (!ret && readsSource) {
        ret = tegra_vic_surface_prepare(&job->src, tegra_vic_surface_extent(&job->src.desc),
                                        gPages[TEGRA_VIC_WINDOW_SRC], kWindowPages, &pages[0]);
    }
    if (ret) {
        *status = ret;
        return true;
    }
    // A GPU-rendered surface may be block-linear (TegraGM20BLayout.h): the copy engine reads one as its rows into the
    // scratch, and writes the scratch's rows back into one's texels.
    tgm_layout layout;
    const bool bounceSrc =
        readsSource && (job->bounceSrc || tegra_gm20b_surface_layout(gPages[TEGRA_VIC_WINDOW_SRC][0], &layout) == 1);
    const bool bounceDst = job->bounceDst || tegra_gm20b_surface_layout(gPages[TEGRA_VIC_WINDOW_DST][0], &layout) == 1;
    // VIC and the copy engine do not snoop the CPU's caches: what the CPU wrote to the source must be in DRAM, and no
    // dirty line of the destination may be written back over what they write.
    if (readsSource)
        switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN, gPages[TEGRA_VIC_WINDOW_SRC], pages[0]);
    switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, gPages[TEGRA_VIC_WINDOW_DST], pages[1]);
    switch_dcache_sync();
    TegraVICBounce *srcBounce = &bounce[TEGRA_VIC_WINDOW_SRC];
    TegraVICBounce *dstBounce = &bounce[TEGRA_VIC_WINDOW_DST];
    TegraVICBounceOrder order = {};
    if (bounceSrc || bounceDst) {
        ret = tegra_vic_bounce_start();
        uint32_t next = *tegra_vic_bounce_next();
        order = tegra_vic_bounce_order(static_cast<int>(bounceSrc), static_cast<int>(bounceDst), &next);
    }
    if (!ret && bounceSrc) {
        ret = tegra_vic_bounce_stage(srcBounce, &job->src.desc, align);
        if (!ret) {
            ret = tegra_vic_bounce_queue(job->src.memory, &job->src.desc, srcBounce->scratch.memory, &srcBounce->desc,
                                         nullptr, !bounceDst, nullptr, nullptr);
        }
    }
    if (!ret && bounceDst) {
        ret = tegra_vic_bounce_stage(dstBounce, &job->dst.desc, align);
        if (!ret) {
            ret = tegra_vic_bounce_queue(job->dst.memory, &job->dst.desc, dstBounce->scratch.memory, &dstBounce->desc,
                                         nullptr, true, nullptr, nullptr);
        }
    }
    if (!ret && order.vicWaits)
        *tegra_vic_bounce_next() = order.vicAcquire;
    const Side dstSide = sideOf(&job->dst, bounceDst, dstBounce, gPages[TEGRA_VIC_WINDOW_DST], pages[1]);
    const Side srcSide = sideOf(&job->src, bounceSrc, srcBounce, gPages[TEGRA_VIC_WINDOW_SRC], pages[0]);
    tegra_vic_lock();
    if (!ret) {
        const uint64_t dstBase = tegra_vic_window_map(TEGRA_VIC_WINDOW_DST, dstSide.pages, dstSide.count, true);
        const uint64_t srcBase =
            readsSource ? tegra_vic_window_map(TEGRA_VIC_WINDOW_SRC, srcSide.pages, srcSide.count, false) : 0;
        why = tegra_vic_image_from(dstSide.desc, dstBase, &dst);
        dst.chromaSite = job->chromaSite;
        if (!why && readsSource)
            why = tegra_vic_image_from(srcSide.desc, srcBase, &src);
        if (why)
            ret = kIOReturnBadArgument;
    }
    [[maybe_unused]] const uint64_t t1 = switch_now_us();
    [[maybe_unused]] uint64_t busy = 0;
    const TegraVICAcquire fills = {order.vicWaits ? tegra_vic_bounce_syncpt() : 0, order.vicAcquire};
    uint32_t fence = 0;
    for (uint32_t i = 0; !ret && i < job->plan.count; i++) {
        const TegraVICOp *op = &job->plan.op[i];
        why = tegra_vic_build(op->fill ? nullptr : &src, &dst, op, tegra_vic_traits(), tegra_vic_config_address(),
                              tegra_vic_filter_address(), &vj);
        if (why) {
            ret = kIOReturnBadArgument;
            break;
        }
        uint64_t us = 0;
        ret = ioreturn(tegra_vic_run_after(&vj, i == 0 && order.vicWaits ? &fills : nullptr, &us, &fence));
        busy += us;
#if TEGRA_VIC_STATS
        __atomic_add_fetch(&gStats.ops, 1, __ATOMIC_RELAXED);
#endif
    }
    tegra_vic_window_clear(TEGRA_VIC_WINDOW_DST);
    tegra_vic_window_clear(TEGRA_VIC_WINDOW_SRC);
    tegra_vic_unlock();
#if TEGRA_VIC_STATS
    gStats.engineUs += busy;
    gStats.mapUs += (t1 - t0) + (switch_now_us() - t1 - busy);
    gStats.bytes += (static_cast<uint64_t>(pages[0]) + pages[1]) << 12;
#endif
    if (why)
        IOLog("TegraVIC: a checked job was refused on the engine thread: %s\n", why);
    if (!ret && order.copiesBack) {
        job->dstPages = IONew(uint64_t, pages[1]);
        ret = job->dstPages ? kIOReturnSuccess : kIOReturnNoMemory;
    }
    if (!ret && order.copiesBack) {
        memcpy(job->dstPages, gPages[TEGRA_VIC_WINDOW_DST], static_cast<size_t>(pages[1]) * sizeof(uint64_t));
        job->dstPageCount = pages[1];
        job->backFrom = dstBounce;
        const tegra_gm20b_ce_wait afterVic = {tegra_vic_syncpt(), fence};
        tegra_vic_bounce_back_queued(dstBounce);
        ret = tegra_vic_bounce_queue(dstBounce->scratch.memory, &dstBounce->desc, job->dst.memory, &job->dst.desc,
                                     &afterVic, true, copiedBack, job);
        if (!ret) {
            *tegra_vic_bounce_next() = order.done;
            return false;
        }
        tegra_vic_bounce_back_landed(dstBounce);
        IODelete(job->dstPages, uint64_t, pages[1]);
        job->dstPages = nullptr;
    }
    // The CPU may have pulled destination lines in while VIC wrote: readers must see DRAM.
    switch_dcache_hw_pages(SWITCH_DCACHE_CLEAN_INVALIDATE, gPages[TEGRA_VIC_WINDOW_DST], pages[1]);
    switch_dcache_sync();
    tegra_vic_surface_complete(&job->dst);
    tegra_vic_surface_complete(&job->src);
    *status = ret;
    return true;
}

#if TEGRA_VIC_STATS
void statsLine()
{
    const uint64_t now = switch_now_us();
    if (!gStats.windowStart)
        gStats.windowStart = now;
    if (now - gStats.windowStart < 5000000)
        return;
    char line[200];
    snprintf(line, sizeof(line),
             "TegraVIC: %llus jobs=%llu ops=%llu refused=%llu pitch=%llu failed=%llu engine=%lluus/job "
             "cpu=%lluus/job mapped=%lluKB/job",
             (now - gStats.windowStart) / 1000000, gStats.jobs, gStats.ops, gStats.refused, gStats.pitch, gStats.failed,
             gStats.jobs ? gStats.engineUs / gStats.jobs : 0, gStats.jobs ? gStats.mapUs / gStats.jobs : 0,
             gStats.jobs ? (gStats.bytes >> 10) / gStats.jobs : 0);
    os_log(OS_LOG_DEFAULT, "%s", line);
    bzero(&gStats, sizeof(gStats));
    gStats.windowStart = now;
}
#endif

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

        IOReturn status = kIOReturnSuccess;
        if (runJob(job, &status))
            finishJob(job, status);
    }
}

// The caller hears back: the destination's seed moves first (r18-planes: VIC wrote it, or may have in part), then the
// reply or the waiting caller's wakeup. On the engine thread, or a copy back's completion.
void finishJob(Job *job, IOReturn status)
{
#if TEGRA_VIC_STATS
    gStats.jobs++;
    if (status)
        gStats.failed++;
    statsLine();
#endif
    tegra_vic_surface_written(&job->dst);
    tegra_vic_surface_release(&job->src);
    tegra_vic_surface_release(&job->dst);
    const IOUserClient *client = job->client;
    Conn *conn = job->conn;
    if (job->async) {
        if (job->reply) {
            io_user_reference_t argument = job->info;
            iouc_send_async_result64(job->ref, status, &argument, 1);
        }
        IODelete(job, Job, 1);
    } else {
        IOLockLock(conn->lock);
        job->status = status;
        job->done = true;
        IOLockWakeup(conn->lock, job, false);
        IOLockUnlock(conn->lock);
    }
    IOLockLock(conn->lock);
    conn->pending--;
    IOLockWakeup(conn->lock, &conn->pending, false);
    IOLockUnlock(conn->lock);
    client->release();
}

}  // namespace

namespace {
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

IOReturn transform(Conn *conn, const void *in, uint32_t size)
{
    if (size != sizeof(TegraVICRequest))
        return kIOReturnBadArgument;
    if (!tegra_vic_ready())
        return kIOReturnNotReady;
    TegraVICRequest r;
    memcpy(&r, in, sizeof(r));
    Job *job = IONewZero(Job, 1);
    if (!job)
        return kIOReturnNoMemory;
    // Surfaces in the caller's task, as Apple's driver looks them up (not the task that opened the connection).
    IOReturn ret = tegra_vic_surface_lookup(r.dst_id, current_task(), &job->dst);
    const bool writeOnly = (r.flags & TEGRA_VIC_REQ_WRITE_ONLY) != 0;
    if (!ret && !writeOnly)
        ret = tegra_vic_surface_lookup(r.src_id, current_task(), &job->src);
    uint32_t kind = TEGRA_VIC_PLAN_BAD;
    const char *why = ret ? "surface ID not found for this task" : nullptr;
    if (!ret) {
        why = tegra_vic_plan(&r, &job->src.desc, &job->dst.desc, &job->plan, &kind);
        if (why)
            ret = kind == TEGRA_VIC_PLAN_UNSUPPORTED ? kIOReturnUnsupported : kIOReturnBadArgument;
    }
    // Every job checked now, against the surfaces as they will be mapped (window addresses are page-aligned, like the
    // real ones), so a refusal is this call's result and the caller can fall back at once.
    if (!ret) {
        TegraVICImage src = {};
        TegraVICImage dst = {};
        TegraVICSurfaceDesc srcStage;
        TegraVICSurfaceDesc dstStage;
        const TegraVICSurfaceDesc *srcAs = &job->src.desc;
        const TegraVICSurfaceDesc *dstAs = &job->dst.desc;
        why = staged(&job->dst.desc, &dstStage, &dstAs, &job->bounceDst);
        if (!why && job->plan.readsSource)
            why = staged(&job->src.desc, &srcStage, &srcAs, &job->bounceSrc);
#if TEGRA_VIC_STATS
        if (!why && (job->bounceSrc || job->bounceDst))
            firstBounce(job->bounceSrc ? &job->src.desc : &job->dst.desc, job->bounceSrc ? "source" : "destination");
#endif
        if (!why)
            why = tegra_vic_image_from(dstAs, kTegraVICWindowBase + TEGRA_VIC_WINDOW_DST * kTegraVICWindowBytes, &dst);
        if (!why && job->plan.readsSource)
            why = tegra_vic_image_from(srcAs, kTegraVICWindowBase, &src);
        if (!why && (tegra_vic_surface_extent(dstAs) > kTegraVICWindowBytes ||
                     (job->plan.readsSource && tegra_vic_surface_extent(srcAs) > kTegraVICWindowBytes)))
            why = "surface larger than a 128 MiB window";
        dst.chromaSite = TEGRA_VIC_REQ_CHROMA_SITE(r.flags);
        for (uint32_t i = 0; !why && i < job->plan.count; i++) {
            why = tegra_vic_build(job->plan.op[i].fill ? nullptr : &src, &dst, &job->plan.op[i], tegra_vic_traits(),
                                  tegra_vic_config_address(), tegra_vic_filter_address(), &job->check);
        }
        if (why)
            ret = kIOReturnUnsupported;  // a shape VIC does not do (kTegraVICTraits)
    }
    if (ret) {
        refused(ret, why ? why : "surface lookup");
        tegra_vic_surface_release(&job->src);
        tegra_vic_surface_release(&job->dst);
        IODelete(job, Job, 1);
        return ret;
    }
    job->chromaSite = TEGRA_VIC_REQ_CHROMA_SITE(r.flags);
    // Apple's driver: a callback makes the call asynchronous; setAsyncReference64(port, callback, refcon) from the
    // request and one reply argument, cb_info. SkipAsyncCallback, or no registered port, runs it without a reply.
    job->async = r.cb_fn != 0;
    const mach_port_t port = conn->port;
    job->reply = job->async && (job->plan.callback != 0u) && port != MACH_PORT_NULL;
    if (job->reply)
        iouc_set_async_reference64(job->ref, port, r.cb_fn, r.cb_refcon, conn->task);
    job->info = r.cb_info;
    conn->client->retain();
    job->client = conn->client;
    job->conn = conn;
    IOLockLock(conn->lock);
    conn->pending++;
    IOLockUnlock(conn->lock);
    queueJob(job);
    if (job->async)
        return kIOReturnSuccess;
    IOLockLock(conn->lock);
    while (!job->done)
        IOLockSleep(conn->lock, job, THREAD_UNINT);
    IOLockUnlock(conn->lock);
    ret = job->status;
    IODelete(job, Job, 1);
    return ret;
}

// --- The client's vtable slots ------------------------------------------------------------------------------------
IOReturn tegra_vic_external_method(const IOUserClient *self, uint32_t selector, const IOExternalMethodArguments *a,
                                   const IOExternalMethodDispatch *dispatch, const OSObject *target,
                                   const void *reference)
{
    (void)dispatch, (void)target, (void)reference;
    Conn *conn = connOf(self);
    if (!conn || !a || a->structureInputDescriptor || a->structureOutputDescriptor)
        return kIOReturnBadArgument;
    switch (selector) {
    case 1:  // transform
        if (a->scalarInputCount || a->scalarOutputCount || a->structureOutputSize)
            return kIOReturnBadArgument;
        return transform(conn, a->structureInput, a->structureInputSize);
    case 5:  // GetID
        if (a->scalarInputCount || a->scalarOutputCount != 1 || a->structureInputSize)
            return kIOReturnBadArgument;
        a->scalarOutput[0] = 0x5d;  // host1x class of VIC; constant, as Apple's is per driver
        return kIOReturnSuccess;
    case 10:  // client properties {s32 PriorityBand; s64 DutyCycleDuration; s64 HistogramDuration}: kept by Apple's
              // driver for its power management, nothing to do on VIC
        return a->structureInputSize == 0x18 ? kIOReturnSuccess : kIOReturnBadArgument;
    case 4:  // custom filter coefficients: accepted, VIC uses its own filter
        return a->structureInputSize == 0x20 ? kIOReturnSuccess : kIOReturnBadArgument;
    case 6:
        return kIOReturnNotPermitted;  // kernel tests (Apple's: release kernels refuse)
    case 0:                            // capture
    case 2:                            // abort transfers
    case 3:                            // abort captures
    case 7:                            // histogram
    case 8:                            // diagnostics
    case 9:                            // transform estimation
        return kIOReturnUnsupported;
    default:
        return kIOReturnBadArgument;
    }
}

IOReturn tegra_vic_register_port(const IOUserClient *self, mach_port_t port, UInt32 type, UInt32 refCon)
{
    (void)type, (void)refCon;  // Apple's driver keeps any type
    Conn *conn = connOf(self);
    if (!conn)
        return kIOReturnBadArgument;
    IOLockLock(conn->lock);
    const mach_port_t old = conn->port;
    conn->port = port;
    IOLockUnlock(conn->lock);
    if (old != MACH_PORT_NULL && old != port)
        iouc_release_port(old);
    return kIOReturnSuccess;
}

// Queued jobs hold the connection; closing waits for them (each is bounded by the engine), so no job outlives the
// task's lookup rights or the port it answers on.
IOReturn tegra_vic_client_close(IOUserClient *self)
{
    if (Conn *conn = connOf(self)) {
        IOLockLock(conn->lock);
        while (conn->pending)
            IOLockSleep(conn->lock, &conn->pending, THREAD_UNINT);
        IOLockUnlock(conn->lock);
    }
    self->terminate();
    return kIOReturnSuccess;
}

void tegra_vic_client_free(IOUserClient *self)
{
    if (Conn *conn = connOf(self)) {
        if (conn->port != MACH_PORT_NULL)
            iouc_release_port(conn->port);
        IOLock *lock = conn->lock;
        IOLockLock(gConnLock);
        bzero(conn, sizeof(*conn));
        conn->lock = lock;  // kept for the slot's next connection
        IOLockUnlock(gConnLock);
    }
    iouc_free(self);  // IOUserClient's: Apple's client's free would release state this connection never made
}

// --- The service's vtable slot ------------------------------------------------------------------------------------
// Apple's driver ignores `type` and checks no entitlement: any process the sandbox lets open it gets a client.
IOReturn tegra_vic_new_user_client(IOService *self, task_t task, void *securityID, UInt32 type,
                                   OSDictionary *properties, IOUserClient **handler)
{
    if (!tegra_vic_ready())
        return kIOReturnNotReady;
    Conn *conn = nullptr;
    IOLockLock(gConnLock);
    for (uint32_t i = 0; i < kMaxConnections && !conn; i++) {
        if (!gConns[i].client && !gConns[i].task)
            conn = &gConns[i];
    }
    if (conn)
        conn->task = task;  // claimed
    IOLockUnlock(gConnLock);
    if (!conn)
        return kIOReturnNoResources;
    if (!conn->lock)
        conn->lock = IOLockAlloc();
    const OSObject *object = OSMetaClass::allocClassWithName("IOSurfaceAcceleratorClient");
    IOUserClient *client = OSDynamicCast(IOUserClient, object);
    if (!conn->lock || !client) {
        OSSafeReleaseNULL(object);
        conn->task = nullptr;
        return kIOReturnNoMemory;
    }
    *reinterpret_cast<const uintptr_t **>(client) =
        &gClientVtable[2];  // Apple's client, our slots (see the top of this file)
    IOLockLock(gConnLock);
    conn->client = client;
    conn->port = MACH_PORT_NULL;
    conn->pending = 0;
    IOLockUnlock(gConnLock);
    if (!client->initWithTask(task, securityID, type, properties) || !client->attach(self)) {
        client->release();  // free() clears the connection
        return kIOReturnNoMemory;
    }
    if (!client->start(self)) {
        client->detach(self);
        client->release();
        return kIOReturnError;
    }
    *handler = client;
    return kIOReturnSuccess;
}

IOReturn tegra_vic_set_properties(const IORegistryEntry *self, const OSObject *properties)
{
    (void)self, (void)properties;
    return kIOReturnUnsupported;
}

// Copies the vtable of `object`'s class and replaces the slots of `fns` (base function -> ours); every replaced slot
// must hold an override of the class's own (inside its kext, not the base), else the class is not what was read.
bool swizzle(const OSObject *object, const uintptr_t *base, uintptr_t *copy, const void *const (*fns)[2],
             uint32_t count, char *line, size_t size)
{
    const uintptr_t *vtable = *reinterpret_cast<const uintptr_t *const *>(object);  // address point
    copy[0] = vtable[-2];
    copy[1] = vtable[-1];
    for (uint32_t i = 0; i < kVtableSlots - 2; i++)
        copy[2 + i] = vtable[i];
    for (uint32_t f = 0; f < count; f++) {
        const int slot = slotOf(base, fns[f][0]);
        if (slot < 0) {
            snprintf(line, size, "V3 FAIL no slot for a base method in %s's vtable",
                     object->getMetaClass()->getClassName());
            return false;
        }
        copy[2 + slot] = reinterpret_cast<uintptr_t>(fns[f][1]);
    }
    return true;
}

void putNumber(OSDictionary *d, const char *key, uint64_t value)
{
    const OSNumber *n = OSNumber::withNumber(value, 64);
    if (n) {
        d->setObject(key, n);
        n->release();
    }
}

// What Core Animation (ScalerLimits, scaler_has_capability) and VideoToolbox (scalerCapabilities) read. Every value is
// what VIC does (kTegraVICTraits); anything it lacks is 0, so those callers keep their own paths (HDR, film
// grain, colour management, histograms, compressed and 10-bit formats).
OSDictionary *capabilities(const TegraVICTraits *t)
{
    OSDictionary *d = OSDictionary::withCapacity(64);
    if (!d)
        return nullptr;
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMinSourceWidth", 4);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMinSourceHeight", 4);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMinDestWidth", 4);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMinDestHeight", 4);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMaxSourceWidth", TEGRA_VIC_MAX_DIM);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMaxSourceHeight", TEGRA_VIC_MAX_DIM);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMaxDestWidth", TEGRA_VIC_MAX_DIM);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMaxDestHeight", TEGRA_VIC_MAX_DIM);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMaxHUpscale", TEGRA_VIC_MAX_UPSCALE);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMaxVUpscale", TEGRA_VIC_MAX_UPSCALE);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMaxHDownscale", TEGRA_VIC_MAX_DOWNSCALE);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesMaxVDownscale", TEGRA_VIC_MAX_DOWNSCALE);
    // Apple's filter geometry, so Core Animation's custom-filter tables keep their shape (VIC ignores them).
    putNumber(d, "IOSurfaceAcceleratorFilterVerticalTapsCount", 9);
    putNumber(d, "IOSurfaceAcceleratorFilterHorizontalTapsCount", 15);
    putNumber(d, "IOSurfaceAcceleratorFilterVerticalPhasesCount", 32);
    putNumber(d, "IOSurfaceAcceleratorFilterHorizontalPhasesCount", 32);
    putNumber(d, "IOSurfaceAcceleratorFilterCoefficientsPrePointBits", 4);
    putNumber(d, "IOSurfaceAcceleratorFilterCoefficientsPostPointBits", 12);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesRotation", static_cast<uint64_t>(t->rotations == 0xff));
    // VideoToolbox gates 420f<->420v pairs on it: range conversion reads 4:2:0 through the slot matrix.
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesIdentityCSC", (t->yccIn & TEGRA_VIC_YCC_IN_RGB) ? 1 : 0);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesTiling", 0);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesFractionalXYOffsetsSupported", 0);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesFractionalDimensions", 0);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesDirectionalScaling", 0);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesHDR", 0);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesEnhancement", 0);
    putNumber(d, "IOSurfaceAcceleratorCapabilitiesFilmGrain", 0);
    putNumber(d, "kSurfaceAcceleratorCapabilitiesColorManager", 0);
    putNumber(d, "kSurfaceAcceleratorCapabilitiesAccessibility", 0);
    putNumber(d, "kSurfaceAcceleratorCapabilitiesHistogram", 0);
    putNumber(d, "kSurfaceAcceleratorCapabilitiesHistogramValues", 0);
    putNumber(d, "kSurfaceAcceleratorCapabilitiesVERF", 0);
    static const char *const kFormats[] = {"ARGB8888", "RGBA8888", "2Planes420"};
    char key[64];
    for (const char *f : kFormats) {
        snprintf(key, sizeof(key), "IOSurfaceAcceleratorFormatIn%s", f);
        putNumber(d, key, 1);
        snprintf(key, sizeof(key), "IOSurfaceAcceleratorFormatOut%s", f);
        putNumber(d, key, 1);
    }
    static const char *const kOff[] = {"Interleaved422", "LumaOnlyL008", "LumaOnlyL010",     "LumaOnlyL012",
                                       "LumaOnlyL016",   "ARGB2101010",  "ARGB8101010",      "2Planes42010",
                                       "2Planes42210",   "2Planes44410", "1PlaneYCBCR10444", "2Planes422",
                                       "2Planes444"};
    for (const char *f : kOff) {
        snprintf(key, sizeof(key), "IOSurfaceAcceleratorFormatIn%s", f);
        putNumber(d, key, 0);
        snprintf(key, sizeof(key), "IOSurfaceAcceleratorFormatOut%s", f);
        putNumber(d, key, 0);
    }
    return d;
}
}  // namespace

extern "C" int tegra_vic_publish(void)
{
    char line[200];
    if (!tegra_vic_surface_start()) {
        IOLog("TegraVIC: V3 FAIL no IOCoreSurfaceRoot (IOSurface kext) to look surfaces up in\n");
        return -19;
    }
    // Apple's driver kext (normally already loaded as the display family's dependency).
    const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.driver.AppleM2ScalerCSCDriver");
    // On failure both objects stay allocated: Apple's free must not run on what its start never made.
    const OSObject *object = OSMetaClass::allocClassWithName("AppleM2ScalerCSCDriver");
    IOService *service = OSDynamicCast(IOService, object);  // NOLINT(clang-analyzer-osx.cocoa.RetainCount): kept
    OSObject *probe = OSMetaClass::allocClassWithName("IOSurfaceAcceleratorClient");
    if (!service || !OSDynamicCast(IOUserClient, probe)) {
        // NOLINTNEXTLINE(clang-analyzer-osx.cocoa.RetainCount): kept, as above
        IOLog("TegraVIC: V3 FAIL Apple's AppleM2ScalerCSCDriver/IOSurfaceAcceleratorClient classes are not loaded "
              "(kext load %#x)\n",
              loaded);
        return -19;
    }
    const void *const serviceFns[][2] = {
        {(const void *)ios_new_user_client, (const void *)tegra_vic_new_user_client},
        {(const void *)ios_start, (const void *)ios_start},
        {(const void *)ios_stop, (const void *)ios_stop},
        {(const void *)ios_probe, (const void *)ios_probe},
        {(const void *)ios_set_power_state, (const void *)ios_set_power_state},
        {(const void *)ire_set_properties, (const void *)tegra_vic_set_properties},
    };
    const void *const clientFns[][2] = {
        {(const void *)iouc_free, (const void *)tegra_vic_client_free},
        {(const void *)iouc_client_close, (const void *)tegra_vic_client_close},
        {(const void *)iouc_external_method, (const void *)tegra_vic_external_method},
        {(const void *)iouc_register_port, (const void *)tegra_vic_register_port},
        {(const void *)iouc_client_memory, (const void *)iouc_client_memory},
        {(const void *)iouc_target_and_method, (const void *)iouc_target_and_method},
    };
    const int s0 = slotOf(tegra_vic_ioservice_vtable, (const void *)ios_new_user_client);
    if (!swizzle(service, tegra_vic_ioservice_vtable, gServiceVtable, serviceFns, 6, line, sizeof(line)) ||
        !swizzle(probe, tegra_vic_iouserclient_vtable, gClientVtable, clientFns, 6, line, sizeof(line))) {
        IOLog("TegraVIC: %s\n", line);
        return -22;  // the two unused objects stay allocated: neither Apple free runs on what its start never made
    }
    *(const uintptr_t **)service = &gServiceVtable[2];
    *(const uintptr_t **)probe = &gClientVtable[2];
    gConnLock = IOLockAlloc();
    probe->release();  // through the copy's free (IOUserClient's): Apple's would release state its start never made
    gQueueLock = IOLockAlloc();
    for (uint64_t *&pages : gPages)
        pages = IONew(uint64_t, kWindowPages);
    OSDictionary *caps = capabilities(tegra_vic_traits());
    if (!gConnLock || !gQueueLock || !gPages[0] || !gPages[1] || !caps || !service->init()) {
        OSSafeReleaseNULL(caps);
        IOLog("TegraVIC: V3 FAIL no memory for the service\n");
        return -12;
    }
    thread_t thread = nullptr;
    if (kernel_thread_start(engineThread, nullptr, &thread) != KERN_SUCCESS) {
        caps->release();
        IOLog("TegraVIC: V3 FAIL could not start the engine thread\n");
        return -12;
    }
    thread_deallocate(thread);
    service->setProperty("IOSurfaceAcceleratorCapabilitiesDict", caps);
    caps->release();
    service->setProperty("IOUserClientClass", "IOSurfaceAcceleratorClient");
    service->setProperty("NativeSandboxGrantProcess",
                         "backboardd,mediaserverd,replayd,videocodecd,mediaplaybackd,cameracaptured,com.apple.WebKit");
    service->setProperty("TegraVICTraitRotations", (unsigned long long)tegra_vic_traits()->rotations, 32);
    service->setProperty("TegraVICTraitPitchAlign", (unsigned long long)tegra_vic_traits()->pitchAlign, 32);
    IOService *resources = IOService::getResourceService();
    if (!resources || !service->attach(resources)) {
        IOLog("TegraVIC: V3 FAIL could not attach the service\n");
        return -12;
    }
    if (!service->start(resources)) {  // IOService::start (the copy's slot)
        service->detach(resources);
        IOLog("TegraVIC: V3 FAIL could not start the service\n");
        return -12;
    }
    service->registerService();
    tegra_vic_present_start();
    const char *present = "off (no 4:2:0 pass-through: NVDEC's pictures are copied by the CPU)";
    if (tegra_vic_present_ready()) {
        present = "on";
    } else if (tegra_vic_traits()->blockLinearIn) {
        present = "off (no memory for its page lists)";
    }
    IOLog("TegraVIC: V3 ok AppleM2ScalerCSCDriver published (Apple's classes, VIC's slots: newUserClient %d; "
          "IOSurfaceAcceleratorClient; BGRA, RGBA, 420v, 420f; transforms %#x; up to %ux%u, %ux up, %ux down); "
          "NVDEC present %s\n",
          s0, tegra_vic_traits()->rotations, TEGRA_VIC_MAX_DIM, TEGRA_VIC_MAX_DIM, TEGRA_VIC_MAX_UPSCALE,
          TEGRA_VIC_MAX_DOWNSCALE, present);
    return 0;  // the service stays: the attachment and the creation reference
}
