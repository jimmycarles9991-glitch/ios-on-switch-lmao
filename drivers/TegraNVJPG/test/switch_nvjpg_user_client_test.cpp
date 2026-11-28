// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVJPGUserClient.cpp's engine-thread decode (beginJob, advanceJob) and TegraNVJPGRepack.cpp against the engine
// model (switch_nvjpg_engine_model.h), one translation unit with TegraNVJPGMapping.cpp. The model's kernel stand-ins declare
// OSObject, IOService and IOMemoryDescriptor as empty classes and define the service's publish as a no-op, so those
// names are moved aside while the model compiles; the user client's own stand-ins follow it.
#define SWITCH_DCACHE_PHYS_MODEL 1
#define OSObject                 SwitchModelOSObject
#define IOService                SwitchModelIOService
#define IOMemoryDescriptor       SwitchModelMemoryDescriptor
#define tegra_nvjpg_publish      switch_model_publish
#include "switch_nvjpg_engine_model.h"
#undef OSObject
#undef IOService
#undef IOMemoryDescriptor
#undef tegra_nvjpg_publish
#include <new>

#define IONewZero(type, count)             new (std::nothrow) type[count]()
#define IONewData(type, count)             new (std::nothrow) type[count]
#define IODelete(pointer, type, count)     delete[] (pointer)
#define IODeleteData(pointer, type, count) delete[] (pointer)
#define APPLE_KEXT_OVERRIDE                override
#define OSDeclareDefaultStructors(className)
#define OSDefineMetaClassAndStructors(className, superclass)
#define OSTypeAlloc(type) (new type)
#define OSSafeReleaseNULL(object) \
    do {                          \
        if (object) {             \
            (object)->release();  \
            (object) = nullptr;   \
        }                         \
    } while (0)

using UInt32 = uint32_t;
using IOByteCount = uint64_t;
using vm_offset_t = uintptr_t;
using mach_port_t = uint32_t;
using io_user_reference_t = uint64_t;
using OSAsyncReference64 = io_user_reference_t[8];
constexpr mach_port_t MACH_PORT_NULL = 0;

struct switch_test_task {};

using task_t = switch_test_task *;

constexpr IOReturn kIOReturnError = static_cast<IOReturn>(0xe00002bcU);
constexpr IOReturn kIOReturnNoMemory = static_cast<IOReturn>(0xe00002bdU);
constexpr IOReturn kIOReturnNoResources = static_cast<IOReturn>(0xe00002beU);
constexpr IOReturn kIOReturnBadArgument = static_cast<IOReturn>(0xe00002c2U);
constexpr IOReturn kIOReturnUnsupported = static_cast<IOReturn>(0xe00002c7U);
constexpr IOReturn kIOReturnIOError = static_cast<IOReturn>(0xe00002caU);
constexpr IOReturn kIOReturnNotAligned = static_cast<IOReturn>(0xe00002d0U);
constexpr IOReturn kIOReturnTimeout = static_cast<IOReturn>(0xe00002d6U);
constexpr IOReturn kIOReturnNotReady = static_cast<IOReturn>(0xe00002d8U);
constexpr IOReturn kIOReturnNotPermitted = static_cast<IOReturn>(0xe00002e2U);

inline task_t current_task()
{
    return nullptr;
}

inline int proc_selfpid()
{
    return 77;
}

class OSObject {
public:
    virtual ~OSObject() = default;

    virtual bool init()
    {
        return true;
    }

    virtual void free()
    {}

    void retain() const
    {}

    void release() const
    {}
};

inline OSObject switch_test_true;
inline OSObject *const kOSBooleanTrue = &switch_test_true;

class OSDictionary : public OSObject {};

class IOUserClient;

class IOService : public OSObject {
public:
    virtual IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **)
    {
        return kIOReturnUnsupported;
    }

    virtual IOReturn setProperties(OSObject *)
    {
        return kIOReturnUnsupported;
    }

    virtual bool start(IOService *)
    {
        return true;
    }

    bool attach(IOService *)
    {
        return true;
    }

    void detach(IOService *)
    {}

    bool terminate()
    {
        return true;
    }

    void registerService()
    {}

    void setName(const char *)
    {}

    bool setProperty(const char *, unsigned long long, unsigned)
    {
        return true;
    }

    bool setProperty(const char *, bool)
    {
        return true;
    }

    bool setProperty(const char *, const char *)
    {
        return true;
    }

    static IOService *getResourceService()
    {
        return nullptr;
    }
};

class IOMemoryDescriptor : public OSObject {
public:
    IOByteCount readBytes(IOByteCount offset, void *bytes, IOByteCount length)
    {
        if (offset + length > contents.size())
            return 0;
        memcpy(bytes, contents.data() + offset, length);
        return length;
    }

    std::vector<uint8_t> contents;
};

struct IOExternalMethodArguments {
    const void *structureInputDescriptor;
    void *structureOutputDescriptor;
    uint32_t scalarInputCount, scalarOutputCount;
    const void *structureInput;
    uint32_t structureInputSize;
    void *structureOutput;
    uint32_t structureOutputSize;
};

struct IOExternalMethodDispatch {};

class IOUserClient : public IOService {
public:
    bool initWithTask(task_t, void *, UInt32, OSDictionary *)
    {
        return true;
    }

    virtual IOReturn clientClose()
    {
        return kIOReturnSuccess;
    }

    virtual IOReturn registerNotificationPort(mach_port_t, UInt32, UInt32)
    {
        return kIOReturnSuccess;
    }

    virtual IOReturn externalMethod(uint32_t, IOExternalMethodArguments *, IOExternalMethodDispatch *, OSObject *,
                                    void *)
    {
        return kIOReturnUnsupported;
    }

    static OSObject *copyClientEntitlement(task_t, const char *)
    {
        return nullptr;
    }

    static void setAsyncReference64(OSAsyncReference64, mach_port_t, uint64_t, uint64_t, task_t)
    {}

    static IOReturn sendAsyncResult64(OSAsyncReference64, IOReturn, io_user_reference_t *, UInt32)
    {
        return kIOReturnSuccess;
    }

    static void releaseNotificationPort(mach_port_t)
    {}
};

#include "../TegraNVJPGMapping.cpp"
#include "../TegraNVJPGRepack.cpp"
#include "../TegraNVJPGRoute.h"
#include "../TegraNVJPGUserClient.cpp"
#include "../TegraNVJPGJobs.cpp"

bool tegra_nvjpg_stats_start(void)
{
    return true;
}

void switch_nvjpg_count_mapping(int)
{}

void tegra_nvjpg_count_refused(int, uint32_t)
{}

void tegra_nvjpg_count_job(int, uint32_t, bool, uint64_t, uint64_t, uint64_t, uint64_t)
{}

void tegra_nvjpg_counts_of(int, TegraNVJPGRouteCounts *out)
{
    *out = {};
}

void tegra_nvjpg_stats_line(void)
{}

void tegra_nvjpg_count_route(int, uint32_t)
{}

std::vector<int> gCancelled;

void tegra_nvjpg_count_cancelled(int pid)
{
    gCancelled.push_back(pid);
}

std::vector<uint32_t> gRoutes;

void tegra_nvjpg_note_route(uint32_t route, bool, uint32_t, uint32_t, uint32_t, const nvjpg_dec_drv_pic_setup_s *,
                            uint32_t, uint32_t)
{
    gRoutes.push_back(route);
}

// A backing as the GPU driver keeps one: the engines' mapping contexts, and the owner its pages go back to once no
// reference is left.
struct switch_gpu_backing {
    switch_gpu_backing_info info;
    void *context = nullptr;
    void (*destroy)(void *) = nullptr;
    void *owner = nullptr;
    void (*releaseOwner)(void *) = nullptr;
    int refs = 1;
};

namespace {
std::vector<std::string> gRefusals;
uint64_t gNextBufferPa = 0xa0000000;
TegraVICAcquire gVicAfter;
switch_gpu_backing *gRetiring;
bool gRetiredDuringRun;
int gMapsUnderTheJobLock;

struct CeCopyCall {
    tegra_gm20b_ce_plane src, dst;
    uint32_t rowBytes, rows;
    tegra_gm20b_ce_wait wait;
    switch_gpu_backing *srcBacking, *dstBacking;
    uint64_t srcBackingOffset, dstBackingOffset;
};

std::vector<CeCopyCall> gCeCopies;

// A CPU write of the re-pack buffer published to the GPU: its range, the word at it, and the copies queued before it
struct GuardPublish {
    uint64_t offset, bytes, value;
    size_t copiesBefore;
};

std::vector<GuardPublish> gGuardPublishes;
uint32_t gCpuWriters;
int gCeStatus = 0;

// A re-pack VIC or the copy engine took, held until NVJPG's syncpoint reaches the fence it was queued behind, as their
// hardware waits hold it
struct HeldRepack {
    uint32_t after;
    void (*completion)(void *, int);
    void *context;
};

std::vector<HeldRepack> gHeldRepacks;

// The re-pack engines run what NVJPG's syncpoint has released.
void runRepacks()
{
    std::vector<HeldRepack> due;
    for (auto it = gHeldRepacks.begin(); it != gHeldRepacks.end();) {
        if (static_cast<int32_t>(gEngine.syncpt - it->after) >= 0) {
            due.push_back(*it);
            it = gHeldRepacks.erase(it);
        } else {
            ++it;
        }
    }
    for (const HeldRepack &repack : due)
        repack.completion(repack.context, 0);
}

int gSurfaceTiled = 0;
int gSurfaceUndescribed = 0;
int gHolds = 0;
std::vector<int> gHoldsAtCopy;
tgm_layout gSurfaceLayout = {};
int gSurfaceCompressed = 0;
int gSurfaceAccessUnreserved = 0;
int gSurfaceAccessReleased = 0;
volatile uint64_t gSurfaceGen = 1;

// The GPU driver's retirement: every engine's mapping destroyed, then the pages given back.
void retire(switch_gpu_backing *backing)
{
    if (backing->context)
        backing->destroy(backing->context);
    backing->context = nullptr;
    if (backing->releaseOwner)
        backing->releaseOwner(backing->owner);
}

// The retirement worker, run while the engine thread decodes: the first time the decode takes gFlightLock with the job
// lock held.
void retireDuringTheRun(IOLock *lock)
{
    if (lock != gFlightLock || !gRetiring || !gJobLock->locked)
        return;
    switch_gpu_backing *backing = gRetiring;
    gRetiring = nullptr;
    retire(backing);
    gRetiredDuringRun = true;
}

void countMapsUnderTheJobLock(IOLock *lock)
{
    gMapsUnderTheJobLock += lock == gMapLock && gJobLock->locked;
}

// The value a decode's stream gives `method` (THI METHOD0, then METHOD1).
uint32_t methodValue(const std::vector<uint32_t> &stream, uint32_t method)
{
    for (size_t i = 0; i + 2 < stream.size(); ++i) {
        if (stream[i] == tegra_host1x_op_incr(kThiMethod0 / 4, 2) && stream[i + 1] == method / 4)
            return stream[i + 2];
    }
    return 0;
}

// Takes every free granule of `space`, so it reads as full until drain() gives them back.
std::vector<uint64_t> fill(TegraSMMUSpace &space)
{
    std::vector<uint64_t> granules;
    while (const uint64_t at = space.alloc(TegraSMMUSpace::kGranule))
        granules.push_back(at);
    return granules;
}

void drain(TegraSMMUSpace &space, const std::vector<uint64_t> &granules)
{
    for (const uint64_t at : granules)
        EXPECT_TRUE(space.release(at, TegraSMMUSpace::kGranule));
}
}  // namespace

extern "C" void *switch_gpu_backing_mapping_context(const switch_gpu_backing *backing, uint32_t engine)
{
    EXPECT_EQ(engine, uint32_t(SWITCH_GPU_ENGINE_NVJPG));
    return backing->context;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
    *info = backing->info;
}

extern "C" int switch_gpu_backing_cpu_acquire(switch_gpu_backing *, uint64_t, uint64_t, uint32_t access)
{
    EXPECT_EQ(access,
              uint32_t(SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_CPU_NO_CACHE | SWITCH_GPU_BACKING_CPU_AVOID_SYNC));
    ++gCpuWriters;
    return 0;
}

extern "C" int switch_gpu_backing_cpu_publish(switch_gpu_backing *, uint64_t offset, uint64_t bytes)
{
    EXPECT_EQ(gCpuWriters, 1u) << "a publish needs the CPU write held";
    const uint64_t value = *reinterpret_cast<const uint64_t *>(gPages->va + offset);
    gGuardPublishes.push_back({offset, bytes, value, gCeCopies.size()});
    return 0;
}

extern "C" int switch_gpu_backing_cpu_release(switch_gpu_backing *, uint64_t, uint64_t, uint32_t)
{
    --gCpuWriters;
    return 0;
}

extern "C" int switch_gpu_backing_mapping_install(switch_gpu_backing *backing, uint32_t, uint64_t generation,
                                                  void *context, void (*destroyContext)(void *))
{
    if (generation != backing->info.allocation_generation || backing->context)
        return SWITCH_GPU_BACKING_STALE;
    backing->context = context;
    backing->destroy = destroyContext;
    return 0;
}

extern "C" switch_gpu_backing *switch_gpu_backing_register(const uint64_t *pages, uint32_t count, uint32_t, void *owner,
                                                           void (*releaseOwner)(void *))
{
    auto *backing = new switch_gpu_backing{};
    backing->info.pages = pages;
    backing->info.page_count = count;
    backing->info.allocation_generation = 1;
    backing->owner = owner;
    backing->releaseOwner = releaseOwner;
    return backing;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
    if (--backing->refs)
        return;
    retire(backing);
    if (backing->releaseOwner)
        delete backing;
}

bool switch_dma_pages_alloc(switch_dma_pages *out, size_t size, IOOptionBits)
{
    *out = {};
    const uint32_t count = static_cast<uint32_t>((size + 4095) / 4096);
    out->memory = new IOBufferMemoryDescriptor;
    out->pages = new uint64_t[count];
    for (uint32_t i = 0; i < count; ++i)
        out->pages[i] = gNextBufferPa + i * 4096ULL;
    gNextBufferPa += count * 4096ULL;
    out->va = new uint8_t[count * 4096ULL];
    out->count = count;
    out->size = count * 4096ULL;
    return true;
}

void switch_dma_pages_free(switch_dma_pages *buffer)
{
    delete buffer->memory;
    delete[] buffer->pages;
    delete[] buffer->va;
    *buffer = {};
}

int switch_vic_surface_access(const TegraVICSurface *surface, bool, SwitchVICSurfaceAccess *access, bool,
                              switch_gpu_backing *held)
{
    *access = {};
    access->descriptor = surface->memory;
    access->backing = held;
    if (held)
        held->refs++;
    access->guard = {held, &gSurfaceGen, 1};
    return 0;
}

void switch_vic_surface_unreserve(SwitchVICSurfaceAccess *access)
{
    if (access->descriptor) {
        gSurfaceAccessUnreserved++;
        access->descriptor = nullptr;
    }
}

void switch_vic_surface_release_access(SwitchVICSurfaceAccess *access)
{
    switch_vic_surface_unreserve(access);
    if (access->backing) {
        gSurfaceAccessReleased++;
        switch_gpu_backing_release(access->backing);
    }
    *access = {};
}

extern "C" int tegra_gm20b_ce_copy_plane_checked(const tegra_gm20b_ce_plane *src, const tegra_gm20b_ce_plane *dst,
                                                 uint32_t rowBytes, uint32_t rows, const tegra_gm20b_ce_wait *wait,
                                                 const tegra_gm20b_ce_done *done, switch_gpu_backing *srcBacking,
                                                 uint64_t srcBackingOffset, switch_gpu_backing *dstBacking,
                                                 uint64_t dstBackingOffset, const switch_gpu_copy_ownership *,
                                                 const switch_gpu_event_submission *)
{
    gCeCopies.push_back({*src, *dst, rowBytes, rows,
                         wait ? *wait : tegra_gm20b_ce_wait{TEGRA_GM20B_CE_NO_SYNCPT, 0, 0, 0}, srcBacking, dstBacking,
                         srcBackingOffset, dstBackingOffset});
    gHoldsAtCopy.push_back(gHolds);
    if (gCeStatus)
        return gCeStatus;
    if (done && done->fn)
        gHeldRepacks.push_back({wait ? wait->threshold : gEngine.syncpt, done->fn, done->arg});
    return 0;
}

bool switch_vic_decode_output_ready(void)
{
    return true;
}

constexpr uint32_t kVicFence = 9001;
std::vector<uint32_t> gVicBackgrounds;

// VIC's re-pack: the test reads which fence it was queued behind, and VIC's own fence for it is kVicFence.
int switch_vic_decode_output(const TegraVICSurface *, const SwitchVICDecodeOutput *output,
                             void (*completion)(void *, int), void *context, const char **)
{
    gVicAfter = *output->after;
    if (output->fence)
        *output->fence = kVicFence;
    gHeldRepacks.push_back({output->after->threshold, completion, context});
    return 0;
}

bool switch_vic_job_background(uint32_t fence)
{
    gVicBackgrounds.push_back(fence);
    return true;
}

extern "C" bool tegra_vic_surface_start(void)
{
    return true;
}

extern "C" IOReturn tegra_vic_surface_lookup(uint32_t, task_t, TegraVICSurface *)
{
    return kIOReturnBadArgument;
}

extern "C" void tegra_vic_surface_release(TegraVICSurface *)
{}

extern "C" IOReturn tegra_vic_surface_prepare(TegraVICSurface *, uint64_t, uint64_t *, uint32_t, uint32_t *)
{
    return kIOReturnUnsupported;
}

extern "C" void tegra_vic_surface_complete(TegraVICSurface *)
{}

extern "C" void tegra_vic_surface_written(const TegraVICSurface *)
{}

switch_gpu_backing *switch_vic_surface_backing(const TegraVICSurface *)
{
    return nullptr;
}

extern "C" void tegra_gm20b_surface_engine_rows(OSObject *)
{}

extern "C" void *tegra_gm20b_surface_hold(uint64_t, int *tiled)
{
    *tiled = gSurfaceTiled;
    gHolds += gSurfaceTiled;
    return gSurfaceTiled ? reinterpret_cast<void *>(0x1) : nullptr;
}

extern "C" void tegra_gm20b_surface_unhold(void *)
{
    --gHolds;
}

extern "C" int tegra_gm20b_surface_layout(uint64_t, struct tgm_layout *out)
{
    if (!gSurfaceTiled || gSurfaceUndescribed)
        return 0;
    *out = gSurfaceLayout;
    return 1;
}

extern "C" int tegra_gm20b_surface_compressed(uint64_t)
{
    return gSurfaceCompressed;
}

extern "C" void switch_refusal(const char *, const char *reason_key, const char *, ...)
{
    gRefusals.emplace_back(reason_key);
}

extern "C" vm_offset_t phystokv(uint64_t pa)
{
    return static_cast<vm_offset_t>(pa);
}

extern "C" uint64_t phystokv_range(uint64_t pa, size_t *)
{
    return pa;
}

extern "C" bool switch_dma_pa_managed(uint64_t)
{
    return false;
}

namespace {
// A destination surface's backing: `count` pages, held by the test. Not copied: the backing points at its pages.
struct Destination {
    std::vector<uint64_t> pages;
    switch_gpu_backing backing{};

    Destination(uint32_t count, uint64_t first)
    {
        for (uint32_t i = 0; i < count; ++i)
            pages.push_back(first + i * 4096ULL);
        backing.info.pages = pages.data();
        backing.info.page_count = count;
        backing.info.allocation_generation = first;
    }

    Destination(const Destination &) = delete;
    Destination &operator=(const Destination &) = delete;

    const SwitchNVJPGMapping *mapping() const
    {
        return static_cast<const SwitchNVJPGMapping *>(backing.context);
    }
};

// A job as decode() queues it, its scan already copied into a slot.
Job *queued(switch_gpu_backing *backing)
{
    Job *job = IONewZero(Job, 1);
    job->engine = decodeJob();
    job->engine.planes = 1;
    job->plan.planes = 1;
    job->plan.target.format = TEGRA_NVJPG_OUT_BGRA;
    job->backing = backing;
    if (backing)
        backing->refs++;
    job->scan = takeScan(4096);
    EXPECT_EQ(switch_nvjpg_scan_fit(job->scan, 4096), 0);
    return job;
}

Job *decodeInto(Destination &d, uint64_t length, uint64_t writeEnd)
{
    Job *job = queued(&d.backing);
    job->dst.desc.length = length;
    job->plan.writeEnd = writeEnd;
    return job;
}

Job *repackOf(uint64_t bytes)
{
    Job *job = queued(nullptr);
    job->plan.repack = 1;
    job->plan.repackBytes = bytes;
    job->plan.writeEnd = bytes;
    job->plan.planes = job->engine.planes = 3;
    job->plan.planeStart[1] = bytes / 2;
    job->plan.planeStart[2] = bytes / 2 + bytes / 4;
    job->plan.target.format = TEGRA_NVJPG_OUT_NV24;
    return job;
}

// The engine thread's part of one queued job: begun, then carried on at each signal, with the model's engine, host1x
// and re-pack engines run between as their hardware would; `repackFirst` delivers a re-pack's end before NVJPG's, as
// two work loops may. A job no signal would ever carry on leaves the engine thread asleep for good: the test fails
// there.
// The copy engine's guard word for a queued copy, in the re-pack's buffer (TegraNVJPGRepack.cpp)
uint64_t guardOf(const CeCopyCall &copy)
{
    return *reinterpret_cast<const uint64_t *>(gPages->va + copy.wait.when);
}

IOReturn engineRuns(Job *job, bool repackFirst = false)
{
    for (bool inFlight = beginJob(job); inFlight; inFlight = !advanceJob(job)) {
        if (!job->woken && repackFirst)
            runRepacks();
        if (!job->woken) {
            deliver();
            runRepacks();
        }
        if (!job->woken) {
            ADD_FAILURE() << "no signal would wake the engine thread";
            return kIOReturnTimeout;
        }
        job->woken = false;
    }
    EXPECT_TRUE(gHeldRepacks.empty()) << "a re-pack outlived its decode";
    const IOReturn status = job->status;
    giveHold(job);
    dropJob(job);
    return status;
}

class NVJPGUserClient : public NVJPGGate {
protected:
    void SetUp() override
    {
        NVJPGGate::SetUp();
        if (!gQueueLock)
            gQueueLock = IOLockAlloc();
        gRefusals.clear();
        gMapsUnderTheJobLock = 0;
        gCeCopies.clear();
        gGuardPublishes.clear();
        gCpuWriters = 0;
        gCeStatus = 0;
        gHeldRepacks.clear();
        gSurfaceTiled = 0;
        gSurfaceUndescribed = 0;
        gHolds = 0;
        gHoldsAtCopy.clear();
        gRoutes.clear();
        gSurfaceLayout = {};
        gSurfaceCompressed = 0;
        gSurfaceAccessUnreserved = 0;
        gSurfaceAccessReleased = 0;
    }

    void TearDown() override
    {
        switch_test_lock_taken = nullptr;
    }
};
}  // namespace

// A destination is mapped at its first decode, once; from then on a decode into it, its scan copied into a slot that
// already holds it, writes no page entry, and nothing is mapped while the job lock is held.
TEST_F(NVJPGUserClient, AFirstUseDestinationMapsOnceAndSteadyStateDecodesMapNothing)
{
    Destination d(4, 0x91000000);
    switch_test_lock_taken = countMapsUnderTheJobLock;
    Job *first = decodeInto(d, 4 * 4096, 4 * 4096);
    const uint64_t scan = first->scan->address;
    const size_t flushes = gSmmuRangeFlushes.size();
    const size_t streams = gEngine.streams.size();
    EXPECT_EQ(engineRuns(first), kIOReturnIOError) << "the model's engine writes no status";
    EXPECT_EQ(gSmmuRangeFlushes.size(), flushes + 1) << "a first-use destination maps once";
    ASSERT_EQ(gEngine.streams.size(), streams + 1);
    ASSERT_NE(d.mapping(), nullptr);
    EXPECT_EQ(methodValue(gEngine.streams.back(), NVE7D0_SET_CUR_PIC), uint32_t(d.mapping()->address >> 8));
    EXPECT_EQ(methodValue(gEngine.streams.back(), NVE7D0_SET_BITSTREAM), uint32_t(scan >> 8));

    for (int i = 0; i < 3; ++i) {
        const size_t before = gSmmuRangeFlushes.size();
        EXPECT_EQ(engineRuns(decodeInto(d, 4 * 4096, 4 * 4096)), kIOReturnIOError);
        EXPECT_EQ(gSmmuRangeFlushes.size(), before) << "decode " << i + 2 << " wrote page entries";
    }
    EXPECT_EQ(gEngine.streams.size(), streams + 4);
    EXPECT_EQ(gMapsUnderTheJobLock, 0);
    EXPECT_EQ(d.backing.refs, 1);
    retire(&d.backing);
}

// The GPU driver's one retirement worker, which every GPU, VIC and NVDEC backing free queues behind, unmaps another
// destination while a decode holds the job lock: it never waits for a decode.
TEST_F(NVJPGUserClient, RetirementCompletesWhileADecodeHoldsTheJobLock)
{
    Destination other(1, 0x92000000);
    EXPECT_EQ(engineRuns(decodeInto(other, 4096, 4096)), kIOReturnIOError);
    ASSERT_NE(other.mapping(), nullptr);
    const uint64_t address = other.mapping()->address;
    Destination d(2, 0x93000000);
    Job *job = decodeInto(d, 2 * 4096, 2 * 4096);
    gRetiring = &other.backing;
    gRetiredDuringRun = false;
    switch_test_lock_taken = retireDuringTheRun;
    EXPECT_NO_THROW((void)engineRuns(job));
    switch_test_lock_taken = nullptr;
    EXPECT_TRUE(gRetiredDuringRun);
    EXPECT_EQ(gDomain.translate(address), UINT64_MAX);
    retire(&d.backing);
}

// A full output window is a refusal like any other the engine thread meets: kIOReturnUnsupported with its reason, so
// the caller decodes the image on the CPU, and the engine never runs.
TEST_F(NVJPGUserClient, AFullOutputWindowIsRefusedSoTheCallerDecodesOnTheCPU)
{
    Destination d(1, 0x94000000);
    Job *job = decodeInto(d, 4096, 4096);
    const std::vector<uint64_t> full = fill(gSpaces[TEGRA_NVJPG_WINDOW_OUT]);
    const size_t streams = gEngine.streams.size();
    EXPECT_EQ(engineRuns(job), kIOReturnUnsupported);
    drain(gSpaces[TEGRA_NVJPG_WINDOW_OUT], full);
    EXPECT_EQ(gEngine.streams.size(), streams);
    ASSERT_EQ(gRefusals.size(), 1u);
    EXPECT_EQ(gRefusals.back(), "output window full");
    EXPECT_EQ(d.mapping(), nullptr);
}

TEST_F(NVJPGUserClient, ADestinationLongerThanItsBackingIsRefused)
{
    Destination d(1, 0x95000000);
    EXPECT_EQ(engineRuns(decodeInto(d, 2 * 4096, 2 * 4096)), kIOReturnUnsupported);
    ASSERT_EQ(gRefusals.size(), 1u);
    EXPECT_EQ(gRefusals.back(), "destination longer than its backing");
    EXPECT_EQ(d.mapping(), nullptr);
}

// A '422f' or '444f' decode maps the service's own buffer once, queues VIC's re-pack behind its fence, and maps nothing
// again while the buffer holds the next one.
TEST_F(NVJPGUserClient, ARepackMapsItsBufferOnceAndQueuesVICBehindTheDecode)
{
    switch_test_lock_taken = countMapsUnderTheJobLock;
    Job *first = repackOf(64 << 10);
    const size_t flushes = gSmmuRangeFlushes.size();
    EXPECT_EQ(engineRuns(first), kIOReturnIOError) << "the model's engine writes no status";
    EXPECT_EQ(gSmmuRangeFlushes.size(), flushes + 1);
    EXPECT_EQ(gVicAfter.syncpt, kSyncpt);
    EXPECT_EQ(gVicAfter.threshold, gEngine.syncpt) << "VIC waits for this decode's OP_DONE";
    Job *next = repackOf(32 << 10);
    const size_t before = gSmmuRangeFlushes.size();
    EXPECT_EQ(engineRuns(next), kIOReturnIOError);
    EXPECT_EQ(gSmmuRangeFlushes.size(), before);
    EXPECT_EQ(gMapsUnderTheJobLock, 0);
    EXPECT_TRUE(gRefusals.empty());
}

// A 2048-byte-row picture into a destination of 2080-byte rows: NVJPG's rows go through the copy engine.
static Job *copyRowsInto(Destination &d, IOMemoryDescriptor &memory)
{
    Job *job = decodeInto(d, 260ULL * 4096, 2048ULL * 512);
    job->dst.memory = &memory;
    job->plan.repack = TEGRA_NVJPG_REPACK_CE;
    job->plan.repackBytes = 2048ULL * 512;
    job->plan.planes = job->engine.planes = 1;
    job->plan.target.pitch[0] = 2048;
    job->plan.planeStart[0] = 0;
    job->plan.copyDstStart[0] = 0;
    job->plan.copyDstPitch[0] = 2080;
    job->plan.copyRowBytes[0] = 2048;
    job->plan.copyRows[0] = 512;
    return job;
}

// HW Test's copy-guard probe (image.nvjpg_copy_guard) queues its copy with the guard already 0, as a cancelled decode
// leaves it; the next decode's guard is open again
TEST_F(NVJPGUserClient, ACopyGuardProbeQueuesItsCopyClosedAndTheNextDecodeOpen)
{
    Destination d(260, 0x9f000000);
    IOMemoryDescriptor dstMem;
    Job *probe = copyRowsInto(d, dstMem);
    probe->guardProbe = true;
    EXPECT_EQ(engineRuns(probe), kIOReturnIOError) << "the model's engine writes no status";
    ASSERT_EQ(gCeCopies.size(), 1u);
    EXPECT_EQ(gCeCopies[0].wait.predicated, 1u);
    EXPECT_EQ(guardOf(gCeCopies[0]), 0u) << "the probe's copy launches with its guard closed";
    gCeCopies.clear();
    EXPECT_EQ(engineRuns(copyRowsInto(d, dstMem)), kIOReturnIOError);
    ASSERT_EQ(gCeCopies.size(), 1u);
    EXPECT_EQ(guardOf(gCeCopies[0]), 1u) << "an ordinary decode's copy lands";
    const uint64_t when = gCeCopies[0].wait.when;
    EXPECT_EQ(when % 16, 0u) << "the predicate is proven on 16-byte-aligned words alone";
    EXPECT_EQ(when + 16, gPages->size) << "the guard is the buffer's last 16 bytes";
    // Each decode's guard is published as a CPU write before its copy is queued, so that copy's submission drops the
    // GPU L2's line: the probe's 0 cached there would otherwise make this decode copy nothing and report success.
    ASSERT_EQ(gGuardPublishes.size(), 2u);
    for (size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(gGuardPublishes[i].offset, when) << i;
        EXPECT_EQ(gGuardPublishes[i].bytes, 16u) << i;
        EXPECT_EQ(gGuardPublishes[i].value, i) << "the probe's closed, then the decode's open";
        EXPECT_EQ(gGuardPublishes[i].copiesBefore, 0u) << "published before the copy is queued";
    }
    EXPECT_EQ(gCpuWriters, 0u);
    retire(&d.backing);
}

// Unaligned destination strides (e.g. stride 2080) decode pitch-linear into the pooled 64B-aligned scratch buffer, then
// Copy Engine DMA-repacks exact visible rows behind NVJPG's OP_DONE syncpoint with zero CPU pixel copies.
TEST_F(NVJPGUserClient, UnalignedStrideExecutesViaCopyEngineRepackBehindNVJPGFence)
{
    Destination d(260, 0x96000000);
    IOMemoryDescriptor dstMem;
    Job *job = copyRowsInto(d, dstMem);
    EXPECT_EQ(engineRuns(job), kIOReturnIOError) << "the model's engine writes no status";
    EXPECT_TRUE(gRefusals.empty());
    ASSERT_EQ(gCeCopies.size(), 1u);
    EXPECT_EQ(gCeCopies[0].wait.syncpt, kSyncpt);
    EXPECT_EQ(gCeCopies[0].wait.threshold, gEngine.syncpt);
    EXPECT_EQ(gCeCopies[0].src.pitch, 2048u);
    EXPECT_EQ(gCeCopies[0].dst.pitch, 2080u);
    EXPECT_EQ(gCeCopies[0].rowBytes, 2048u);
    EXPECT_EQ(gCeCopies[0].rows, 512u);
    EXPECT_EQ(gCeCopies[0].wait.predicated, 1u);
    EXPECT_EQ(guardOf(gCeCopies[0]), 1u) << "a decode that ends copies its picture";
    EXPECT_EQ(gCeCopies[0].dstBacking, &d.backing);
    EXPECT_EQ(gSurfaceAccessUnreserved, 1);
    EXPECT_EQ(gSurfaceAccessReleased, 1);
    EXPECT_EQ(gRoutes, std::vector<uint32_t>{TEGRA_NVJPG_ROUTE_COPY_ROWS});
    EXPECT_EQ(d.backing.refs, 1);
    retire(&d.backing);
}

// A decode cancelled on the engine, past its bound or on a falcon error, releases its fence: the copy engine's re-pack
// queued behind it runs and its end wakes the engine thread, which would otherwise sleep for good. The decode fails.
TEST_F(NVJPGUserClient, ACancelledDecodeLetsTheCopyEngineQueuedBehindItEnd)
{
    for (const bool falconError : {false, true}) {
        Destination d(260, 0x9c000000);
        IOMemoryDescriptor dstMem;
        gCeCopies.clear();
        gEngine.released.clear();
        gEngine.stallDecodes = true;
        gEngine.falconErrorInDecode = falconError;
        EXPECT_EQ(engineRuns(copyRowsInto(d, dstMem)), kIOReturnIOError);
        gEngine.stallDecodes = false;
        gEngine.falconErrorInDecode = false;
        EXPECT_EQ(gEngine.decodeEndedBy, falconError ? "falcon" : "fence");
        ASSERT_EQ(gCeCopies.size(), 1u);
        EXPECT_EQ(gEngine.released, std::vector<uint32_t>{gCeCopies[0].wait.threshold}) << falconError;
        EXPECT_EQ(gCeCopies[0].wait.predicated, 1u) << "the re-pack runs only on its guard";
        EXPECT_EQ(guardOf(gCeCopies[0]), 0u)
            << "a cancelled decode's re-pack copies nothing: the buffer may hold another process's picture";
        EXPECT_TRUE(logged("engine reset: running again"));
        EXPECT_EQ(d.backing.refs, 1);
        retire(&d.backing);
    }
    EXPECT_EQ(gSurfaceAccessReleased, 2);
}

// HW Test's tile probe (image.nvjpg_tile_probe) tells NVJPG a tiling no driver uses. One the engine never finishes is
// bounded by its picture (50 ms and a microsecond per 16 pixels), cancelled once with its tiling named and counted for
// its process, and never run again; its destination's mapping runs the sink past every byte the worst reading of its
// tiling reaches; and the next decode runs.
TEST_F(NVJPGUserClient, AHungTileProbeIsCancelledOnceAndTheNextDecodeRuns)
{
    Destination d(64, 0x9d000000);
    gSurfaceTiled = 1;
    gSurfaceLayout = {256, 256, 4, 1024, 256, 4, 8192, 262144};
    Job *probe = decodeInto(d, 64 * 4096, 64 * 4096);
    probe->tileProbe = true;
    probe->tileMode = 1;
    probe->tileGob = 3;
    probe->plan.extent.rowBytes[0] = 1024;
    probe->plan.extent.rows[0] = 256;
    probe->engine.pixels = 256 * 256;
    probe->pid = 77;
    gCancelled.clear();
    const size_t streams = gEngine.streams.size();
    gEngine.stallDecodes = true;
    EXPECT_EQ(engineRuns(probe), kIOReturnIOError);
    gEngine.stallDecodes = false;
    EXPECT_EQ(gEngine.fenceBoundUs, 50000u + 256u * 256u / 16u);
    EXPECT_TRUE(logged("tile mode 1 block height 3"));
    EXPECT_TRUE(logged("engine reset: running again"));
    EXPECT_EQ(gCancelled, std::vector<int>{77});
    EXPECT_EQ(std::count_if(gEngine.streams.begin() + long(streams), gEngine.streams.end(),
                            [](const std::vector<uint32_t> &s) {
                                return s.size() > 3;
                            }),
              1)
        << "the probe's decode is sent once";
    EXPECT_EQ(gRoutes, std::vector<uint32_t>{TEGRA_NVJPG_ROUTE_TILE_PROBE});
    ASSERT_NE(d.mapping(), nullptr);
    EXPECT_GE(uint64_t(d.mapping()->mapped) * 4096, 256ull * 1024 * 64 + 1024ull * 2048);
    EXPECT_EQ(gHolds, 0);

    Destination next(4, 0x9e000000);
    gSurfaceTiled = 0;
    const size_t before = gEngine.streams.size();
    EXPECT_EQ(engineRuns(decodeInto(next, 4 * 4096, 4 * 4096)), kIOReturnIOError)
        << "the model's engine writes no status";
    EXPECT_EQ(gEngine.streams.size(), before + 1);
    EXPECT_EQ(gCancelled.size(), 1u);
    retire(&d.backing);
    retire(&next.backing);
}

// The same for VIC's re-pack of a '444f' decode, whose job renders its background before the release lets it run: the
// buffer may hold another process's picture
TEST_F(NVJPGUserClient, ACancelledDecodeLetsVICsRepackQueuedBehindItEnd)
{
    gVicBackgrounds.clear();
    gEngine.stallDecodes = true;
    EXPECT_EQ(engineRuns(repackOf(64 << 10)), kIOReturnIOError);
    gEngine.stallDecodes = false;
    EXPECT_EQ(gEngine.decodeEndedBy, "fence");
    EXPECT_EQ(gEngine.released, std::vector<uint32_t>{gVicAfter.threshold});
    EXPECT_EQ(gVicBackgrounds, std::vector<uint32_t>{kVicFence}) << "VIC's job writes none of the buffer";
    EXPECT_TRUE(logged("engine reset: running again"));
}

// A rebooted falcon's proof increments NVJPG's syncpoint past the cancelled fence, but a reboot whose falcon never
// loads sends no proof: only the fence's release lets the copy engine queued behind it run, so the engine thread wakes
// and a later decode can power the engine on.
TEST_F(NVJPGUserClient, ADecodeWhoseFalconDoesNotLoadAgainStillEndsItsQueuedRepack)
{
    Destination d(260, 0x9f000000);
    IOMemoryDescriptor dstMem;
    gEngine.stallDecodes = true;
    gEngine.scrubNeverEnds = true;
    EXPECT_EQ(engineRuns(copyRowsInto(d, dstMem)), kIOReturnIOError);
    gEngine.stallDecodes = false;
    gEngine.scrubNeverEnds = false;
    EXPECT_TRUE(logged("it did not come back, the next decode powers it on again: J1 FAIL IMEM/DMEM scrubbing"));
    ASSERT_EQ(gCeCopies.size(), 1u);
    EXPECT_EQ(gEngine.released, std::vector<uint32_t>{gCeCopies[0].wait.threshold});
    EXPECT_EQ(gSurfaceAccessReleased, 1);
    retire(&d.backing);
}

// The copy engine's end can come before NVJPG's fence callback (each on its own work loop): the engine thread finishes
// NVJPG's part at its own signal, then ends the decode, whichever signal came first.
TEST_F(NVJPGUserClient, ARepackThatEndsBeforeNVJPGsSignalStillEndsTheDecodeOnce)
{
    Destination d(260, 0x9e000000);
    IOMemoryDescriptor dstMem;
    EXPECT_EQ(engineRuns(copyRowsInto(d, dstMem), true), kIOReturnIOError) << "the model's engine writes no status";
    EXPECT_EQ(gEngine.decodeEndedBy, "fence");
    ASSERT_EQ(gCeCopies.size(), 1u);
    EXPECT_TRUE(gEngine.released.empty());
    EXPECT_EQ(gSurfaceAccessReleased, 1);
    retire(&d.backing);
}

// A decode whose fence could not be armed is cancelled inside its start, after its re-pack was queued: the start fails,
// and the re-pack, released with the fence, still ends before the decode does.
TEST_F(NVJPGUserClient, ADecodeWhoseFenceCannotBeArmedStillEndsItsQueuedRepack)
{
    Destination d(260, 0x9d000000);
    IOMemoryDescriptor dstMem;
    gEngine.stallDecodes = true;
    gEngine.armError = TEGRA_HOST1X_ENODEV;
    EXPECT_EQ(engineRuns(copyRowsInto(d, dstMem)), kIOReturnIOError);
    gEngine.stallDecodes = false;
    ASSERT_EQ(gCeCopies.size(), 1u);
    EXPECT_EQ(gEngine.released, std::vector<uint32_t>{gCeCopies[0].wait.threshold});
    EXPECT_EQ(gSurfaceAccessReleased, 1);
    retire(&d.backing);
}

// A GM20B block-linear destination the copy engine cannot reach: NVJPG writes rows only (the T210 wrote rows when told
// tile_mode 2), so the decode is refused, never written as rows into the blocks.
TEST_F(NVJPGUserClient, ABlockLinearDestinationTheCopyEngineCannotReachIsRefused)
{
    Destination d(72, 0x97000000);
    gSurfaceTiled = 1;
    gSurfaceLayout = {260, 260, 4, 1088, 264, 0, 4096, 287232};
    Job *job = decodeInto(d, 282880, 1088ULL * 272);
    job->request.base.out_width = 260;
    job->request.base.out_height = 260;
    const size_t streams = gEngine.streams.size();
    EXPECT_EQ(engineRuns(job), kIOReturnUnsupported);
    EXPECT_EQ(gEngine.streams.size(), streams) << "NVJPG never ran";
    ASSERT_EQ(gRefusals.size(), 1u);
    EXPECT_EQ(gRefusals.back(), "block-linear destination the copy engine cannot reach");
    EXPECT_TRUE(gCeCopies.empty());
    EXPECT_EQ(gHolds, 0);
    EXPECT_EQ(d.mapping(), nullptr);
    retire(&d.backing);
}

// A block-linear destination: NVJPG writes its rows into the service's scratch and the copy engine writes the blocks,
// with the layout held until it has
TEST_F(NVJPGUserClient, ABlockLinearDestinationIsWrittenByTheCopyEngine)
{
    Destination d(70, 0x98000000);  // 286,720 bytes < 287,232 BL0 block rows, >= 282,880 pitch picture
    IOMemoryDescriptor dstMem;
    gSurfaceTiled = 1;
    gSurfaceLayout = {260, 260, 4, 1088, 264, 0, 4096, 287232};
    Job *job = decodeInto(d, 282880, 1088ULL * 272);
    job->dst.memory = &dstMem;
    job->dst.desc.fourcc = TEGRA_NVJPG_FOURCC('B', 'G', 'R', 'A');
    job->dst.desc.width = 260;
    job->dst.desc.height = 260;
    job->dst.desc.planeBytesPerRow[0] = 1088;
    job->dst.desc.planeHeight[0] = 260;
    job->request.base.out_width = 260;
    job->request.base.out_height = 260;
    job->plan.extent.rowBytes[0] = 1088;
    job->plan.extent.rows[0] = 272;
    EXPECT_EQ(engineRuns(job), kIOReturnIOError);
    EXPECT_TRUE(gRefusals.empty());
    ASSERT_EQ(gCeCopies.size(), 1u);
    EXPECT_EQ(gCeCopies[0].wait.syncpt, kSyncpt);
    EXPECT_EQ(gCeCopies[0].wait.threshold, gEngine.syncpt);
    EXPECT_EQ(gCeCopies[0].rowBytes, 1040u);
    EXPECT_EQ(gCeCopies[0].rows, 260u);
    EXPECT_EQ(gCeCopies[0].dstBacking, &d.backing);
    EXPECT_EQ(gHoldsAtCopy, std::vector<int>{1})
        << "the layout stays held until the copy engine has written its blocks";
    EXPECT_EQ(gHolds, 0);
    EXPECT_EQ(gRoutes, std::vector<uint32_t>{TEGRA_NVJPG_ROUTE_COPY_BLOCKS});
    EXPECT_EQ(d.backing.refs, 1);
    retire(&d.backing);
}

// GM20B holds the destination block-linear but describes no layout of it: refused, never written as rows.
TEST_F(NVJPGUserClient, ADestinationDecidedBlockLinearWithNoLayoutDescribedIsRefused)
{
    Destination d(64, 0x99000000);
    gSurfaceTiled = 1;
    gSurfaceUndescribed = 1;
    Job *job = decodeInto(d, 64 * 4096, 64 * 4096);
    const size_t streams = gEngine.streams.size();
    EXPECT_EQ(engineRuns(job), kIOReturnUnsupported);
    EXPECT_EQ(gEngine.streams.size(), streams) << "NVJPG never ran";
    ASSERT_EQ(gRefusals.size(), 1u);
    EXPECT_EQ(gRefusals.back(), "destination decided block-linear, but GM20B describes no layout of it");
    EXPECT_EQ(gHolds, 0);
    EXPECT_EQ(d.mapping(), nullptr);
}

// A decode the planner staged for the copy engine (an IOSurface stride off 64 bytes) into a destination GM20B holds
// block-linear: the layout is asked and held like a direct decode's, and the copy engine writes its blocks whole.
static Job *stagedInto(Destination &d, IOMemoryDescriptor &memory, uint64_t copyDstStart)
{
    Job *job = decodeInto(d, 282880, 1088ULL * 264);
    job->dst.memory = &memory;
    job->plan.repack = TEGRA_NVJPG_REPACK_CE;
    job->plan.repackBytes = 1088ULL * 264;
    job->plan.target.pitch[0] = 1088;
    job->plan.copyDstStart[0] = copyDstStart;
    job->plan.copyDstPitch[0] = 1040;
    job->plan.copyRowBytes[0] = 1040;
    job->plan.copyRows[0] = 260;
    return job;
}

TEST_F(NVJPGUserClient, AStagedCopyIntoAWholeBlockLinearDestinationWritesItsBlocksUnderTheHold)
{
    Destination d(72, 0x9a000000);
    IOMemoryDescriptor memory;
    gSurfaceTiled = 1;
    gSurfaceLayout = {260, 260, 4, 1088, 264, 0, 4096, 287232};
    EXPECT_EQ(engineRuns(stagedInto(d, memory, 0)), kIOReturnIOError) << "the model's engine writes no status";
    EXPECT_TRUE(gRefusals.empty());
    ASSERT_EQ(gCeCopies.size(), 1u);
    EXPECT_EQ(gHoldsAtCopy, std::vector<int>{1});
    EXPECT_EQ(gHolds, 0);
    EXPECT_EQ(gRoutes, std::vector<uint32_t>{TEGRA_NVJPG_ROUTE_COPY_BLOCKS});
    retire(&d.backing);
}

TEST_F(NVJPGUserClient, AStagedCopyIntoPartOfABlockLinearDestinationIsRefused)
{
    Destination d(72, 0x9b000000);
    IOMemoryDescriptor memory;
    gSurfaceTiled = 1;
    gSurfaceLayout = {260, 260, 4, 1088, 264, 0, 4096, 287232};
    const size_t streams = gEngine.streams.size();
    EXPECT_EQ(engineRuns(stagedInto(d, memory, 64)), kIOReturnUnsupported);
    EXPECT_EQ(gEngine.streams.size(), streams);
    EXPECT_TRUE(gCeCopies.empty());
    ASSERT_EQ(gRefusals.size(), 1u);
    EXPECT_EQ(gRefusals.back(), "copy-engine re-pack into part of a block-linear destination");
    EXPECT_EQ(gHolds, 0);
    retire(&d.backing);
}
