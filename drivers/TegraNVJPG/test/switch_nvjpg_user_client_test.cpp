// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVJPGUserClient.cpp's engine-thread decode (runJob) and TegraNVJPGRepack.cpp against the engine model
// (switch_nvjpg_engine_model.h), one translation unit with TegraNVJPGMapping.cpp. The model's kernel stand-ins declare
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
#include "../TegraNVJPGUserClient.cpp"

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

// The GPU driver's retirement: every engine's mapping destroyed, then the pages given back.
void retire(switch_gpu_backing *backing)
{
    if (backing->context)
        backing->destroy(backing->context);
    backing->context = nullptr;
    if (backing->releaseOwner)
        backing->releaseOwner(backing->owner);
}

// The retirement worker, run while the engine thread decodes: the first time the decode takes gStateLock with the job
// lock held.
void retireDuringTheRun(IOLock *lock)
{
    if (lock != gStateLock || !gRetiring || !gJobLock->locked)
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
    delete[] buffer->pages;
    delete[] buffer->va;
    *buffer = {};
}

bool switch_vic_decode_output_ready(void)
{
    return true;
}

// VIC's re-pack, completed at once: the test reads which fence it was queued behind.
int switch_vic_decode_output(const TegraVICSurface *, const SwitchVICDecodeOutput *output,
                             void (*completion)(void *, int), void *context, const char **)
{
    gVicAfter = *output->after;
    completion(context, 0);
    return 0;
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
    *tiled = 0;
    return nullptr;
}

extern "C" void tegra_gm20b_surface_unhold(void *)
{}

extern "C" int tegra_gm20b_surface_layout(uint64_t, struct tgm_layout *)
{
    return 0;
}

extern "C" int tegra_gm20b_surface_compressed(uint64_t)
{
    return 0;
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

// The engine thread's part of one queued job.
IOReturn engineRuns(Job *job)
{
    const IOReturn status = runJob(job);
    giveScan(job);
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
// destination while runJob's decode holds the job lock: it never waits for a decode.
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
