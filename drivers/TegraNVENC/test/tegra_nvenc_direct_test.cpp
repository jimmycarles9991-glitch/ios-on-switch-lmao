// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraNVENCDirect.cpp's ownership of a native picture NVENC reads in place, through its own begin and end over the
// real SwitchGpuOwnership.cpp: tegra_nvenc_run calls begin, writes and submits the job's stream, then calls end, which
// commits (or, for a job not queued, aborts) the transaction begin opened.
#include "drivers/TegraGM20B/test/switch_gpu_backing_fixture.h"
#include "drivers/TegraNVENC/TegraNVENCDirect.cpp"
#include <vector>

namespace {
constexpr uint32_t kSyncpt = 170;
std::vector<switch_host1x_fence *> gRetirements;

int nvencStatus(const switch_gpu_backing_token *token)
{
    return status(token);
}

// tegra_nvenc_run's work between begin and end (the stream written, the submit, the inputs' record) reuses the stack
// begin returned from.
__attribute__((noinline)) void runBetweenBeginAndEnd()
{
    volatile uint8_t stack[16384];
    for (auto &byte : stack)
        byte = 0;
}

struct Picture {
    switch_gpu_backing *native = backing(), *logical = backing();
    volatile uint64_t generation = 1, representation = 1;

    ~Picture()
    {
        switch_gpu_backing_release(native);
        switch_gpu_backing_release(logical);
    }

    TegraNVENCDirect direct()
    {
        TegraNVENCDirect d = {};
        d.view.native = native;
        d.view.logical = logical;
        d.view.guard = {logical, &generation, 1};
        d.view.representation = {native, &representation, 1};
        d.map = reinterpret_cast<void *>(0x1);
        return d;
    }
};

// What a GM20B job on queue 9 would wait for before writing `allocation`.
std::vector<switch_gpu_backing_token> writerWaits(switch_gpu_backing *allocation)
{
    switch_gpu_backing_use use = {allocation, SWITCH_GPU_BACKING_WRITE, 0};
    const auto completion = token(9, 100);
    switch_gpu_backing_transaction transaction = {};
    EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
    switch_gpu_backing_token waits[4] = {};
    uint32_t count = 0;
    EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, waits, 4, &count), 0);
    switch_gpu_backing_abort(&transaction);
    return {waits, waits + count};
}

class NVENCDirect : public Backing {
protected:
    void SetUp() override
    {
        Backing::SetUp();
        static bool registered;
        if (!registered) {
            const switch_gpu_backing_engine nvenc = {nvencStatus, wait, retain, release, 32};
            ASSERT_EQ(switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_NVENC, &nvenc), 0);
            registered = true;
        }
        gRetirements.clear();
    }

    void TearDown() override
    {
        for (auto *fence : gRetirements)
            fence->signaled(fence, 0);
        gRetirements.clear();
        Backing::TearDown();
    }
};
}  // namespace

extern "C" {
uint32_t tegra_nvenc_syncpt(void)
{
    return kSyncpt;
}

void tegra_nvenc_native_unmap(void *)
{}

void *tegra_nvenc_native_map(switch_gpu_backing *, uint64_t *, uint64_t *)
{
    return nullptr;
}

IOReturn tegra_vic_surface_lookup(uint32_t, task_t, TegraVICSurface *)
{
    return -1;
}

void tegra_vic_surface_release(TegraVICSurface *)
{}

void switch_host1x_fence_init(switch_host1x_fence *fence, uint32_t syncpt, uint32_t threshold,
                              void (*signaled)(switch_host1x_fence *, int))
{
    *fence = {};
    fence->syncpt = syncpt;
    fence->threshold = threshold;
    fence->signaled = signaled;
}

int switch_host1x_fence_add(switch_host1x_fence *fence, uint32_t)
{
    gRetirements.push_back(fence);
    return TEGRA_HOST1X_OK;
}
}

switch_gpu_backing_token tegra_nvenc_token(uint32_t fence)
{
    return {fence, 1, SWITCH_GPU_ENGINE_NVENC, kSyncpt};
}

int switch_native_surface_acquire(const TegraVICSurface *, SwitchNativeSurfaceView *, bool, bool)
{
    return -95;
}

void switch_native_surface_release(SwitchNativeSurfaceView *)
{}

// Boot 18 (T-v4b): the job's commit read the picture's backings from the use array begin had kept on its own frame,
// overwritten by then, and stored through a null access list. The commit records the read on the picture's backings
// and unlocks them, however the stack was used in between.
TEST_F(NVENCDirect, AQueuedReadCommitsOnThePicturesBackingsAfterTheStackBetweenIsReused)
{
    Picture picture;
    auto d = picture.direct();
    TegraNVENCAfter waits[TEGRA_NVENC_ORDER_WAITS] = {};
    uint32_t count = 99;
    ASSERT_EQ(begin(&d, 41, waits, &count), 0);
    EXPECT_EQ(count, 0u);
    runBetweenBeginAndEnd();
    end(&d, 41, true);
    EXPECT_EQ(d.map, nullptr) << "the mapping goes with the job's fence";
    ASSERT_EQ(gRetirements.size(), 1u);
    EXPECT_EQ(gRetirements[0]->threshold, 41u);
    for (auto *allocation : {picture.native, picture.logical}) {
        const auto waitsFor = writerWaits(allocation);
        ASSERT_EQ(waitsFor.size(), 1u) << "a later writer waits for NVENC's read";
        EXPECT_EQ(waitsFor[0].engine, uint32_t(SWITCH_GPU_ENGINE_NVENC));
        EXPECT_EQ(waitsFor[0].value, 41u);
    }
}

TEST_F(NVENCDirect, AJobNotQueuedAbortsAndLeavesNothingBehind)
{
    Picture picture;
    auto d = picture.direct();
    TegraNVENCAfter waits[TEGRA_NVENC_ORDER_WAITS] = {};
    uint32_t count = 0;
    ASSERT_EQ(begin(&d, 42, waits, &count), 0);
    runBetweenBeginAndEnd();
    end(&d, 42, false);
    EXPECT_TRUE(gRetirements.empty());
    EXPECT_TRUE(writerWaits(picture.native).empty());
    EXPECT_TRUE(writerWaits(picture.logical).empty());
}

// The picture's decode still running: the job's stream waits for it on NVDEC's syncpoint, and the commit still finds
// both backings.
TEST_F(NVENCDirect, APendingDecodeIsWaitedForAndTheReadStillCommits)
{
    Picture picture;
    const auto decode = token(162, 7, SWITCH_GPU_ENGINE_NVDEC);
    ASSERT_EQ(publish(picture.native, SWITCH_GPU_BACKING_WRITE, decode), 0);
    auto d = picture.direct();
    TegraNVENCAfter waits[TEGRA_NVENC_ORDER_WAITS] = {};
    uint32_t count = 0;
    ASSERT_EQ(begin(&d, 43, waits, &count), 0);
    ASSERT_EQ(count, 1u);
    EXPECT_EQ(waits[0].syncpt, 162u);
    EXPECT_EQ(waits[0].threshold, 7u);
    runBetweenBeginAndEnd();
    end(&d, 43, true);
    complete(decode);
    const auto waitsFor = writerWaits(picture.native);
    ASSERT_EQ(waitsFor.size(), 1u);
    EXPECT_EQ(waitsFor[0].engine, uint32_t(SWITCH_GPU_ENGINE_NVENC));
}
