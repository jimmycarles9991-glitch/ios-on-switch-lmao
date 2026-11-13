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
#include "TegraNVENC.h"
#include "TegraNVENCABI.h"
#include "TegraNVENCDirect.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"

namespace {
constexpr uint32_t kMaxBuffers = 64, kMaxConnections = 16;
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

    IOLock *lock_ = nullptr;
    alignas(8) uint8_t input_[sizeof(TegraNVENCSubmit)] = {};  // under lock_: the record of a locked call
    TegraNVENCSubmit direct_ = {};                             // under lock_: a submit reading a native picture
    Buffer buffers_[kMaxBuffers] = {};
    uint32_t nextHandle_ = 1, lastFence_ = 0;
    bool submitted_ = false, counted_ = false;

    Buffer *find(uint32_t handle);
    bool quiesce() const;
    static void drop(Buffer *b, bool returnMemory);
    // Wait takes lock_ itself; every other method runs with the caller holding it.
    IOReturn run(uint32_t method, const void *in, uint32_t inSize, void *out, uint32_t *outSize);
    static bool resolve(void *context, uint32_t handle, TegraNVENCBlock *block);
    static bool resolveDirect(void *context, uint32_t handle, TegraNVENCBlock *block);
    int submitDirect(const TegraNVENCSubmit *submit, uint32_t *fence, const char **why);

public:
    bool setUp()
    {
        return (lock_ = IOLockAlloc()) != nullptr;
    }

    void countConnection()
    {
        counted_ = true;
    }

    IOReturn clientClose() APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;
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

bool AppleAVE2UserClient::resolve(void *context, uint32_t handle, TegraNVENCBlock *block)
{
    const Buffer *b = static_cast<AppleAVE2UserClient *>(context)->find(handle);
    if (b)
        *block = b->block;
    return b != nullptr;
}

struct DirectContext {
    AppleAVE2UserClient *client;
    const TegraNVENCDirect *direct;
};

bool AppleAVE2UserClient::resolveDirect(void *context, uint32_t handle, TegraNVENCBlock *block)
{
    const auto *c = static_cast<const DirectContext *>(context);
    if (handle != TEGRA_NVENC_DIRECT_HANDLE)
        return resolve(c->client, handle, block);
    *block = c->direct->block;
    return true;
}

// The source as its native NVDEC picture, read in place: 0, or -95 when NVENC cannot (VIC takes it), -11 when the
// picture's producers are not ones NVENC's stream can wait for, else the job's error.
int AppleAVE2UserClient::submitDirect(const TegraNVENCSubmit *submit, uint32_t *fence, const char **why)
{
    if (!tegra_nvenc_engine_ready())
        return -95;
    TegraNVENCDirect direct;
    int err = tegra_nvenc_direct_prepare(submit, current_task(), &direct, &direct_, why);
    if (!err) {
        DirectContext context = {this, &direct};
        err = tegra_nvenc_run(&direct_, resolveDirect, &context, nullptr, &direct.order, fence, why);
    }
    tegra_nvenc_direct_release(&direct);
    return err;
}

// The connection's jobs are done, or were cancelled (-5: the engine was reset and its memory client flushed before the
// failure was published), or the engine is wedged and will never touch memory again before a resume resets it.
bool AppleAVE2UserClient::quiesce() const
{
    if (!submitted_)
        return true;
    const int ret = tegra_nvenc_wait(lastFence_);
    return ret == 0 || ret == -5 || !tegra_nvenc_ready();
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

IOReturn AppleAVE2UserClient::clientClose()
{
    IOLockLock(lock_);
    const bool idle = quiesce();
    if (!idle)
        IOLog("TegraNVENC: a closing client's job never finished; its buffers stay wired\n");
    for (Buffer &b : buffers_) {
        if (b.handle)
            drop(&b, idle);
    }
    IOLockUnlock(lock_);
    terminate();
    return kIOReturnSuccess;
}

void AppleAVE2UserClient::free()
{
    if (lock_) {
        for (Buffer &b : buffers_) {
            if (b.handle)
                drop(&b, quiesce());
        }
        IOLockFree(lock_);
        lock_ = nullptr;
    }
    if (counted_)
        __atomic_sub_fetch(&gConnections, 1, __ATOMIC_SEQ_CST);
    IOUserClient::free();
}

IOReturn AppleAVE2UserClient::run(uint32_t method, const void *in, uint32_t inSize, void *out, uint32_t *outSize)
{
    static const struct {
        uint32_t in, out;
    } kSizes[] = {
        {0, 0},
        {0, sizeof(TegraNVENCInfo)},
        {sizeof(TegraNVENCBufferAlloc), sizeof(TegraNVENCBufferReply)},
        {sizeof(TegraNVENCBuffer), 0},
        {sizeof(TegraNVENCSubmit), sizeof(TegraNVENCFence)},
        {sizeof(TegraNVENCWait), 0},
    };

    if (method < TegraNVENCMethodGetInfo || method > TegraNVENCMethodWait || inSize != kSizes[method].in ||
        *outSize < kSizes[method].out)
        return kIOReturnBadArgument;
    *outSize = kSizes[method].out;

    if (method == TegraNVENCMethodWait) {  // no lock: other calls of this connection may proceed while it sleeps
        const TegraNVENCWait *w = static_cast<const TegraNVENCWait *>(in);
        // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): input size checked above
        if (w->invalidate_count > TEGRA_NVENC_MAX_RANGES)
            return kIOReturnBadArgument;
        int ret = tegra_nvenc_wait(w->fence);  // bounded by the kernel, whatever timeout_us says
        if (!ret)
            ret = tegra_nvenc_input_failure(w->fence);
        if (ret)
            return ioreturn(ret);
        IOLockLock(lock_);
        IOReturn result = kIOReturnSuccess;
        for (uint32_t i = 0; i < w->invalidate_count; i++) {
            const TegraNVENCRange *r = &w->invalidate[i];
            const Buffer *b = find(r->handle);
            if (!b || static_cast<uint64_t>(r->offset) + r->length > b->block.size) {
                result = kIOReturnBadArgument;
                break;
            }
            switch_dcache_clean_invalidate(b->block.va + r->offset, r->length);
        }
        IOLockUnlock(lock_);
        return result;
    }

    IOReturn ret = kIOReturnSuccess;
    switch (method) {
    case TegraNVENCMethodGetInfo: {
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
        info->features = tegra_nvenc_source_ready() ? TEGRA_NVENC_FEATURE_SURFACE_INPUT : 0;
        info->surface_row_align = info->features ? tegra_nvenc_source_row_align() : 0;
        tegra_nvenc_memory_usage(&info->memory_limit, &info->memory_free);
        break;
    }
    case TegraNVENCMethodBufferAlloc: {
        // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): input size checked above
        const uint64_t size = static_cast<const TegraNVENCBufferAlloc *>(in)->size;
        Buffer *slot = nullptr;
        for (uint32_t i = 0; i < kMaxBuffers && !slot; i++) {
            if (!buffers_[i].handle)
                slot = &buffers_[i];
        }
        if (!slot || !size || size > kMaxBufferBytes) {
            ret = slot ? kIOReturnBadArgument : kIOReturnNoResources;
            break;
        }
        ret = ioreturn(tegra_nvenc_buffer_alloc(size, &slot->block));
        if (ret)
            break;
        slot->map =
            slot->block.pages.memory->createMappingInTask(current_task(), 0, kIOMapAnywhere | kIOMapDefaultCache);
        if (!slot->map) {
            tegra_nvenc_buffer_free(&slot->block);
            *slot = {};
            ret = kIOReturnNoMemory;
            break;
        }
        slot->handle = nextHandle_++;
        if (!nextHandle_)
            nextHandle_ = 1;
        TegraNVENCBufferReply *reply = static_cast<TegraNVENCBufferReply *>(out);
        reply->handle = slot->handle;
        reply->reserved = 0;
        reply->size = slot->block.size;
        reply->address = slot->map->getAddress();
        break;
    }
    case TegraNVENCMethodBufferFree: {
        // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): input size checked above
        Buffer *b = find(static_cast<const TegraNVENCBuffer *>(in)->handle);
        if (!b) {
            ret = kIOReturnBadArgument;
            break;
        }
        drop(b, quiesce());
        break;
    }
    case TegraNVENCMethodSubmit: {
        const auto *submit = static_cast<const TegraNVENCSubmit *>(in);
        uint32_t fence = 0;
        const char *why = nullptr;
        TegraNVENCAfter after = {};
        uint32_t route = TEGRA_NVENC_INPUT_CPU;
        const bool sourced = submit->source.surface_id != 0;
        if (sourced) {
            if (const int err = tegra_nvenc_check_submit(submit, resolve, this, &why)) {
                ret = ioreturn(err);
                break;
            }
            const char *nativeWhy = nullptr;
            const int direct = submitDirect(submit, &fence, &nativeWhy);
            if (!direct) {
                submitted_ = true;
                lastFence_ = fence;
                TegraNVENCFence *reply = static_cast<TegraNVENCFence *>(out);
                reply->fence = fence;
                reply->input = TEGRA_NVENC_INPUT_DIRECT;
                break;
            }
            if (direct != -95 && direct != -11) {
                ret = ioreturn(direct);
                break;
            }
            if (nativeWhy)
                switch_refusal("TegraNVENC", nativeWhy, "the native picture goes through VIC: %s", nativeWhy);
            else if (direct == -11)
                switch_refusal(
                    "TegraNVENC", "native picture not orderable",
                    "the native picture goes through VIC: a GPU job still writes it, it changed since it was "
                    "looked up, or more than %u engines wrote it",
                    static_cast<unsigned>(TEGRA_NVENC_ORDER_WAITS));
            const int err = tegra_nvenc_source(submit, resolve, this, current_task(),
                                               submitted_ ? &lastFence_ : nullptr, &after, &route, &why);
            if (err) {
                switch_refusal("TegraNVENC", why ? why : "source surface", "the input picture is not VIC's: %s",
                               why ? why : "refused");
                ret = err == -95 ? kIOReturnUnsupported : kIOReturnNotReady;
                break;
            }
        }
        const int err = tegra_nvenc_run(submit, resolve, this, sourced ? &after : nullptr, nullptr, &fence, &why);
        if (err) {
            static volatile int logged;
            if (why &&
                !__atomic_exchange_n(&logged, 1, __ATOMIC_SEQ_CST))  // one line per boot is enough to debug a client
                IOLog("TegraNVENC: submit refused: %s\n", why);
            ret = ioreturn(err);
            break;
        }
        submitted_ = true;
        lastFence_ = fence;
        TegraNVENCFence *reply = static_cast<TegraNVENCFence *>(out);
        reply->fence = fence;
        reply->input = route;
        break;
    }
    default:
        ret = kIOReturnUnsupported;
        break;
    }
    return ret;
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
    // Records are naturally aligned structs and the caller's buffer may not be: each is copied, a Wait (which sleeps
    // without the lock) onto the stack, every other call into the connection's own record under the lock.
    const uint32_t method = header[1];
    const uint32_t inSize = arguments->structureInputSize - 8;
    uint32_t outSize = arguments->structureOutputSize;
    IOReturn ret;
    if (method == TegraNVENCMethodWait) {
        alignas(8) uint8_t record[sizeof(TegraNVENCWait)];
        if (inSize > sizeof(record))
            return kIOReturnBadArgument;
        memcpy(record, header + 2, inSize);
        ret = run(method, record, inSize, arguments->structureOutput, &outSize);
    } else {
        if (inSize > sizeof(input_))
            return kIOReturnBadArgument;
        IOLockLock(lock_);
        memcpy(input_, header + 2, inSize);
        ret = run(method, inSize ? input_ : nullptr, inSize, arguments->structureOutput, &outSize);
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
