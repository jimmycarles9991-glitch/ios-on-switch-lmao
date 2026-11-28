// SPDX-License-Identifier: BSD-2-Clause-Patent
// E3 (on by default): the TegraNVENC service and its user client (ABI in TegraNVENCABI.h).
//
// The client class is named AppleAVE2UserClient, the name of iPad7,1's own encoder client (AppleAVE2Driver, the AVE
// kext, not loaded here), which 21H461's media sandbox profiles admit. The service is not called AppleAVE2Driver:
// H9.videoencoder and H264H9.videoencoder (in the dyld cache) register Apple's AVE encoders when IOServiceMatching
// finds that name (AppleAVEVA_CheckPlatform), and would then drive this service with AVE's interface.
// VTCompressionSession runs encoders in the media server (mediaserverd, the Fig server), in videocodecd when CoreMedia's
// SeparateCodecProcess_Encode feature is on, and in processes entitled com.apple.videotoolbox.encode-in-process or
// asking for low latency (21H461 VTCompressionSessionCreateWithOptions); everyone else's sessions are remoted to the
// media server. The processes known to encode are granted the service outright (xnu IOUserClient.cpp
// native_sandbox_grant, first 16 characters of the process name): mediaserverd and videocodecd, replayd (ReplayKit),
// avconferenced (FaceTime), cameracaptured, and the WebKit processes (WebRTC encodes in com.apple.WebKit.GPU). Any other
// process gets the client only if its own profile admits it; AVE's encoder opens it only when a session starts.
// The sandbox may admit only some selectors of the class, so every call is tunnelled through whichever one it admits.
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <string.h>
#include "SwitchNVENCCompletions.h"
#include "TegraNVENC.h"
#include "TegraNVENCABI.h"
#include "TegraNVENCDirect.h"
#include "TegraNVENCPitchPlan.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"

namespace {
constexpr uint32_t kMaxBuffers = 64, kMaxConnections = 16, kOutputHistory = 16;
constexpr uint64_t kMaxBufferBytes = 32ULL << 20;
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
class AppleAVE2UserClient : public IOUserClient {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
    OSDeclareDefaultStructors(AppleAVE2UserClient)

        struct Buffer {
        uint32_t handle;  // 0: slot free
        TegraNVENCBlock block;
        IOMemoryMap *map;  // the buffer in the task that allocated it
    };

    struct SnapshotEntry {
        uint32_t handle;
        TegraNVENCBlock block;
    };

    struct SubmitScratch {
        TegraNVENCSubmit submit;  // single-owner while submitting_ is true
        TegraNVENCSubmit direct;  // single-owner while submitting_ is true
        SnapshotEntry blocks[kMaxBuffers];
        uint32_t blockCount;
    };

    struct OutputRecord {
        uint32_t fence;
        SwitchNVENCOutputSpec spec;
        bool valid;
        bool invalidated;
    };

    IOLock *lock_ = nullptr;
    SwitchNVENCCompletions *completions_ = nullptr;
    SubmitScratch *scratch_ = nullptr;
    alignas(8) uint8_t input_[sizeof(TegraNVENCBufferAlloc)] = {};
    OutputRecord outputs_[kOutputHistory] = {};
    uint32_t outputCursor_ = 0;
    Buffer buffers_[kMaxBuffers] = {};
    TegraNVENCBlock pitchScratch_ = {};
    uint32_t nextHandle_ = 1, lastFence_ = 0, submitWaiters_ = 0;
    bool submitted_ = false, counted_ = false, submitting_ = false, closing_ = false;

    Buffer *find(uint32_t handle);
    OutputRecord *findOutputLocked(uint32_t fence);
    bool inActiveSubmitLocked(const switch_gpu_backing *backing) const;
    void snapshotHandleLocked(uint32_t handle);
    void snapshotSubmitLocked();
    static bool quiesceFence(bool submitted, uint32_t fence);
    bool quiesce() const;
    static void drop(Buffer *b, bool returnMemory);
    int ensurePitchScratch(uint64_t bytes, bool hasLast, uint32_t lastFence);
    IOReturn waitMethod(const void *in, uint32_t inSize, uint32_t *outSize);
    IOReturn bufferFreeMethod(const void *in, uint32_t inSize, uint32_t *outSize);
    IOReturn submitPrepared(bool hasLast, uint32_t prevFence, const SwitchNVENCCompletion *completion, uint32_t *fence,
                            uint32_t *route, bool *submittedJob);
    IOReturn submitMethod(const void *in, uint32_t inSize, void *out, uint32_t *outSize);
    IOReturn runLocked(uint32_t method, const void *in, uint32_t inSize, void *out, uint32_t *outSize);
    static bool resolve(void *context, uint32_t handle, TegraNVENCBlock *block);
    static bool resolveSnapshot(void *context, uint32_t handle, TegraNVENCBlock *block);
    static bool resolveDirect(void *context, uint32_t handle, TegraNVENCBlock *block);
    static bool resolvePitch(void *context, uint32_t handle, TegraNVENCBlock *block);
    int submitDirect(const TegraNVENCSubmit *submit, const SwitchNVENCCompletion *completion, uint32_t *fence,
                     const char **why);
    int preparePitch(const TegraNVENCSubmit *submit, bool hasLast, uint32_t lastFence, TegraNVENCAfter *after,
                     uint32_t *route, const char **why);

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
               (completions_ = switch_nvenc_completions_create({sendCompletion, referCompletion})) != nullptr;
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

OSDefineMetaClassAndStructors(AppleAVE2UserClient, IOUserClient)

    AppleAVE2UserClient::Buffer *AppleAVE2UserClient::find(uint32_t handle)
{
    for (uint32_t i = 0; handle && i < kMaxBuffers; i++) {
        if (buffers_[i].handle == handle)
            return &buffers_[i];
    }
    return nullptr;
}

AppleAVE2UserClient::OutputRecord *AppleAVE2UserClient::findOutputLocked(uint32_t fence)
{
    for (OutputRecord &rec : outputs_) {
        if (rec.valid && rec.fence == fence)
            return &rec;
    }
    return nullptr;
}

bool AppleAVE2UserClient::inActiveSubmitLocked(const switch_gpu_backing *backing) const
{
    for (uint32_t i = 0; i < scratch_->blockCount; ++i) {
        if (scratch_->blocks[i].block.backing == backing)
            return true;
    }
    return false;
}

void AppleAVE2UserClient::snapshotHandleLocked(uint32_t handle)
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

void AppleAVE2UserClient::snapshotSubmitLocked()
{
    scratch_->blockCount = 0;
    const uint32_t entries = scratch_->submit.entry_count <= TEGRA_NVENC_MAX_ENTRIES ? scratch_->submit.entry_count : 0;
    for (uint32_t i = 0; i < entries; ++i) {
        if (scratch_->submit.entries[i].kind == TEGRA_NVENC_ENTRY_RELOC)
            snapshotHandleLocked(scratch_->submit.entries[i].value);
    }
    const uint32_t cleans = scratch_->submit.clean_count <= TEGRA_NVENC_MAX_RANGES ? scratch_->submit.clean_count : 0;
    for (uint32_t i = 0; i < cleans; ++i)
        snapshotHandleLocked(scratch_->submit.clean[i].handle);
}

bool AppleAVE2UserClient::resolve(void *context, uint32_t handle, TegraNVENCBlock *block)
{
    const Buffer *b = static_cast<AppleAVE2UserClient *>(context)->find(handle);
    if (b)
        *block = b->block;
    return b != nullptr;
}

bool AppleAVE2UserClient::resolveSnapshot(void *context, uint32_t handle, TegraNVENCBlock *block)
{
    const auto *client = static_cast<const AppleAVE2UserClient *>(context);
    for (uint32_t i = 0; handle && i < client->scratch_->blockCount; ++i) {
        if (client->scratch_->blocks[i].handle == handle) {
            *block = client->scratch_->blocks[i].block;
            return true;
        }
    }
    return false;
}

struct DirectContext {
    AppleAVE2UserClient *client;
    const TegraNVENCDirect *direct;
};

bool AppleAVE2UserClient::resolveDirect(void *context, uint32_t handle, TegraNVENCBlock *block)
{
    const auto *c = static_cast<const DirectContext *>(context);
    if (handle != TEGRA_NVENC_DIRECT_HANDLE)
        return resolveSnapshot(c->client, handle, block);
    *block = c->direct->block;
    return true;
}

bool AppleAVE2UserClient::resolvePitch(void *context, uint32_t handle, TegraNVENCBlock *block)
{
    auto *client = static_cast<AppleAVE2UserClient *>(context);
    if (handle != TEGRA_NVENC_PITCH_HANDLE)
        return resolveSnapshot(client, handle, block);
    *block = client->pitchScratch_;
    return client->pitchScratch_.iova != 0;
}

int AppleAVE2UserClient::ensurePitchScratch(uint64_t bytes, bool hasLast, uint32_t lastFence)
{
    if (pitchScratch_.iova && pitchScratch_.size >= bytes)
        return 0;
    if (pitchScratch_.iova) {
        if (!quiesceFence(hasLast, lastFence))
            return -16;
        tegra_nvenc_buffer_free(&pitchScratch_);
    }
    return tegra_nvenc_buffer_alloc(bytes, &pitchScratch_);
}

int AppleAVE2UserClient::preparePitch(const TegraNVENCSubmit *submit, bool hasLast, uint32_t lastFence,
                                      TegraNVENCAfter *after, uint32_t *route, const char **why)
{
    if (!tegra_nvenc_ready() || tegra_nvenc_wedged()) {
        *why = "NVENC is not ready";
        return -19;
    }
    if (const int err = tegra_nvenc_check_submit(submit, resolveSnapshot, this, why))
        return err;
    TegraNVENCPitchPlan plan = {};
    if ((*why = tegra_nvenc_pitch_plan(submit, &plan)) != nullptr)
        return -95;
    TegraNVENCBlock srcBlock = {};
    if (!resolveSnapshot(this, plan.inputHandle, &srcBlock) || !srcBlock.backing) {
        *why = "pitch input buffer";
        return -22;
    }
    const uint64_t lumaBytes = uint64_t(plan.srcPitch) * plan.height;
    const uint64_t chromaBytes = uint64_t(plan.srcPitch) * (plan.height / 2);
    int err = switch_gpu_backing_cpu_acquire(srcBlock.backing, 0, srcBlock.size,
                                             SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE);
    if (!err) {
        err = switch_gpu_backing_cpu_publish(srcBlock.backing, plan.srcLumaOffset, lumaBytes);
        if (!err)
            err = switch_gpu_backing_cpu_publish(srcBlock.backing, plan.srcChromaOffset, chromaBytes);
        (void)switch_gpu_backing_cpu_release(srcBlock.backing, 0, srcBlock.size,
                                             SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE);
    }
    if (err) {
        *why = "pitch input cache publish";
        return err;
    }
    err = ensurePitchScratch(plan.scratchBytes, hasLast, lastFence);
    if (err) {
        *why = "pitch conversion scratch";
        return err;
    }
    err =
        tegra_nvenc_pitch_convert(&plan, &srcBlock, &pitchScratch_, hasLast ? &lastFence : nullptr, after, route, why);
    if (!err)
        tegra_nvenc_pitch_patch(submit, &plan, TEGRA_NVENC_PITCH_HANDLE, &scratch_->direct);
    return err;
}

// The source as its native picture, read in place: 0, or -95 when NVENC cannot (VIC takes it), -11 when the picture's
// producers are not ones NVENC's stream can wait for, else the job's error.
int AppleAVE2UserClient::submitDirect(const TegraNVENCSubmit *submit, const SwitchNVENCCompletion *completion,
                                      uint32_t *fence, const char **why)
{
    if (!tegra_nvenc_engine_ready()) {
        if (why)
            *why = "NVENC is not ready for a direct native read";
        return -95;
    }
    TegraNVENCDirect direct;
    int err = tegra_nvenc_direct_prepare(submit, current_task(), &direct, &scratch_->direct, why);
    if (!err) {
        DirectContext context = {this, &direct};
        err =
            tegra_nvenc_run(&scratch_->direct, resolveDirect, &context, nullptr, &direct.order, completion, fence, why);
    }
    tegra_nvenc_direct_release(&direct);
    return err;
}

bool AppleAVE2UserClient::quiesceFence(bool submitted, uint32_t fence)
{
    if (!submitted)
        return true;
    const int ret = tegra_nvenc_wait(fence);
    return ret == 0 || ret == -5 || tegra_nvenc_wedged();
}

// The connection's jobs are done, or were cancelled (-5: the engine was reset and its memory client flushed before the
// failure was published), or the engine is wedged: parked, it touches no memory until a later job powers it on.
bool AppleAVE2UserClient::quiesce() const
{
    if (!submitted_)
        return true;
    const int ret = tegra_nvenc_wait(lastFence_);
    return ret == 0 || ret == -5 || tegra_nvenc_wedged();
}

// The client's mappings of the buffer are revoked before its pages can go to anyone else. A buffer a wedged job may
// still write stays wired and mapped in NVENC's address space for good.
void AppleAVE2UserClient::drop(Buffer *b, bool returnMemory)
{
    if (b->map) {
        b->map->unmap();
        b->map->release();
    }
    if (returnMemory)
        tegra_nvenc_buffer_free(&b->block);
    *b = {};
}

IOReturn AppleAVE2UserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
    (void)refCon;
    // A refused port's right is the MIG server's to destroy: it does so when this returns an error.
    if (type != SWITCH_NVENC_COMPLETION_PORT || !completions_)
        return kIOReturnBadArgument;
    return switch_nvenc_completions_listen(completions_, port, current_task());
}

IOReturn AppleAVE2UserClient::clientClose()
{
    if (completions_)
        switch_nvenc_completions_close(completions_);
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
        IOLog("TegraNVENC: a closing client's job never finished; its buffers stay wired\n");
    IOLockLock(lock_);
    for (Buffer &b : buffers_) {
        if (b.handle)
            drop(&b, idle);
    }
    if (idle && pitchScratch_.iova)
        tegra_nvenc_buffer_free(&pitchScratch_);
    IOLockUnlock(lock_);
    terminate();
    return kIOReturnSuccess;
}

void AppleAVE2UserClient::free()
{
    if (lock_) {
        const bool idle = quiesce();
        for (Buffer &b : buffers_) {
            if (b.handle)
                drop(&b, idle);
        }
        if (idle && pitchScratch_.iova)
            tegra_nvenc_buffer_free(&pitchScratch_);
        IOLockFree(lock_);
        lock_ = nullptr;
    }
    switch_nvenc_completions_release(completions_);
    completions_ = nullptr;
    if (scratch_) {
        IODelete(scratch_, SubmitScratch, 1);
        scratch_ = nullptr;
    }
    if (counted_)
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
    IOUserClient::free();
}

IOReturn AppleAVE2UserClient::waitMethod(const void *in, uint32_t inSize, uint32_t *outSize)
{
    if (inSize != sizeof(TegraNVENCWait))
        return kIOReturnBadArgument;
    *outSize = 0;
    alignas(8) TegraNVENCWait record;
    memcpy(&record, in, sizeof(record));
    const TegraNVENCWait *w = &record;
    if (w->invalidate_count > TEGRA_NVENC_MAX_RANGES)
        return kIOReturnBadArgument;
    int ret = tegra_nvenc_wait(w->fence);  // bounded by the kernel, whatever timeout_us says
    if (!ret)
        ret = tegra_nvenc_input_failure(w->fence);
    if (ret)
        return ioreturn(ret);
    IOLockLock(lock_);
    OutputRecord *rec = findOutputLocked(w->fence);
    bool statusCovered = false;
    IOReturn result = kIOReturnSuccess;
    for (uint32_t i = 0; i < w->invalidate_count; i++) {
        const TegraNVENCRange *r = &w->invalidate[i];
        const Buffer *b = find(r->handle);
        if (!b || static_cast<uint64_t>(r->offset) + r->length > b->block.size) {
            result = kIOReturnBadArgument;
            break;
        }
        switch_dcache_clean_invalidate(b->block.va + r->offset, r->length);
        if (rec && switch_nvenc_status_range_covered(&rec->spec, r->handle, r->offset, r->length))
            statusCovered = true;
    }
    if (!result && rec && !rec->invalidated) {
        if (switch_nvenc_invalidate_spec(&rec->spec, resolve, this, statusCovered, nullptr))
            rec->invalidated = true;
    }
    IOLockUnlock(lock_);
    return result;
}

IOReturn AppleAVE2UserClient::bufferFreeMethod(const void *in, uint32_t inSize, uint32_t *outSize)
{
    if (inSize != sizeof(TegraNVENCBuffer))
        return kIOReturnBadArgument;
    *outSize = 0;
    TegraNVENCBuffer request;
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
    const bool submitted = submitted_;
    const uint32_t fence = lastFence_;
    IOLockUnlock(lock_);
    drop(&victim, quiesceFence(submitted, fence));
    return kIOReturnSuccess;
}

IOReturn AppleAVE2UserClient::submitPrepared(bool hasLast, uint32_t prevFence, const SwitchNVENCCompletion *completion,
                                             uint32_t *fence, uint32_t *route, bool *submittedJob)
{
    const TegraNVENCSubmit *submit = &scratch_->submit;
    const char *why = nullptr;
    TegraNVENCAfter after = {};
    const TegraNVENCSubmit *runSubmit = submit;
    TegraNVENCResolve runResolve = resolveSnapshot;
    bool hasAfter = false;
    if (submit->source.surface_id != 0) {
        if (!tegra_nvenc_ready() || tegra_nvenc_wedged())
            return kIOReturnNotReady;
        if (const int err = tegra_nvenc_check_submit(submit, resolveSnapshot, this, &why))
            return ioreturn(err);
        const char *nativeWhy = nullptr;
        const int direct = submitDirect(submit, completion, fence, &nativeWhy);
        if (!direct) {
            *route = TEGRA_NVENC_INPUT_DIRECT;
            *submittedJob = true;
            return kIOReturnSuccess;
        }
        if (direct != -95 && direct != -11)
            return ioreturn(direct);
        if (nativeWhy)
            switch_refusal("TegraNVENC", nativeWhy, "the native picture goes through VIC: %s", nativeWhy);
        else if (direct == -11)
            switch_refusal("TegraNVENC", "native picture not orderable",
                           "the native picture goes through VIC: a GPU job the bridge does not cover still writes it, "
                           "it changed since it was looked up, or more than %u engines wrote it",
                           static_cast<unsigned>(TEGRA_NVENC_ORDER_WAITS));
        const int err = tegra_nvenc_source(submit, resolveSnapshot, this, current_task(),
                                           hasLast ? &prevFence : nullptr, &after, route, &why);
        if (err) {
            switch_refusal("TegraNVENC", why ? why : "source surface", "the input picture is not VIC's: %s",
                           why ? why : "refused");
            return err == -95 ? kIOReturnUnsupported : kIOReturnNotReady;
        }
        hasAfter = true;
        if (tegra_nvenc_is_pitch_input(submit)) {
            tegra_nvenc_source_bl2_patch(submit, &scratch_->direct);
            runSubmit = &scratch_->direct;
        }
    } else if (tegra_nvenc_is_pitch_input(submit)) {
        const int err = preparePitch(submit, hasLast, prevFence, &after, route, &why);
        if (err) {
            switch_refusal("TegraNVENC", why ? why : "pitch input", "pitch input conversion refused: %s",
                           why ? why : "refused");
            return err == -95 ? kIOReturnUnsupported : ioreturn(err);
        }
        runSubmit = &scratch_->direct;
        runResolve = resolvePitch;
        hasAfter = true;
    }
    const int err =
        tegra_nvenc_run(runSubmit, runResolve, this, hasAfter ? &after : nullptr, nullptr, completion, fence, &why);
    if (err) {
        static volatile int logged;
        if (why && !__atomic_exchange_n(&logged, 1, __ATOMIC_SEQ_CST))
            IOLog("TegraNVENC: submit refused: %s\n", why);
        return ioreturn(err);
    }
    *submittedJob = true;
    return kIOReturnSuccess;
}

IOReturn AppleAVE2UserClient::submitMethod(const void *in, uint32_t inSize, void *out, uint32_t *outSize)
{
    if (inSize != sizeof(TegraNVENCSubmit) || *outSize < sizeof(TegraNVENCFence) || !out)
        return kIOReturnBadArgument;
    *outSize = sizeof(TegraNVENCFence);
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
    memcpy(&scratch_->submit, in, sizeof(scratch_->submit));
    snapshotSubmitLocked();
    const bool hasLast = submitted_;
    const uint32_t prevFence = lastFence_;
    IOLockUnlock(lock_);

    SwitchNVENCOutputSpec outputSpec = {};
    const bool hasOutputSpec = switch_nvenc_output_spec(&scratch_->submit, &outputSpec);
    SwitchNVENCCompletion completion = {};
    uint32_t fence = 0;
    uint32_t route = TEGRA_NVENC_INPUT_CPU;
    bool submittedJob = false;
    IOReturn ret = kIOReturnSuccess;

    const int armed = switch_nvenc_completions_arm(completions_, &scratch_->submit, resolveSnapshot, this, &completion);
    if (armed) {
        ret = ioreturn(armed);
    } else {
        ret = submitPrepared(hasLast, prevFence, &completion, &fence, &route, &submittedJob);
        if (!submittedJob)
            switch_nvenc_completions_disarm(&completion);
    }

    switch_gpu_backing *toRelease[kMaxBuffers];
    uint32_t releaseCount = 0;
    IOLockLock(lock_);
    if (submittedJob) {
        submitted_ = true;
        lastFence_ = fence;
        if (hasOutputSpec) {
            outputs_[outputCursor_ % kOutputHistory] = {fence, outputSpec, true, false};
            ++outputCursor_;
        }
    }
    for (uint32_t i = 0; i < scratch_->blockCount; ++i) {
        if (scratch_->blocks[i].block.backing)
            toRelease[releaseCount++] = scratch_->blocks[i].block.backing;
    }
    scratch_->blockCount = 0;
    submitting_ = false;
    if (submitWaiters_)
        IOLockWakeup(lock_, &submitting_, false);
    IOLockUnlock(lock_);
    for (uint32_t i = 0; i < releaseCount; ++i)
        switch_gpu_backing_release(toRelease[i]);
    if (submittedJob) {
        TegraNVENCFence *reply = static_cast<TegraNVENCFence *>(out);
        reply->fence = fence;
        reply->input = route;
    }
    return ret;
}

IOReturn AppleAVE2UserClient::runLocked(uint32_t method, const void *in, uint32_t inSize, void *out, uint32_t *outSize)
{
    switch (method) {
    case TegraNVENCMethodGetInfo: {
        if (inSize != 0 || *outSize < sizeof(TegraNVENCInfo) || !out)
            return kIOReturnBadArgument;
        *outSize = sizeof(TegraNVENCInfo);
        TegraNVENCInfo *info = static_cast<TegraNVENCInfo *>(out);
        memset(info, 0, sizeof(*info));
        info->abi_version = TEGRA_NVENC_ABI_VERSION;
        info->nvenc_version = 0x50;
        info->codecs = kTegraNVENCCodecs;
        info->max_width = info->max_height = 4096;
        info->max_mbs = 36864;
        info->input_layout = TEGRA_NVENC_LAYOUT_TILED16;
        info->chroma_mode = TEGRA_NVENC_CHROMA_OFFSET;
        info->p_frames = 1;
        info->jobs_in_flight = kSwitchNVENCInFlight;
        info->features =
            TEGRA_NVENC_FEATURE_COMPLETION | (tegra_nvenc_source_ready() ? TEGRA_NVENC_FEATURE_SURFACE_INPUT : 0);
        info->surface_row_align =
            (info->features & TEGRA_NVENC_FEATURE_SURFACE_INPUT) ? tegra_nvenc_source_row_align() : 0;
        tegra_nvenc_memory_usage(&info->memory_limit, &info->memory_free);
        return kIOReturnSuccess;
    }
    case TegraNVENCMethodBufferAlloc: {
        if (inSize != sizeof(TegraNVENCBufferAlloc) || *outSize < sizeof(TegraNVENCBufferReply) || !out)
            return kIOReturnBadArgument;
        *outSize = sizeof(TegraNVENCBufferReply);
        // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): input size checked above
        const uint64_t size = static_cast<const TegraNVENCBufferAlloc *>(in)->size;
        Buffer *slot = nullptr;
        for (uint32_t i = 0; i < kMaxBuffers && !slot; i++) {
            if (!buffers_[i].handle)
                slot = &buffers_[i];
        }
        if (!slot || !size || size > kMaxBufferBytes)
            return slot ? kIOReturnBadArgument : kIOReturnNoResources;
        const IOReturn ret = ioreturn(tegra_nvenc_buffer_alloc(size, &slot->block));
        if (ret)
            return ret;
        slot->map =
            slot->block.pages.memory->createMappingInTask(current_task(), 0, kIOMapAnywhere | kIOMapDefaultCache);
        if (!slot->map) {
            tegra_nvenc_buffer_free(&slot->block);
            *slot = {};
            return kIOReturnNoMemory;
        }
        slot->handle = nextHandle_++;
        if (!nextHandle_ || nextHandle_ >= TEGRA_NVENC_DIRECT_HANDLE)
            nextHandle_ = 1;
        TegraNVENCBufferReply *reply = static_cast<TegraNVENCBufferReply *>(out);
        reply->handle = slot->handle;
        reply->reserved = 0;
        reply->size = slot->block.size;
        reply->address = slot->map->getAddress();
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnBadArgument;
    }
}

IOReturn AppleAVE2UserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
                                             IOExternalMethodDispatch *dispatch, OSObject *target, void *reference)
{
    (void)selector, (void)dispatch, (void)target, (void)reference;
    if (!arguments || arguments->structureInputDescriptor || arguments->structureOutputDescriptor ||
        arguments->scalarInputCount || arguments->scalarOutputCount || arguments->structureInputSize < 8)
        return kIOReturnBadArgument;
    const uint32_t *header = static_cast<const uint32_t *>(arguments->structureInput);
    if (header[0] != TEGRA_NVENC_TUNNEL_MAGIC)
        return kIOReturnBadArgument;
    if (header[1] == TEGRA_NVENC_TUNNEL_PROBE)
        return kIOReturnSuccess;
    const uint32_t method = header[1];
    const uint32_t inSize = arguments->structureInputSize - 8;
    uint32_t outSize = arguments->structureOutputSize;
    IOReturn ret;
    if (method == TegraNVENCMethodWait) {
        ret = waitMethod(header + 2, inSize, &outSize);
    } else if (method == TegraNVENCMethodBufferFree) {
        ret = bufferFreeMethod(header + 2, inSize, &outSize);
    } else if (method == TegraNVENCMethodSubmit) {
        ret = submitMethod(header + 2, inSize, arguments->structureOutput, &outSize);
    } else {
        if (inSize > sizeof(input_))
            return kIOReturnBadArgument;
        IOLockLock(lock_);
        if (inSize)
            memcpy(input_, header + 2, inSize);
        ret = runLocked(method, inSize ? input_ : nullptr, inSize, arguments->structureOutput, &outSize);
        IOLockUnlock(lock_);
    }
    arguments->structureOutputSize = ret ? 0 : outSize;
    return ret;
}

// NOLINTNEXTLINE(misc-use-internal-linkage): OSDefineMetaClassAndStructors needs the class's external linkage
class TegraNVENC : public IOService {
    // NOLINTNEXTLINE(misc-explicit-constructor): IOKit's structors macro declares the metaclass constructor
OSDeclareDefaultStructors(TegraNVENC)

    public : IOReturn newUserClient(task_t task, void *securityID, UInt32 type, OSDictionary *properties,
                                    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(TegraNVENC, IOService)

    extern "C" void proc_selfname(char *buf, int size);

IOReturn TegraNVENC::newUserClient(task_t task, void *securityID, UInt32 type, OSDictionary *properties,
                                   IOUserClient **handler)
{
    if (type != TEGRA_NVENC_OPEN_TYPE)
        return kIOReturnBadArgument;
    if (!tegra_nvenc_ready())
        return kIOReturnNotReady;
    if (__atomic_add_fetch(&gConnections, 1, __ATOMIC_SEQ_CST) > kMaxConnections) {
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
        return kIOReturnNoResources;
    }
    AppleAVE2UserClient *client = OSTypeAlloc(AppleAVE2UserClient);
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

extern "C" int tegra_nvenc_publish(void)
{
    IOService *resources = IOService::getResourceService();
    TegraNVENC *service = OSTypeAlloc(TegraNVENC);
    if (!resources || !service || !service->init()) {
        OSSafeReleaseNULL(service);
        IOLog("TegraNVENC: E3 FAIL could not create the service\n");
        return -12;
    }
    service->setProperty("IOUserClientClass", "AppleAVE2UserClient");
    service->setProperty("NativeSandboxGrantProcess",
                         "mediaserverd,videocodecd,replayd,avconferenced,cameracaptured,com.apple.WebKit");
    service->setProperty("TegraNVENCABIVersion", (unsigned long long)TEGRA_NVENC_ABI_VERSION, 32);
    if (!service->attach(resources)) {
        service->release();
        IOLog("TegraNVENC: E3 FAIL could not attach the service\n");
        return -12;
    }
    if (!service->start(resources)) {
        service->detach(resources);
        service->release();
        IOLog("TegraNVENC: E3 FAIL could not start the service\n");
        return -12;
    }
    service->registerService();
    uint64_t limit = 0;
    uint64_t free = 0;
    tegra_nvenc_memory_usage(&limit, &free);
    IOLog("TegraNVENC: E3 ok service published (user client AppleAVE2UserClient, ABI %u, input layout %u, chroma mode "
          "%u, P pictures; buffers of any pages behind the SMMU, %llu MiB at most)\n",
          TEGRA_NVENC_ABI_VERSION, static_cast<unsigned>(TEGRA_NVENC_LAYOUT_TILED16),
          static_cast<unsigned>(TEGRA_NVENC_CHROMA_OFFSET), limit >> 20);
    service->release();  // the attachment keeps it
    return 0;
}
