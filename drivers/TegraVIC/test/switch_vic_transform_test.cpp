// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <vector>
#include "switch_vic_backing_stubs.h"
#include "../SwitchNativePicture.h"
#include "../SwitchVICTransform.cpp"
#include "../SwitchVICTurn.cpp"

struct SwitchVICMapping {
    uint64_t address;
};

namespace {
OSObject inputObject, outputObject;
IOMemoryDescriptor inputMemory, outputMemory;
switch_gpu_backing inputPitch, inputNative, outputPitch;
volatile uint64_t inputGeneration = 1, outputGeneration = 1, payloadGeneration = 1;
bool nativeInput = true, immediate = false;
int admission = 0, mappings = 0, gates = 0, pitchInputs = 0, notifications = 0;
void (*pending)(void *, int);
void *pendingContext;
TegraVICJob submittedJob;
std::vector<switch_gpu_backing_use> declaredUses;
std::vector<switch_gpu_backing_guard> declaredGuards;
}

extern "C" const TegraVICTraits *tegra_vic_traits()
{
    return &kTegraVICTraits;
}

extern "C" uint64_t tegra_vic_config_address()
{
    return 0x80001000;
}

extern "C" uint64_t tegra_vic_filter_address()
{
    return 0x80002000;
}

extern "C" int tegra_gm20b_surface_layout(uint64_t, tgm_layout *)
{
    return 0;
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *, switch_gpu_backing_info *info)
{
    static uint64_t pages[1] = {0x100000};
    *info = {};
    info->pages = pages;
    info->page_count = 1024;
}

int switch_vic_surface_access(const TegraVICSurface *surface, bool write, SwitchVICSurfaceAccess *access, bool)
{
    ++gates;
    pitchInputs += !write;
    access->descriptor = surface->memory;
    access->backing = write ? &outputPitch : &inputPitch;
    ++access->backing->refs;
    access->guard = {access->backing, write ? &outputGeneration : &inputGeneration, 1};
    return 0;
}

void switch_vic_surface_unreserve(SwitchVICSurfaceAccess *access)
{
    if (access->descriptor)
        --gates;
    access->descriptor = nullptr;
}

void switch_vic_surface_release_access(SwitchVICSurfaceAccess *access)
{
    switch_vic_surface_unreserve(access);
    if (access->backing)
        --access->backing->refs;
    *access = {};
}

int switch_native_surface_acquire(const TegraVICSurface *surface, SwitchNativeSurfaceView *view, bool, bool)
{
    if (!nativeInput)
        return -2;
    TegraVICPicture picture = {};
    picture.pitch = 1536;
    picture.rows = 736;
    picture.chromaOffset = 1536 * 736;
    picture.bytes = uint64_t(picture.chromaOffset) * 3 / 2;
    picture.blockHeightLog2 = 1;
    picture.width = 1280;
    picture.height = 720;
    picture.sampleBytes = 1;
    EXPECT_EQ(switch_native_picture(picture, surface->desc, view->layout, view->crop), nullptr);
    view->owner = &inputObject;
    view->native = &inputNative;
    view->logical = &inputPitch;
    view->guard = {&inputPitch, &payloadGeneration, 1};
    view->representation = {&inputPitch, &inputGeneration, 1};
    ++inputNative.refs;
    ++inputPitch.refs;
    return 0;
}

void switch_native_surface_release(SwitchNativeSurfaceView *view)
{
    if (view->owner) {
        --view->native->refs;
        --view->logical->refs;
    }
    *view = {};
}

SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *, bool write)
{
    ++mappings;
    return new SwitchVICMapping{write ? 0x50000000u : 0x40000000u};
}

void switch_vic_mapping_release(SwitchVICMapping *mapping)
{
    if (mapping) {
        --mappings;
        delete mapping;
    }
}

uint64_t switch_vic_mapping_address(const SwitchVICMapping *mapping)
{
    return mapping->address;
}

int switch_vic_submit_checked(const TegraVICJob *jobs, uint32_t count, const TegraVICAcquire *,
                              const SwitchVICOwnership &ownership, void (*completion)(void *, int), void *context,
                              uint32_t *, SwitchVICEvents *)
{
    EXPECT_EQ(count, 1u);
    EXPECT_GT(gates, 0);
    submittedJob = jobs[0];
    declaredUses.assign(ownership.uses, ownership.uses + ownership.useCount);
    declaredGuards.assign(ownership.guards, ownership.guards + ownership.guardCount);
    if (admission)
        return admission;
    if (immediate)
        completion(context, 0);
    else {
        pending = completion;
        pendingContext = context;
    }
    return 0;
}

namespace {
unsigned publishedTurns = 0;
}

SwitchVICEvents *publishedEvents = nullptr;

int switch_native_surface_publish_turned(const TegraVICSurface *, const SwitchNativeSurfaceView &source,
                                         const TegraVICOp &op, uint32_t, const char **, SwitchVICEvents *events)
{
    EXPECT_EQ(source.native, &inputNative);
    EXPECT_EQ(op.transform, 7u);
    ++publishedTurns;
    publishedEvents = events;
    return 0;
}

switch_gpu_backing *switch_vic_surface_backing(const TegraVICSurface *)
{
    return nullptr;
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
    ++backing->refs;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
    --backing->refs;
}

extern "C" int switch_vic_wait(uint32_t)
{
    return 0;
}

namespace {
class SwitchVICTransformTest : public testing::Test {
protected:
    TegraVICSurface source = {}, destination = {};
    TegraVICPlan plan = {};

    void SetUp() override
    {
        nativeInput = true;
        immediate = false;
        admission = mappings = gates = pitchInputs = notifications = 0;
        pending = nullptr;
        pendingContext = nullptr;
        declaredUses.clear();
        declaredGuards.clear();
        source.surface = &inputObject;
        source.memory = &inputMemory;
        source.desc.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
        source.desc.width = 1280;
        source.desc.height = 720;
        source.desc.planes = 2;
        source.desc.planeBytesPerRow[0] = source.desc.planeBytesPerRow[1] = 1536;
        source.desc.planeHeight[0] = 720;
        source.desc.planeHeight[1] = 360;
        source.desc.planeOffset[1] = 1536 * 720;
        source.desc.length = 1536 * 720 * 3 / 2;
        destination.surface = &outputObject;
        destination.memory = &outputMemory;
        destination.desc.fourcc = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
        destination.desc.width = 1280;
        destination.desc.height = 720;
        destination.desc.planes = 1;
        destination.desc.planeBytesPerRow[0] = 5120;
        destination.desc.planeHeight[0] = 720;
        destination.desc.length = 5120 * 720;
        plan.count = plan.readsSource = 1;
        plan.op[0].srcW = uint64_t(1280) << 16;
        plan.op[0].srcH = uint64_t(720) << 16;
        plan.op[0].dstW = 1280;
        plan.op[0].dstH = 720;
    }

    static void done(void *, int status)
    {
        EXPECT_EQ(status, 0);
        ++notifications;
    }

    int submit()
    {
        return switch_vic_transform(&source, &destination, &plan, 0, done, nullptr);
    }

    void TearDown() override
    {
        EXPECT_EQ(mappings, 0);
        EXPECT_EQ(gates, 0);
        EXPECT_EQ(inputPitch.refs, 1u);
        EXPECT_EQ(inputNative.refs, 1u);
        EXPECT_EQ(outputPitch.refs, 1u);
    }
};

TEST_F(SwitchVICTransformTest, NativeInputQueuesBothReadAuthoritiesWithoutPitchMaterialization)
{
    ASSERT_EQ(submit(), 1);
    ASSERT_NE(pending, nullptr);
    EXPECT_EQ(notifications, 0);
    EXPECT_EQ(pitchInputs, 0);
    EXPECT_EQ(gates, 0);
    EXPECT_EQ(mappings, 2);
    ASSERT_EQ(declaredUses.size(), 3u);
    EXPECT_EQ(declaredUses[0].backing, &outputPitch);
    EXPECT_EQ(declaredUses[0].access, uint32_t(SWITCH_GPU_BACKING_WRITE));
    EXPECT_EQ(declaredUses[1].backing, &inputNative);
    EXPECT_EQ(declaredUses[1].access, uint32_t(SWITCH_GPU_BACKING_READ));
    EXPECT_EQ(declaredUses[2].backing, &inputPitch);
    EXPECT_EQ(declaredGuards.size(), 3u);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE, SURF_BLK_KIND), 1u);
    pending(pendingContext, 0);
    EXPECT_EQ(notifications, 1);
}

// A job VIC cancelled at its bound, or that ran on a cancelled producer's memory, completes with the failure, which
// the IOSurfaceAccelerator client's transform hears (its async result: kIOReturnAborted).
TEST_F(SwitchVICTransformTest, ACancelledJobsFailureReachesTheTransformsCaller)
{
    static int heard;
    heard = 1;
    ASSERT_EQ(switch_vic_transform(
                  &source, &destination, &plan, 0,
                  [](void *, int status) {
                      heard = status;
                  },
                  nullptr),
              1);
    ASSERT_NE(pending, nullptr);
    pending(pendingContext, -5);
    EXPECT_EQ(heard, -5);
}

TEST_F(SwitchVICTransformTest, CompletionCanRaceSubmissionReturnWithoutFreeingCallerState)
{
    immediate = true;
    EXPECT_EQ(submit(), 1);
    EXPECT_EQ(notifications, 1);
}

TEST_F(SwitchVICTransformTest, StaleAdmissionReleasesReservationsWithoutSignallingCompletion)
{
    admission = SWITCH_GPU_BACKING_STALE;
    EXPECT_EQ(submit(), SWITCH_GPU_BACKING_STALE);
    EXPECT_EQ(notifications, 0);
    EXPECT_EQ(pending, nullptr);
}

TEST_F(SwitchVICTransformTest, PitchSourceRetainsTheOrdinaryTwoBackingRoute)
{
    nativeInput = false;
    immediate = true;
    EXPECT_EQ(submit(), 1);
    EXPECT_EQ(pitchInputs, 1);
    EXPECT_EQ(declaredUses.size(), 2u);
    EXPECT_EQ(declaredGuards.size(), 2u);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_SLOTS + TEGRA_VIC_SLOT_SURFACE, SURF_BLK_KIND), 0u);
}

// iPadOS 27's detached-video copy: a 1920x1080 '420v' a quarter turn into a 300x534 '422v', both on 256-byte rows, goes
// straight to VIC as one job writing the transposed 4:2:2 output.
TEST_F(SwitchVICTransformTest, DetachedVideoCopyInto422vRunsDirect)
{
    nativeInput = false;
    immediate = true;
    auto &s = source.desc;
    s.width = 1920;
    s.height = 1080;
    s.planeBytesPerRow[0] = s.planeBytesPerRow[1] = 2048;
    s.planeHeight[0] = 1080;
    s.planeHeight[1] = 540;
    s.planeOffset[1] = 2048 * 1080;
    s.length = 2048 * 1080 * 3 / 2;
    auto &d = destination.desc;
    d.fourcc = TEGRA_VIC_FOURCC('4', '2', '2', 'v');
    d.width = 300;
    d.height = 534;
    d.planes = 2;
    d.planeBytesPerRow[0] = d.planeBytesPerRow[1] = 512;
    d.planeHeight[0] = d.planeHeight[1] = 534;
    d.planeOffset[1] = 512 * 536;
    d.length = 512 * 536 * 2;
    plan.op[0].srcW = uint64_t(1920) << 16;
    plan.op[0].srcH = uint64_t(1080) << 16;
    plan.op[0].dstW = 300;
    plan.op[0].dstH = 534;
    plan.op[0].transform = TEGRA_VIC_ROT90;
    ASSERT_EQ(submit(), 1);
    EXPECT_EQ(notifications, 1);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_FORMAT),
              uint64_t(TEGRA_VIC_T_Y8___U8V8_N422R));
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT_SURFACE, SURF_CHROMA_WIDTH), 533u);
    EXPECT_EQ(TEGRA_VIC_GET(submittedJob.config, TEGRA_VIC_OUTPUT, OUT_TRANSPOSE), 1u);
}

TEST_F(SwitchVICTransformTest, OverlappingDescriptorFallsBackBeforeTakingReservations)
{
    source.memory = destination.memory;
    EXPECT_EQ(submit(), 0);
    EXPECT_TRUE(declaredUses.empty());
    EXPECT_EQ(notifications, 0);
}

// A landscape video's turn (SwitchVICTurn.h): a native picture a quarter turn onto all of a portrait '420v' is left to
// the display only once V3 has passed and only for a request with no events; anything else is copied.
class TurnDefer : public SwitchVICTransformTest {
protected:
    static constexpr SwitchDisplayTraits kProven = {false, true, false};

    void SetUp() override
    {
        SwitchVICTransformTest::SetUp();
        publishedTurns = 0;
        publishedEvents = nullptr;
        auto &d = destination.desc;
        d.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
        d.width = 720;
        d.height = 1280;
        d.planes = 2;
        d.planeBytesPerRow[0] = d.planeBytesPerRow[1] = 768;
        d.planeHeight[0] = 1280;
        d.planeHeight[1] = 640;
        d.planeOffset[1] = 768 * 1280;
        d.length = 768 * 1280 * 3 / 2;
        plan.op[0].dstW = 720;
        plan.op[0].dstH = 1280;
        plan.op[0].transform = 7;
    }

    int defer(SwitchVICEvents **events, const SwitchDisplayTraits &traits = kProven)
    {
        return switch_vic_turn_defer(&source, &destination, &plan, 0, events, traits);
    }
};

TEST_F(TurnDefer, AProvenTurnIsRecordedAndNothingIsSubmitted)
{
    EXPECT_EQ(defer(nullptr), 1);
    EXPECT_EQ(publishedTurns, 1u);
    EXPECT_EQ(pending, nullptr);
}

TEST_F(TurnDefer, BeforeV3PassesEveryTurnIsCopied)
{
    EXPECT_EQ(defer(nullptr, kSwitchDisplayTraits), 0);
    EXPECT_EQ(publishedTurns, 0u);
}

// The record takes the request's events (its waits order every reader, its signals go once): the job keeps none.
TEST_F(TurnDefer, ARequestWithEventsIsRecordedWithThem)
{
    auto *requested = reinterpret_cast<SwitchVICEvents *>(0x5e7);
    SwitchVICEvents *events = requested;
    EXPECT_EQ(defer(&events), 1);
    EXPECT_EQ(publishedTurns, 1u);
    EXPECT_EQ(publishedEvents, requested);
    EXPECT_EQ(events, nullptr);
    EXPECT_EQ(pending, nullptr);
}

TEST_F(TurnDefer, ARequestCopiedKeepsItsEvents)
{
    auto *requested = reinterpret_cast<SwitchVICEvents *>(0x5e7);
    SwitchVICEvents *events = requested;
    plan.op[0].transform = 0;
    EXPECT_EQ(defer(&events), 0);
    EXPECT_EQ(events, requested);
}

TEST_F(TurnDefer, APitchSourcePartOfTheDestinationOrNoTurnIsCopied)
{
    nativeInput = false;
    EXPECT_EQ(defer(nullptr), 0);
    nativeInput = true;
    plan.op[0].dstH = 1000;
    EXPECT_EQ(defer(nullptr), 0);
    plan.op[0].dstH = 1280;
    plan.op[0].transform = 0;
    EXPECT_EQ(defer(nullptr), 0);
    EXPECT_EQ(publishedTurns, 0u);
}
}  // namespace
