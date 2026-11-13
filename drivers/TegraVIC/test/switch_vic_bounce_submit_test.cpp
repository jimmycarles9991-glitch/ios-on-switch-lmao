// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>
#include "switch_vic_backing_stubs.h"
#include "../SwitchVICBounceSubmit.cpp"

struct SwitchVICMapping {
    uint64_t address;
};

namespace {
OSObject sourceObject, destinationObject;
IOMemoryDescriptor sourceMemory, destinationMemory, scratchMemory[2];
switch_gpu_backing sourceBacking, destinationBacking, scratchBacking[2];
volatile uint64_t sourceGeneration = 1, destinationGeneration = 1;
SwitchVICMapping sourceMap{0x10000000}, destinationMap{0x20000000}, scratchMaps[2]{{0x30000000}, {0x40000000}};
SwitchVICBounceSlot pool;
std::vector<std::string> stages;
std::vector<uint32_t> guardCounts;
std::vector<std::pair<uint32_t, uint32_t>> copybackPlanes;  // bytes a row, rows
bool immediate = false;
int failStage = -1, gates = 0, callbacks = 0, result = 0, unreservesAtCallback = -1;

struct Callback {
    void (*fn)(void *, int);
    void *context;
};

std::vector<Callback> pending;

TegraVICSurface makeSurface(bool source, uint32_t pitch)
{
    TegraVICSurface surface = {};
    surface.surface = source ? &sourceObject : &destinationObject;
    surface.memory = source ? &sourceMemory : &destinationMemory;
    auto &d = surface.desc;
    d.fourcc = TEGRA_VIC_FOURCC('B', 'G', 'R', 'A');
    d.width = 32;
    d.height = 16;
    d.planeBytesPerRow[0] = pitch;
    d.planeHeight[0] = 16;
    d.length = uint64_t(pitch) * 16;
    return surface;
}

void notify(void *, int status)
{
    ++callbacks;
    result = status;
    unreservesAtCallback = gates;
}

void finish()
{
    while (!pending.empty()) {
        auto next = pending.back();
        pending.pop_back();
        next.fn(next.context, 0);
    }
}

void accept(void (*done)(void *, int), void *context)
{
    if (!done)
        return;
    if (immediate)
        done(context, 0);
    else
        pending.push_back({done, context});
}

class Bounce : public testing::Test {
protected:
    void SetUp() override
    {
        stages.clear();
        guardCounts.clear();
        copybackPlanes.clear();
        failStage = -1;
        immediate = false;
        gates = callbacks = result = 0;
        unreservesAtCallback = -1;
        pending.clear();
        pool = {};
        sourceBacking.refs = destinationBacking.refs = 1;
    }

    void TearDown() override
    {
        EXPECT_EQ(gates, 0);
        EXPECT_FALSE(pool.busy);
        EXPECT_TRUE(pending.empty());
        EXPECT_EQ(sourceBacking.refs, 1u);
        EXPECT_EQ(destinationBacking.refs, 1u);
    }

    TegraVICPlan plan(bool full = true)
    {
        TegraVICPlan p = {};
        p.count = 1;
        p.readsSource = 1;
        p.op[0].srcW = 32ull << 16;
        p.op[0].srcH = 16ull << 16;
        p.op[0].dstW = full ? 32 : 16;
        p.op[0].dstH = 16;
        return p;
    }
};
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
    info->page_count = 1;
}

int switch_vic_surface_access(const TegraVICSurface *s, bool, SwitchVICSurfaceAccess *a, bool)
{
    ++gates;
    const bool source = s->memory == &sourceMemory;
    a->descriptor = s->memory;
    a->backing = source ? &sourceBacking : &destinationBacking;
    ++a->backing->refs;
    a->guard = {a->backing, source ? &sourceGeneration : &destinationGeneration, 1};
    return 0;
}

void switch_vic_surface_unreserve(SwitchVICSurfaceAccess *a)
{
    if (a->descriptor)
        --gates;
    a->descriptor = nullptr;
}

void switch_vic_surface_release_access(SwitchVICSurfaceAccess *a)
{
    switch_vic_surface_unreserve(a);
    if (a->backing)
        --a->backing->refs;
    *a = {};
}

int switch_native_surface_acquire(const TegraVICSurface *, SwitchNativeSurfaceView *, bool, bool)
{
    return -2;
}

void switch_native_surface_release(SwitchNativeSurfaceView *)
{}

SwitchVICMapping *switch_vic_mapping_acquire(switch_gpu_backing *b, bool)
{
    return b == &sourceBacking ? &sourceMap : &destinationMap;
}

void switch_vic_mapping_release(SwitchVICMapping *)
{}

uint64_t switch_vic_mapping_address(const SwitchVICMapping *m)
{
    return m->address;
}

int switch_vic_bounce_acquire(const TegraVICSurfaceDesc *src, const TegraVICSurfaceDesc *dst, SwitchVICBounceSlot **out)
{
    EXPECT_FALSE(pool.busy);
    pool.busy = true;
    const TegraVICSurfaceDesc *descs[] = {src, dst};
    for (unsigned i = 0; i < 2; ++i)
        if (descs[i]) {
            auto &b = pool.buffers[i];
            b.memory = &scratchMemory[i];
            b.backing = &scratchBacking[i];
            b.mapping = &scratchMaps[i];
            tegra_vic_bounce_desc(descs[i], 256, &b.desc);
        }
    *out = &pool;
    return 0;
}

void switch_vic_bounce_release(SwitchVICBounceSlot *slot)
{
    if (slot)
        slot->busy = false;
}

void switch_vic_events_submission(SwitchVICEvents *, bool, switch_gpu_event_submission *out)
{
    *out = {};
}

int switch_vic_events_stage(SwitchVICEvents *, SwitchVICEvents **out)
{
    *out = nullptr;
    return 0;
}

void switch_vic_events_commit(SwitchVICEvents *)
{}

void switch_vic_events_release(SwitchVICEvents **)
{}

extern "C" int tegra_gm20b_ce_copy_plane_checked(const tegra_gm20b_ce_plane *, const tegra_gm20b_ce_plane *,
                                                 uint32_t rowBytes, uint32_t rows, const tegra_gm20b_ce_wait *,
                                                 const tegra_gm20b_ce_done *done, switch_gpu_backing *src, uint64_t,
                                                 switch_gpu_backing *dst, uint64_t,
                                                 const switch_gpu_copy_ownership *ownership,
                                                 const switch_gpu_event_submission *)
{
    stages.push_back(src == &scratchBacking[1] ? "copyback" : dst == &scratchBacking[1] ? "preserve" : "source");
    if (src == &scratchBacking[1])
        copybackPlanes.emplace_back(rowBytes, rows);
    guardCounts.push_back(ownership->guard_count);
    for (uint32_t i = 0; i < ownership->guard_count; ++i)
        EXPECT_TRUE(ownership->guards[i].backing == src || ownership->guards[i].backing == dst);
    if (int(stages.size()) == failStage)
        return -5;
    if (done)
        accept(done->fn, done->arg);
    return 0;
}

int switch_vic_submit_checked(const TegraVICJob *, uint32_t, const TegraVICAcquire *,
                              const SwitchVICOwnership &ownership, void (*done)(void *, int), void *context, uint32_t *,
                              SwitchVICEvents *)
{
    stages.push_back("vic");
    for (uint32_t i = 0; i < ownership.guardCount; ++i) {
        bool found = false;
        for (uint32_t j = 0; j < ownership.useCount; ++j)
            found |= ownership.guards[i].backing == ownership.uses[j].backing;
        EXPECT_TRUE(found);
    }
    if (int(stages.size()) == failStage)
        return -5;
    accept(done, context);
    return 0;
}

TEST_F(Bounce, FullOverwriteQueuesSourceVicAndCopybackWithoutPreservingDestination)
{
    auto src = makeSurface(true, 192), dst = makeSurface(false, 160);
    auto p = plan();
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    EXPECT_EQ(stages, (std::vector<std::string>{"source", "vic", "copyback"}));
    EXPECT_EQ(callbacks, 0);
    EXPECT_TRUE(pool.busy);
    finish();
    EXPECT_EQ(callbacks, 1);
}

TEST_F(Bounce, OnlyTheSourceBouncesAtA64ByteMultiplePitch)
{
    auto src = makeSurface(true, 192), dst = makeSurface(false, 256);
    auto p = plan();
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    EXPECT_EQ(stages, (std::vector<std::string>{"source", "vic"}));
    finish();
    stages.clear();
    src = makeSurface(true, 256);
    dst = makeSurface(false, 192);
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    EXPECT_EQ(stages, (std::vector<std::string>{"vic"}));
    finish();
}

TEST_F(Bounce, PartialWritePreservesDestinationBeforeVic)
{
    auto src = makeSurface(true, 192), dst = makeSurface(false, 160);
    auto p = plan(false);
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    EXPECT_EQ(stages, (std::vector<std::string>{"source", "preserve", "vic", "copyback"}));
    finish();
}

TEST_F(Bounce, FastFinalCallbackRunsAfterAllAdmissionOwnershipIsReleased)
{
    immediate = true;
    auto src = makeSurface(true, 192), dst = makeSurface(false, 160);
    auto p = plan();
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(unreservesAtCallback, 0);
}

TEST_F(Bounce, FailedAcceptedPrefixRetiresBeforeReportingFailure)
{
    failStage = 2;
    auto src = makeSurface(true, 192), dst = makeSurface(false, 160);
    auto p = plan();
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    EXPECT_EQ(callbacks, 0);
    EXPECT_TRUE(pool.busy);
    finish();
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(result, -5);
}

TEST_F(Bounce, InPlaceRequestSnapshotsSourceBeforeWritingOriginal)
{
    auto src = makeSurface(true, 256);
    auto p = plan();
    ASSERT_EQ(switch_vic_bounce_submit(&src, &src, &p, 0, notify, nullptr, nullptr), 1);
    EXPECT_EQ(stages, (std::vector<std::string>{"source", "vic"}));
    finish();
}

TEST_F(Bounce, FinalCopyAdmissionFailureJoinsEarlierVicAndCopy)
{
    failStage = 3;
    auto src = makeSurface(true, 192), dst = makeSurface(false, 160);
    auto p = plan();
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    EXPECT_EQ(callbacks, 0);
    ASSERT_EQ(pending.size(), 2u);
    auto vic = pending.back();
    pending.pop_back();
    vic.fn(vic.context, 0);
    EXPECT_TRUE(pool.busy);
    EXPECT_EQ(callbacks, 0);
    finish();
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(result, -5);
}

TEST_F(Bounce, FullFillSkipsAllReadsAndDestinationPreservation)
{
    auto dst = makeSurface(false, 160);
    auto p = plan();
    p.readsSource = 0;
    p.op[0].fill = 1;
    ASSERT_EQ(switch_vic_bounce_submit(nullptr, &dst, &p, 0, notify, nullptr, nullptr), 1);
    EXPECT_EQ(stages, (std::vector<std::string>{"vic", "copyback"}));
    finish();
}

TEST_F(Bounce, FirstStageRefusalLeavesCompletionWithCaller)
{
    failStage = 1;
    auto src = makeSurface(true, 192), dst = makeSurface(false, 160);
    auto p = plan();
    EXPECT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), -5);
    EXPECT_EQ(callbacks, 0);
    EXPECT_TRUE(pending.empty());
}

// A copy the copy engine accepted and the GPU then failed (a killed channel's jobs report -5 through their timeline)
// fails the job that depends on it, from that copy's own completion, once everything the job queued has retired. The
// next job is not touched.
TEST_F(Bounce, AFillTheGpuFailsFailsItsJobAndNotTheNext)
{
    auto src = makeSurface(true, 192), dst = makeSurface(false, 160);
    auto p = plan();
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    ASSERT_EQ(stages, (std::vector<std::string>{"source", "vic", "copyback"}));
    auto fill = pending.front();
    pending.erase(pending.begin());
    fill.fn(fill.context, -5);
    EXPECT_EQ(callbacks, 0);
    EXPECT_TRUE(pool.busy);
    finish();
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(result, -5);
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    finish();
    EXPECT_EQ(callbacks, 2);
    EXPECT_EQ(result, 0);
}

TEST_F(Bounce, ACopyBackTheGpuFailsFailsItsJob)
{
    auto src = makeSurface(true, 192), dst = makeSurface(false, 160);
    auto p = plan();
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    auto copyback = pending.back();
    pending.pop_back();
    finish();
    EXPECT_EQ(callbacks, 0);
    copyback.fn(copyback.context, -5);
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(result, -5);
}

// iPadOS 27's detached-video copy into a '422v' on CoreVideo's 64-byte rows: VIC writes the scratch, then both planes
// come back whole, the chroma plane a row for every luma row.
TEST_F(Bounce, DetachedVideoCopyInto422vComesBackWithEveryChromaRow)
{
    TegraVICSurface src = {};
    src.surface = &sourceObject;
    src.memory = &sourceMemory;
    auto &s = src.desc;
    s.fourcc = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
    s.width = 1920;
    s.height = 1080;
    s.planes = 2;
    s.planeBytesPerRow[0] = s.planeBytesPerRow[1] = 2048;
    s.planeHeight[0] = 1080;
    s.planeHeight[1] = 540;
    s.planeOffset[1] = 2048 * 1080;
    s.length = 2048 * 1080 * 3 / 2;
    TegraVICSurface dst = {};
    dst.surface = &destinationObject;
    dst.memory = &destinationMemory;
    auto &d = dst.desc;
    d.fourcc = TEGRA_VIC_FOURCC('4', '2', '2', 'v');
    d.width = 300;
    d.height = 534;
    d.planes = 2;
    d.planeBytesPerRow[0] = d.planeBytesPerRow[1] = 320;
    d.planeHeight[0] = d.planeHeight[1] = 534;
    d.planeOffset[1] = 320 * 534;
    d.length = 320 * 534 * 2;
    TegraVICPlan p = {};
    p.count = 1;
    p.readsSource = 1;
    p.op[0].srcW = 1920ull << 16;
    p.op[0].srcH = 1080ull << 16;
    p.op[0].dstW = 300;
    p.op[0].dstH = 534;
    p.op[0].transform = TEGRA_VIC_ROT90;
    ASSERT_EQ(switch_vic_bounce_submit(&src, &dst, &p, 0, notify, nullptr, nullptr), 1);
    EXPECT_EQ(stages, (std::vector<std::string>{"vic", "copyback", "copyback"}));
    EXPECT_EQ(copybackPlanes, (std::vector<std::pair<uint32_t, uint32_t>>{{300, 534}, {300, 534}}));
    EXPECT_EQ(pool.buffers[1].desc.planeBytesPerRow[1], 512u);
    finish();
    EXPECT_EQ(callbacks, 1);
    EXPECT_EQ(result, 0);
}
