// SPDX-License-Identifier: BSD-2-Clause-Patent
// N3 (on unless switch-no-nvdec): the TegraNVDEC service and its user client (ABI in TegraNVDECABI.h).
//
// The client class is named AppleD5500UserClient, the name of iPad7,1's own decoder client (AppleD5500, the VXD393
// kext, not loaded here), which some of 21H461's sandbox profiles may admit. The service is not called AppleD5500:
// H264H8.videodecoder (in the dyld cache) matches that name and would register Apple's decoder against this driver.
// The processes known to decode video are granted the service outright (xnu IOUserClient.cpp native_sandbox_grant,
// first 16 characters of the process name): the WebKit processes (Safari's media runs in com.apple.WebKit.GPU; the
// grant's 16 characters, "com.apple.WebKit", cover WebContent and Networking too), mediaplaybackd (AVPlayer's
// out-of-process playback: Photos, Files, Safari's non-MSE video), mediaserverd, videocodecd (CoreMedia's
// SeparateCodecProcess) and MobileSlideShow (Photos). Any other process gets the client only if its own profile admits
// it; AVD.videodecoder registers nothing in a process that cannot open it, so VideoToolbox's answers stay true there.
// The sandbox may admit only some selectors of the class, so every call is tunnelled through whichever one it admits.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <string.h>
#include "../TegraGM20B/SwitchGpuBacking.h"
#include "../TegraVIC/TegraVICPresent.h"
#include "SwitchNVDECCompletions.h"
#include "SwitchNVDECOutput.h"
#include "SwitchNVDECPark.h"
#include "SwitchNVDECProbeABI.h"
#include "TegraNVDEC.h"
#include "nvidia/nvdec_drv.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"

#if SWITCH_NVDEC_PROBES
extern "C" void switch_native_surface_publications(uint64_t *published, uint64_t *allocated, uint64_t *surfaces)
    __attribute__((weak));
#endif

namespace {
constexpr uint32_t kMaxBuffers = 64, kMaxConnections = 16;
constexpr uint64_t kMaxBufferBytes = 64ULL << 20;
volatile uint32_t gConnections;

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
        return kIOReturnError;
    }
}
}  // namespace

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class AppleD5500UserClient : public IOUserClient {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(AppleD5500UserClient)

        using Buffer = SwitchNVDECBuffer;

    struct SnapshotEntry {
        uint32_t handle;
        TegraNVDECBlock block;
    };

    struct SubmitScratch {
        const TegraNVDECSubmit *submit;  // single-owner while submitting_ is true
        SnapshotEntry blocks[kMaxBuffers];
        uint32_t blockCount;
    };

    IOLock *lock_ = nullptr;
    SwitchNVDECCompletions *completions_ = nullptr;
    SubmitScratch *scratch_ = nullptr;
    Buffer buffers_[kMaxBuffers] = {};
    SwitchNVDECPark park_ = {};
#if SWITCH_NVDEC_PROBES
    uint64_t recycles_ = 0, exports_ = 0, exportObjects_ = 0, exportSurfaces_ = 0;
    IOReturn bufferStats(uint32_t inSize, void *out, uint32_t *outSize) const;
#endif
    alignas(8) uint8_t input_[sizeof(TegraNVDECPresent)] = {};
    uint32_t nextHandle_ = 1, lastFence_ = 0, submitWaiters_ = 0;
    bool submitted_ = false, counted_ = false, submitting_ = false, closing_ = false;
#if SWITCH_NVDEC_PROBES
    bool displayProbe_ = false;
    IOReturn armDisplayProbe(const void *in, uint32_t inSize, uint32_t *outSize);
#endif

    Buffer *find(uint32_t handle);
    bool retainBlock(uint32_t handle, TegraNVDECBlock *out);
    bool inActiveSubmitLocked(const switch_gpu_backing *backing) const;
    void snapshotHandleLocked(uint32_t handle);
    void snapshotSubmitLocked();
    static bool quiesceFence(bool submitted, uint32_t fence);
    bool quiesce() const;
    static void drop(Buffer *b, bool returnMemory);
    void dropAll(bool returnMemory);
    IOReturn bufferFreeMethod(const void *in, uint32_t inSize, uint32_t *outSize);
    IOReturn submitMethod(const void *in, uint32_t inSize, void *out, uint32_t *outSize);
    IOReturn exportMethod(const void *in, uint32_t inSize, uint32_t *outSize);
    IOReturn presentMethod(const void *in, uint32_t inSize, uint32_t *outSize);
    // Wait, BufferFree, Submit, Export, Present and the probe arm manage lock_ themselves; every other method runs
    // under lock_.
    IOReturn run(uint32_t method, const void *in, uint32_t inSize, void *out, uint32_t *outSize);
    static bool resolveSnapshot(void *context, uint32_t handle, TegraNVDECBlock *block);

    static IOReturn sendCompletion(OSAsyncReference64 reference, IOReturn result, io_user_reference_t *arguments,
                                   UInt32 count)
    {
        return sendAsyncResult64(reference, result, arguments, count);
    }

    static void referCompletion(OSAsyncReference64 reference, mach_port_t port, task_t task)
    {
        setAsyncReference64(reference, port, 0, 0, task);
    }

public:
    bool setUp()
    {
        return (lock_ = IOLockAlloc()) != nullptr && (scratch_ = IONewZero(SubmitScratch, 1)) != nullptr &&
               (completions_ = switch_nvdec_completions_create({sendCompletion, referCompletion})) != nullptr;
    }

    void countConnection()
    {
        counted_ = true;
    }

    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon) APPLE_KEXT_OVERRIDE;
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *arguments, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleD5500UserClient, IOUserClient)

    AppleD5500UserClient::Buffer *AppleD5500UserClient::find(uint32_t handle)
{
    for (uint32_t i = 0; handle && i < kMaxBuffers; i++) {
        if (buffers_[i].handle == handle)
            return &buffers_[i];
    }
    return nullptr;
}

bool AppleD5500UserClient::retainBlock(uint32_t handle, TegraNVDECBlock *out)
{
    IOLockLock(lock_);
    const Buffer *b = find(handle);
    const bool ok = b && b->block.backing;
    if (ok) {
        *out = b->block;
        switch_gpu_backing_retain(out->backing);
    }
    IOLockUnlock(lock_);
    return ok;
}

bool AppleD5500UserClient::inActiveSubmitLocked(const switch_gpu_backing *backing) const
{
    for (uint32_t i = 0; i < scratch_->blockCount; ++i) {
        if (scratch_->blocks[i].block.backing == backing)
            return true;
    }
    return false;
}

void AppleD5500UserClient::snapshotHandleLocked(uint32_t handle)
{
    if (!handle)
        return;
    for (uint32_t i = 0; i < scratch_->blockCount; ++i) {
        if (scratch_->blocks[i].handle == handle)
            return;
    }
    const Buffer *b = find(handle);
    if (!b || scratch_->blockCount >= kMaxBuffers)
        return;
    if (b->block.backing)
        switch_gpu_backing_retain(b->block.backing);
    scratch_->blocks[scratch_->blockCount++] = {handle, b->block};
}

void AppleD5500UserClient::snapshotSubmitLocked()
{
    scratch_->blockCount = 0;
    const TegraNVDECSubmit *submit = scratch_->submit;
    const uint32_t entries = submit->entry_count <= TEGRA_NVDEC_MAX_ENTRIES ? submit->entry_count : 0;
    for (uint32_t i = 0; i < entries; ++i) {
        if (submit->entries[i].kind == TEGRA_NVDEC_ENTRY_RELOC)
            snapshotHandleLocked(submit->entries[i].value);
    }
    const uint32_t cleans = submit->clean_count <= TEGRA_NVDEC_MAX_RANGES ? submit->clean_count : 0;
    for (uint32_t i = 0; i < cleans; ++i)
        snapshotHandleLocked(submit->clean[i].handle);
}

bool AppleD5500UserClient::resolveSnapshot(void *context, uint32_t handle, TegraNVDECBlock *block)
{
    const auto *client = static_cast<const AppleD5500UserClient *>(context);
    for (uint32_t i = 0; handle && i < client->scratch_->blockCount; ++i) {
        if (client->scratch_->blocks[i].handle == handle) {
            *block = client->scratch_->blocks[i].block;
            return true;
        }
    }
    return false;
}

bool AppleD5500UserClient::quiesceFence(bool submitted, uint32_t fence)
{
    if (!submitted)
        return true;
    const int ret = tegra_nvdec_wait(fence);
    return ret == 0 || ret == -5 || tegra_nvdec_wedged();
}

// The connection's jobs are done (or the engine is wedged: parked, it touches no memory until a later job powers it on).
// A cancelled job (-5) is done too: its engine was reset with its memory client flushed before it failed.
bool AppleD5500UserClient::quiesce() const
{
    if (!submitted_)
        return true;
    const int ret = tegra_nvdec_wait(lastFence_);
    return ret == 0 || ret == -5 || tegra_nvdec_wedged();
}

// The client's mappings of the buffer are revoked before its pages can go to anyone else. A buffer a wedged job may
// still write stays wired and mapped in NVDEC's address space for good.
void AppleD5500UserClient::drop(Buffer *b, bool returnMemory)
{
    if (b->map) {
        b->map->unmap();
        b->map->release();
    }
    if (returnMemory)
        tegra_nvdec_buffer_free(&b->block);
    *b = {};
}

IOReturn AppleD5500UserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
    (void)refCon;
    // A refused port's right is the MIG server's to destroy: it does so when this returns an error.
    if (type != SWITCH_NVDEC_COMPLETION_PORT || !completions_)
        return kIOReturnBadArgument;
    return switch_nvdec_completions_listen(completions_, port, current_task());
}

// The parked buffers too (SwitchNVDECPark.h).
void AppleD5500UserClient::dropAll(bool returnMemory)
{
    for (Buffer &b : buffers_) {
        if (b.handle)
            drop(&b, returnMemory);
    }
    switch_nvdec_park_drop(&park_, returnMemory);
}

IOReturn AppleD5500UserClient::clientClose()
{
    if (completions_)
        switch_nvdec_completions_close(completions_);
    IOLockLock(lock_);
    closing_ = true;
    if (submitWaiters_)
        IOLockWakeup(lock_, &submitting_, false);
    while (submitting_) {
        ++submitWaiters_;
        IOLockSleep(lock_, &submitting_, THREAD_UNINT);
        --submitWaiters_;
    }
    const bool submitted = submitted_;
    const uint32_t fence = lastFence_;
    IOLockUnlock(lock_);
    const bool idle = quiesceFence(submitted, fence);
    if (!idle)
        IOLog("TegraNVDEC: a closing client's job never finished; its buffers stay wired\n");
    IOLockLock(lock_);
#if SWITCH_NVDEC_PROBES && defined(SWITCH_STATS_IO) && SWITCH_STATS_IO
    if (recycles_) {
        uint64_t published = 0, objects = 0, surfaces = 0;
        if (switch_native_surface_publications)
            switch_native_surface_publications(&published, &objects, &surfaces);
        IOLog("TegraNVDEC: client closed: %llu recycles, %llu of an exported picture's buffer: %llu took a parked "
              "buffer, %llu allocated one (%u parked at most); %llu exports into %llu surfaces, %llu new surface "
              "objects; since boot %llu native publications into %llu surfaces, %llu new surface objects\n",
              recycles_, park_.recycled, park_.taken, park_.allocated, park_.most,
              __atomic_load_n(&exports_, __ATOMIC_RELAXED), __atomic_load_n(&exportSurfaces_, __ATOMIC_RELAXED),
              __atomic_load_n(&exportObjects_, __ATOMIC_RELAXED), published, surfaces, objects);
    }
#endif
    dropAll(idle);
    IOLockUnlock(lock_);
    terminate();
    return kIOReturnSuccess;
}

void AppleD5500UserClient::free()
{
    if (lock_) {
        dropAll(quiesce());
        IOLockFree(lock_);
        lock_ = nullptr;
    }
    switch_nvdec_completions_release(completions_);
    completions_ = nullptr;
    if (scratch_) {
        IODelete(scratch_, SubmitScratch, 1);
        scratch_ = nullptr;
    }
    if (counted_)
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
    IOUserClient::free();
}

#if SWITCH_NVDEC_PROBES
IOReturn AppleD5500UserClient::armDisplayProbe(const void *in, uint32_t inSize, uint32_t *outSize)
{
    const auto *probe = static_cast<const SwitchNVDECDisplayProbe *>(in);
    if (inSize != sizeof(*probe) || !probe || probe->op != SwitchNVDECDisplayProbeArm || probe->reserved)
        return kIOReturnBadArgument;
    *outSize = 0;
    IOLockLock(lock_);
    displayProbe_ = true;
    IOLockUnlock(lock_);
    return kIOReturnSuccess;
}
#endif

#if SWITCH_NVDEC_PROBES
// Under lock_, as every short method.
IOReturn AppleD5500UserClient::bufferStats(uint32_t inSize, void *out, uint32_t *outSize) const
{
    if (inSize || !out || *outSize < sizeof(SwitchNVDECBufferStats))
        return kIOReturnBadArgument;
    *static_cast<SwitchNVDECBufferStats *>(out) = {
        recycles_,
        park_.recycled,
        park_.taken,
        park_.allocated,
        __atomic_load_n(&exports_, __ATOMIC_RELAXED),
        __atomic_load_n(&exportObjects_, __ATOMIC_RELAXED),
        park_.most,
        uint32_t(__atomic_load_n(&exportSurfaces_, __ATOMIC_RELAXED)),
    };
    *outSize = sizeof(SwitchNVDECBufferStats);
    return kIOReturnSuccess;
}
#endif

IOReturn AppleD5500UserClient::bufferFreeMethod(const void *in, uint32_t inSize, uint32_t *outSize)
{
    if (inSize != sizeof(TegraNVDECBuffer))
        return kIOReturnBadArgument;
    *outSize = 0;
    TegraNVDECBuffer request;
    memcpy(&request, in, sizeof(request));
    IOLockLock(lock_);
    Buffer *b = find(request.handle);
    while (b && submitting_ && b->block.backing && inActiveSubmitLocked(b->block.backing)) {
        ++submitWaiters_;
        IOLockSleep(lock_, &submitting_, THREAD_UNINT);
        --submitWaiters_;
        b = find(request.handle);
    }
    if (!b) {
        IOLockUnlock(lock_);
        return kIOReturnBadArgument;
    }
    Buffer victim = *b;
    *b = {};
    uint32_t engines = 0, owners = 0;
    const bool needsWait =
        switch_gpu_backing_pending(victim.block.backing, SWITCH_GPU_BACKING_WRITE, &engines, &owners) ||
        (engines & (1u << SWITCH_GPU_ENGINE_NVDEC)) != 0;
    const bool submitted = submitted_;
    const uint32_t fence = lastFence_;
    IOLockUnlock(lock_);
    drop(&victim, !needsWait || quiesceFence(submitted, fence));
    return kIOReturnSuccess;
}

IOReturn AppleD5500UserClient::submitMethod(const void *in, uint32_t inSize, void *out, uint32_t *outSize)
{
    if (inSize != sizeof(TegraNVDECSubmit) || *outSize < sizeof(TegraNVDECFence) || !out)
        return kIOReturnBadArgument;
    *outSize = sizeof(TegraNVDECFence);
    const TegraNVDECSubmit *submit = static_cast<const TegraNVDECSubmit *>(in);
    IOLockLock(lock_);
    while (submitting_ && !closing_) {
        ++submitWaiters_;
        IOLockSleep(lock_, &submitting_, THREAD_UNINT);
        --submitWaiters_;
    }
    if (closing_) {
        IOLockUnlock(lock_);
        return kIOReturnNotOpen;
    }
    submitting_ = true;
    scratch_->submit = submit;
    snapshotSubmitLocked();
#if SWITCH_NVDEC_PROBES
    const bool probe = displayProbe_;
    displayProbe_ = false;
#endif
    IOLockUnlock(lock_);

    uint32_t fence = 0;
    bool waitedForRoom = false;
    const char *why = nullptr;
    int err = -22;
    const uint32_t codecs = tegra_nvdec_codecs();
    const bool tenBit = submit->codec == TEGRA_NVDEC_CODEC_HEVC && submit->setup_bytes >= sizeof(nvdec_hevc_pic_s) &&
                        reinterpret_cast<const nvdec_hevc_pic_s *>(submit->setup)->bit_depth_luma > 8;
    SwitchNVDECCompletion completion = {};
    IOReturn ret = kIOReturnSuccess;
    if (const int armed = switch_nvdec_completions_arm(completions_, submit, resolveSnapshot, this, &completion)) {
        ret = ioreturn(armed);
    } else {
        if (submit->codec < 32 && (codecs & (1u << submit->codec)) && (!tenBit || (codecs & TEGRA_NVDEC_HEVC_MAIN10))) {
#if SWITCH_NVDEC_PROBES
            if (probe) {
                err = switch_nvdec_run_display_probe(submit, resolveSnapshot, this, &completion, &fence, &waitedForRoom,
                                                     &why);
                IOLog("TegraNVDEC: display-buffer probe: mode %u submit %s (%d)\n",
                      reinterpret_cast<const nvdec_hevc_pic_s *>(submit->setup)->disp_output_mode,
                      err ? (why ? why : "failed") : "queued", err);
            } else
#endif
                err = tegra_nvdec_run(submit, resolveSnapshot, this, &completion, &fence, &waitedForRoom, &why);
        } else {
            why = "codec not decoded on NVDEC";
        }
        if (err) {
            switch_nvdec_completions_disarm(&completion);
            static volatile int logged;
            if (why && !__atomic_exchange_n(&logged, 1, __ATOMIC_SEQ_CST))
                IOLog("TegraNVDEC: submit refused: %s\n", why);
            ret = ioreturn(err);
        }
    }

    switch_gpu_backing *toRelease[kMaxBuffers];
    uint32_t releaseCount = 0;
    IOLockLock(lock_);
    if (!ret) {
        submitted_ = true;
        lastFence_ = fence;
    }
    for (uint32_t i = 0; i < scratch_->blockCount; ++i) {
        if (scratch_->blocks[i].block.backing)
            toRelease[releaseCount++] = scratch_->blocks[i].block.backing;
    }
    scratch_->blockCount = 0;
    scratch_->submit = nullptr;
    submitting_ = false;
    if (submitWaiters_)
        IOLockWakeup(lock_, &submitting_, false);
    IOLockUnlock(lock_);
    for (uint32_t i = 0; i < releaseCount; ++i)
        switch_gpu_backing_release(toRelease[i]);
    if (!ret) {
        TegraNVDECFence *reply = static_cast<TegraNVDECFence *>(out);
        reply->fence = fence;
        reply->flags = waitedForRoom ? SWITCH_NVDEC_FENCE_WAITED_FOR_ROOM : 0;
    }
    return ret;
}

IOReturn AppleD5500UserClient::exportMethod(const void *in, uint32_t inSize, uint32_t *outSize)
{
    if (inSize != sizeof(TegraNVDECPresent))
        return kIOReturnBadArgument;
    *outSize = 0;
    TegraNVDECPresent request;
    memcpy(&request, in, sizeof(request));
    TegraNVDECBlock block = {};
    const bool held = retainBlock(request.handle, &block);
    SwitchNVDECExportRefusal refusal = {"native export of a buffer the connection does not hold", 0, 0, 0};
    bool fresh = false, first = false;
    const int error =
        held ? switch_nvdec_output_export(&block, &request, current_task(), &refusal, &fresh, &first) : -22;
    if (held)
        switch_gpu_backing_release(block.backing);
#if SWITCH_NVDEC_PROBES
    if (!error) {
        __atomic_add_fetch(&exports_, 1, __ATOMIC_RELAXED);
        if (fresh)
            __atomic_add_fetch(&exportObjects_, 1, __ATOMIC_RELAXED);
        if (first)
            __atomic_add_fetch(&exportSurfaces_, 1, __ATOMIC_RELAXED);
    }
#endif
    if (error) {
        const uint64_t key = switch_refusal_key_with(switch_refusal_key("TegraNVDEC", refusal.why), uint32_t(error));
        switch_refusal_k(key, "TegraNVDEC", refusal.why,
                         "a %ux%u %u-bit picture stays VIC's copy into surface %u (%d): allocation %llu bytes, "
                         "descriptor %llu, base offset %llu",
                         request.width, request.height, request.sample_bytes * 8, request.surface_id, error,
                         refusal.allocationBytes, refusal.descriptorBytes, refusal.baseOffset);
    }
    return error == -95 ? kIOReturnUnsupported : ioreturn(error);
}

IOReturn AppleD5500UserClient::presentMethod(const void *in, uint32_t inSize, uint32_t *outSize)
{
    if (inSize != sizeof(TegraNVDECPresent))
        return kIOReturnBadArgument;
    *outSize = 0;
    TegraNVDECPresent request;
    memcpy(&request, in, sizeof(request));
    TegraNVDECBlock block = {};
    const bool held = retainBlock(request.handle, &block);
    TegraVICPicture picture = {};
    const int checked = switch_nvdec_output_picture(held ? &block : nullptr, &request, &picture);
    if (checked) {
        if (held) {
            switch_dcache_invalidate(block.va, block.size);
            switch_gpu_backing_release(block.backing);
        }
        return ioreturn(checked);
    }
    const char *why = nullptr;
    const int error = tegra_vic_present(&picture, current_task(), &why);
    if (error)
        switch_dcache_invalidate(block.va, picture.bytes);
    switch_gpu_backing_release(block.backing);
    if (error)
        return (error == -22 || error == -12) ? ioreturn(error) : kIOReturnUnsupported;
    return kIOReturnSuccess;
}

IOReturn AppleD5500UserClient::run(uint32_t method, const void *in, uint32_t inSize, void *out, uint32_t *outSize)
{
    static const struct {
        uint32_t in, out;
    } kSizes[] = {
        {0, 0},
        {0, sizeof(TegraNVDECInfo)},
        {sizeof(TegraNVDECBufferAlloc), sizeof(TegraNVDECBufferReply)},
        {sizeof(TegraNVDECBuffer), 0},
        {sizeof(TegraNVDECSubmit), sizeof(TegraNVDECFence)},
        {sizeof(TegraNVDECWait), 0},
        {sizeof(TegraNVDECPresent), 0},
        {sizeof(TegraNVDECBuffer), sizeof(TegraNVDECBufferReply)},
        {sizeof(TegraNVDECPresent), 0},
    };

#if SWITCH_NVDEC_PROBES
    if (method == SwitchNVDECMethodDisplayProbe)
        return armDisplayProbe(in, inSize, outSize);
    if (method == SwitchNVDECMethodBufferStats)
        return bufferStats(inSize, out, outSize);
#endif
    if (method < TegraNVDECMethodGetInfo || method > SwitchNVDECMethodExport || inSize != kSizes[method].in ||
        *outSize < kSizes[method].out || (inSize && !in) || (kSizes[method].out && !out))
        return kIOReturnBadArgument;
    *outSize = kSizes[method].out;

    if (method == TegraNVDECMethodWait) {  // no lock: other calls of this connection may proceed while it sleeps
        const TegraNVDECWait *w = static_cast<const TegraNVDECWait *>(in);
        // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): input size checked above
        if (w->invalidate_count > TEGRA_NVDEC_MAX_RANGES)
            return kIOReturnBadArgument;
        const int ret = tegra_nvdec_wait(w->fence);  // bounded by the kernel, whatever timeout_us says
        if (ret)
            return ioreturn(ret);
        if (!w->invalidate_count)
            return kIOReturnSuccess;

        struct Span {
            switch_gpu_backing *backing;
            uint8_t *va;
            uint64_t length;
        } spans[TEGRA_NVDEC_MAX_RANGES];

        uint32_t spanCount = 0;
        IOReturn result = kIOReturnSuccess;
        IOLockLock(lock_);
        for (uint32_t i = 0; i < w->invalidate_count; i++) {
            const TegraNVDECRange *r = &w->invalidate[i];
            const Buffer *b = find(r->handle);
            if (!b || !b->block.backing || static_cast<uint64_t>(r->offset) + r->length > b->block.size) {
                result = kIOReturnBadArgument;
                break;
            }
            if (!r->length)
                continue;
            switch_gpu_backing_retain(b->block.backing);
            spans[spanCount++] = {b->block.backing, b->block.va + r->offset, r->length};
        }
        IOLockUnlock(lock_);
        if (!result && spanCount) {
            for (uint32_t i = 0; i < spanCount; i++)
                switch_dcache_invalidate_lines(spans[i].va, spans[i].length);
            switch_dcache_sync();
        }
        for (uint32_t i = 0; i < spanCount; i++)
            switch_gpu_backing_release(spans[i].backing);
        return result;
    }

    IOReturn ret = kIOReturnSuccess;
    switch (method) {
    case TegraNVDECMethodGetInfo: {
        TegraNVDECInfo *info = static_cast<TegraNVDECInfo *>(out);
        memset(info, 0, sizeof(*info));
        info->abi_version = TEGRA_NVDEC_ABI_VERSION;
        info->nvdec_version = 0x20;
        info->codecs = tegra_nvdec_codecs();
        info->max_width = info->max_height = 4096;
        info->features = TEGRA_NVDEC_FEATURE_COMPLETION | (tegra_vic_present_ready() ? TEGRA_NVDEC_FEATURE_PRESENT : 0);
        if (switch_nvdec_native_output_ready())
            info->features |= SWITCH_NVDEC_FEATURE_NATIVE_OUTPUT;
        info->jobs_in_flight = SwitchNVDECQueue::kSlots;
        tegra_nvdec_memory_usage(&info->memory_limit, &info->memory_free);
        break;
    }
    case TegraNVDECMethodBufferAlloc: {
        // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): input size checked above
        const uint64_t size = static_cast<const TegraNVDECBufferAlloc *>(in)->size;
        Buffer *slot = nullptr;
        for (uint32_t i = 0; i < kMaxBuffers && !slot; i++) {
            if (!buffers_[i].handle)
                slot = &buffers_[i];
        }
        if (!slot || !size || size > kMaxBufferBytes) {
            ret = slot ? kIOReturnBadArgument : kIOReturnNoResources;
            break;
        }
        ret = ioreturn(tegra_nvdec_buffer_alloc(size, &slot->block));
        if (ret)
            break;
        slot->map =
            slot->block.pages.memory->createMappingInTask(current_task(), 0, kIOMapAnywhere | kIOMapDefaultCache);
        if (!slot->map) {
            tegra_nvdec_buffer_free(&slot->block);
            *slot = {};
            ret = kIOReturnNoMemory;
            break;
        }
        slot->handle = nextHandle_++;
        if (!nextHandle_)
            nextHandle_ = 1;
        TegraNVDECBufferReply *reply = static_cast<TegraNVDECBufferReply *>(out);
        reply->handle = slot->handle;
        reply->reserved = 0;
        reply->size = slot->block.size;
        reply->address = slot->map->getAddress();
        break;
    }
    case SwitchNVDECMethodBufferRecycle: {
        const auto *request = static_cast<const TegraNVDECBuffer *>(in);
        auto *buffer = find(request->handle);
#if SWITCH_NVDEC_PROBES
        ++recycles_;
#endif
        ret = !buffer || request->reserved ? kIOReturnBadArgument
                                           : ioreturn(switch_nvdec_output_recycle(buffer, current_task(), &park_));
        if (!ret)
            *static_cast<TegraNVDECBufferReply *>(out) = {buffer->handle, 0, buffer->block.size,
                                                          buffer->map->getAddress()};
        break;
    }
    default:
        ret = kIOReturnUnsupported;
        break;
    }
    return ret;
}

#if SWITCH_NVDEC_PROBES
static constexpr uint32_t kUnlockedProbe = SwitchNVDECMethodDisplayProbe;
#else
static constexpr uint32_t kUnlockedProbe = TegraNVDECMethodWait;
#endif

IOReturn AppleD5500UserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                              IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)selector, (void)dispatch, (void)target, (void)reference;
    if (!arguments || arguments->structureInputDescriptor || arguments->structureOutputDescriptor ||
        arguments->scalarInputCount || arguments->scalarOutputCount || arguments->structureInputSize < 8)
        return kIOReturnBadArgument;
    const uint32_t *header = static_cast<const uint32_t *>(arguments->structureInput);
    if (header[0] != TEGRA_NVDEC_TUNNEL_MAGIC)
        return kIOReturnBadArgument;
    if (header[1] == TEGRA_NVDEC_TUNNEL_PROBE)
        return kIOReturnSuccess;
    const uint32_t method = header[1];
    const uint32_t inSize = arguments->structureInputSize - 8;
    uint32_t outSize = arguments->structureOutputSize;
    IOReturn ret;
    if (method == TegraNVDECMethodWait || method == kUnlockedProbe) {
        alignas(8) uint8_t record[sizeof(TegraNVDECWait)];
        if (inSize > sizeof(record))
            return kIOReturnBadArgument;
        memcpy(record, header + 2, inSize);
        ret = run(method, record, inSize, arguments->structureOutput, &outSize);
    } else if (method == TegraNVDECMethodBufferFree) {
        ret = bufferFreeMethod(header + 2, inSize, &outSize);
    } else if (method == TegraNVDECMethodSubmit) {
        ret = submitMethod(header + 2, inSize, arguments->structureOutput, &outSize);
    } else if (method == SwitchNVDECMethodExport) {
        ret = exportMethod(header + 2, inSize, &outSize);
    } else if (method == TegraNVDECMethodPresent) {
        ret = presentMethod(header + 2, inSize, &outSize);
    } else {
        if (inSize > sizeof(input_))
            return kIOReturnBadArgument;
        IOLockLock(lock_);
        if (inSize)
            memcpy(input_, header + 2, inSize);
        ret = run(method, inSize ? input_ : nullptr, inSize, arguments->structureOutput, &outSize);
        IOLockUnlock(lock_);
    }
    arguments->structureOutputSize = ret ? 0 : outSize;
    return ret;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class TegraNVDEC : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(TegraNVDEC)

    public : IOReturn newUserClient(task_t task, void *securityID, UInt32 type, OSDictionary *properties,
                                    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(TegraNVDEC, IOService)

    extern "C" void proc_selfname(char *buf, int size);

IOReturn TegraNVDEC::newUserClient(task_t task, void *securityID, UInt32 type, OSDictionary *properties,
                                   IOUserClient **handler)
{
    if (type != TEGRA_NVDEC_OPEN_TYPE)
        return kIOReturnBadArgument;
    if (!tegra_nvdec_ready())
        return kIOReturnNotReady;
    if (__atomic_add_fetch(&gConnections, 1, __ATOMIC_SEQ_CST) > kMaxConnections) {
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
        return kIOReturnNoResources;
    }
    AppleD5500UserClient *client = OSTypeAlloc(AppleD5500UserClient);
    if (!client || !client->initWithTask(task, securityID, type, properties)) {
        if (client)
            client->release();
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
        return kIOReturnNoMemory;
    }
    client->countConnection();  // from here free() gives the slot back
    if (!client->setUp() || !client->attach(this)) {
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

extern "C" int tegra_nvdec_publish(void)
{
    IOService *resources = IOService::getResourceService();
    TegraNVDEC *service = OSTypeAlloc(TegraNVDEC);
    if (!resources || !service || !service->init()) {
        OSSafeReleaseNULL(service);
        IOLog("TegraNVDEC: N3 FAIL could not create the service\n");
        return -12;
    }
    service->setProperty("IOUserClientClass", "AppleD5500UserClient");
    service->setProperty("NativeSandboxGrantProcess",
                         "com.apple.WebKit,mediaplaybackd,mediaserverd,videocodecd,MobileSlideShow");
    service->setProperty("TegraNVDECABIVersion", (unsigned long long)TEGRA_NVDEC_ABI_VERSION, 32);
    const uint32_t codecs = tegra_nvdec_codecs();
    service->setProperty(TEGRA_NVDEC_DECODES_H264, (codecs & (1u << TEGRA_NVDEC_CODEC_H264)) != 0);
    service->setProperty(TEGRA_NVDEC_DECODES_VP9, (codecs & (1u << TEGRA_NVDEC_CODEC_VP9)) != 0);
    service->setProperty(TEGRA_NVDEC_DECODES_HEVC, (codecs & (1u << TEGRA_NVDEC_CODEC_HEVC)) != 0);
    service->setProperty(TEGRA_NVDEC_DECODES_MPEG4, (codecs & (1u << TEGRA_NVDEC_CODEC_MPEG4)) != 0);
    service->setProperty(TEGRA_NVDEC_DECODES_MPEG2, (codecs & (1u << TEGRA_NVDEC_CODEC_MPEG12)) != 0);
    if (!service->attach(resources)) {
        service->release();
        IOLog("TegraNVDEC: N3 FAIL could not attach the service\n");
        return -12;
    }
    if (!service->start(resources)) {
        service->detach(resources);
        service->release();
        IOLog("TegraNVDEC: N3 FAIL could not start the service\n");
        return -12;
    }
    service->registerService();
    uint64_t limit = 0;
    uint64_t free = 0;
    tegra_nvdec_memory_usage(&limit, &free);
    IOLog("TegraNVDEC: N3 ok service published (user client AppleD5500UserClient, ABI %u, buffers of any pages behind "
          "the SMMU, %llu MiB at most)\n",
          TEGRA_NVDEC_ABI_VERSION, limit >> 20);
    service->release();  // the attachment keeps it
    return 0;
}
