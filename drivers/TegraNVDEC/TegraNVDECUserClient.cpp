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
#include "../TegraVIC/TegraVICPresent.h"
#include "TegraNVDEC.h"
#include "nvidia/nvdec_drv.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

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

        struct Buffer {
        uint32_t handle;  // 0: slot free
        TegraNVDECBlock block;
        IOMemoryMap *map;  // the buffer in the task that allocated it
    };

    IOLock *lock_ = nullptr;
    Buffer buffers_[kMaxBuffers] = {};
    uint32_t nextHandle_ = 1, lastFence_ = 0;
    bool submitted_ = false, counted_ = false;

    Buffer *find(uint32_t handle);
    bool quiesce() const;
    static void drop(Buffer *b, bool returnMemory);
    IOReturn run(uint32_t method, const void *in, uint32_t inSize, void *out, uint32_t *outSize);
    static bool resolve(void *context, uint32_t handle, TegraNVDECBlock *block);

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

OSDefineMetaClassAndStructors(AppleD5500UserClient, IOUserClient)

    AppleD5500UserClient::Buffer *AppleD5500UserClient::find(uint32_t handle)
{
    for (uint32_t i = 0; handle && i < kMaxBuffers; i++) {
        if (buffers_[i].handle == handle)
            return &buffers_[i];
    }
    return nullptr;
}

bool AppleD5500UserClient::resolve(void *context, uint32_t handle, TegraNVDECBlock *block)
{
    const Buffer *b = static_cast<AppleD5500UserClient *>(context)->find(handle);
    if (b)
        *block = b->block;
    return b != nullptr;
}

// The connection's jobs are done (or the engine is wedged and will never touch memory again before a resume resets it).
bool AppleD5500UserClient::quiesce() const
{
    if (!submitted_)
        return true;
    const int ret = tegra_nvdec_wait(lastFence_);
    return ret == 0 || !tegra_nvdec_ready();
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

IOReturn AppleD5500UserClient::clientClose()
{
    IOLockLock(lock_);
    const bool idle = quiesce();
    if (!idle)
        IOLog("TegraNVDEC: a closing client's job never finished; its buffers stay wired\n");
    for (Buffer &b : buffers_) {
        if (b.handle)
            drop(&b, idle);
    }
    IOLockUnlock(lock_);
    terminate();
    return kIOReturnSuccess;
}

void AppleD5500UserClient::free()
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
    };

    if (method < TegraNVDECMethodGetInfo || method > TegraNVDECMethodPresent || inSize != kSizes[method].in ||
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
        IOLockLock(lock_);
        IOReturn result = kIOReturnSuccess;
        for (uint32_t i = 0; i < w->invalidate_count; i++) {
            const TegraNVDECRange *r = &w->invalidate[i];
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

    IOLockLock(lock_);
    IOReturn ret = kIOReturnSuccess;
    switch (method) {
    case TegraNVDECMethodGetInfo: {
        TegraNVDECInfo *info = static_cast<TegraNVDECInfo *>(out);
        memset(info, 0, sizeof(*info));
        info->abi_version = TEGRA_NVDEC_ABI_VERSION;
        info->nvdec_version = 0x20;
        info->codecs = tegra_nvdec_codecs();
        info->max_width = info->max_height = 4096;
        info->features = tegra_vic_present_ready() ? TEGRA_NVDEC_FEATURE_PRESENT : 0;
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
    case TegraNVDECMethodBufferFree: {
        // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): input size checked above
        Buffer *b = find(static_cast<const TegraNVDECBuffer *>(in)->handle);
        if (!b) {
            ret = kIOReturnBadArgument;
            break;
        }
        drop(b, quiesce());
        break;
    }
    case TegraNVDECMethodSubmit: {
        uint32_t fence = 0;
        const char *why = nullptr;
        const TegraNVDECSubmit *submit = static_cast<const TegraNVDECSubmit *>(in);
        int err = -22;
        const uint32_t codecs = tegra_nvdec_codecs();
        // NOLINTNEXTLINE(clang-analyzer-core.NullDereference): input size checked above
        const bool tenBit = submit->codec == TEGRA_NVDEC_CODEC_HEVC &&
                            submit->setup_bytes >= sizeof(nvdec_hevc_pic_s) &&
                            reinterpret_cast<const nvdec_hevc_pic_s *>(submit->setup)->bit_depth_luma > 8;
        if (submit->codec < 32 && (codecs & (1u << submit->codec)) && (!tenBit || (codecs & TEGRA_NVDEC_HEVC_MAIN10))) {
            err = tegra_nvdec_run(submit, resolve, this, &fence, &why);
        } else {
            why = "codec not decoded on NVDEC";
        }
        if (err) {
            static volatile int logged;
            if (why &&
                !__atomic_exchange_n(&logged, 1, __ATOMIC_SEQ_CST))  // one line per boot is enough to debug a client
                IOLog("TegraNVDEC: submit refused: %s\n", why);
            ret = ioreturn(err);
            break;
        }
        submitted_ = true;
        lastFence_ = fence;
        TegraNVDECFence *reply = static_cast<TegraNVDECFence *>(out);
        reply->fence = fence;
        reply->reserved = 0;
        break;
    }
    case TegraNVDECMethodPresent: {
        // Under the connection's lock: the buffer cannot be freed while VIC reads it.
        const TegraNVDECPresent *p = static_cast<const TegraNVDECPresent *>(in);
        const Buffer *b = find(p->handle);  // NOLINT(clang-analyzer-core.NullDereference): input size checked above
        const uint64_t luma = static_cast<uint64_t>(p->pitch) * p->rows;
        const uint64_t picture = static_cast<uint64_t>(p->chroma_offset) + luma / 2;
        if (!b) {
            ret = kIOReturnBadArgument;
            break;
        }
        if (!p->pitch || p->pitch % 64 || !p->rows || p->rows % 32 || p->block_height_log2 != 1 ||
            p->chroma_offset % 256 || p->chroma_offset < luma || picture > b->block.size ||
            (p->sample_bytes != 1 && p->sample_bytes != 2) || p->reserved ||
            (static_cast<uint64_t>(p->crop_x) + p->width) * p->sample_bytes > p->pitch ||
            static_cast<uint64_t>(p->crop_y) + p->height > p->rows) {
            // As for any refusal: the caller copies on the CPU.
            switch_dcache_clean_invalidate(b->block.va, b->block.size);
            ret = kIOReturnBadArgument;
            break;
        }
        TegraVICPicture vic;
        vic.pages = b->block.pages.pages;
        vic.pageCount = b->block.pages.count;
        vic.bytes = picture;
        vic.pitch = p->pitch;
        vic.rows = p->rows;
        vic.chromaOffset = p->chroma_offset;
        vic.blockHeightLog2 = p->block_height_log2;
        vic.cropX = p->crop_x;
        vic.cropY = p->crop_y;
        vic.width = p->width;
        vic.height = p->height;
        vic.dstX = p->dst_x;
        vic.dstY = p->dst_y;
        vic.surfaceID = p->surface_id;
        vic.sampleBytes = p->sample_bytes;
        const char *why = nullptr;
        const int err = tegra_vic_present(&vic, current_task(), &why);
        if (err) {
            // The caller copies the picture on the CPU now: it must read DRAM (what NVDEC wrote), as after Wait.
            switch_dcache_clean_invalidate(b->block.va, picture);
            ret = (err == -22 || err == -12) ? ioreturn(err) : kIOReturnUnsupported;
        }
        break;
    }
    default:
        ret = kIOReturnUnsupported;
        break;
    }
    IOLockUnlock(lock_);
    return ret;
}

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
    // Records are naturally aligned structs and the caller's buffer may not be: copy the input (Submit is the largest).
    const uint32_t inSize = arguments->structureInputSize - 8;
    void *copy = inSize ? IOMallocData(inSize) : nullptr;
    if (inSize && !copy)
        return kIOReturnNoMemory;
    if (inSize)
        memcpy(copy, header + 2, inSize);
    uint32_t outSize = arguments->structureOutputSize;
    const IOReturn ret = run(header[1], copy, inSize, arguments->structureOutput, &outSize);
    if (copy)
        IOFreeData(copy, inSize);
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
