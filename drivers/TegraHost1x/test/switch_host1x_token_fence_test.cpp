// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_host1x_token_fence_stubs.h"
#include <gtest/gtest.h>
#include <map>
#include <tuple>
#include <vector>
#include "../SwitchHost1xTokenFence.cpp"
#include "../SwitchHost1xResults.h"

namespace {
using Key = std::tuple<uint32_t, uint32_t, uint64_t>;
std::map<Key, int> gStatus, gPhysical;
int (*gStatusOf)(const switch_gpu_backing_token *), (*gPhysicalOf)(const switch_gpu_backing_token *);
std::vector<switch_host1x_fence *> gAdded;
uint32_t gSyncpoint[192];
int gAddError;
uint32_t gAddsBeforeError = UINT32_MAX;

Key key(const switch_gpu_backing_token &token)
{
    return {token.engine, token.queue, token.value};
}

struct Heard {
    int calls = 0, status = 1;
};

void heard(void *context, int status)
{
    auto *h = static_cast<Heard *>(context);
    ++h->calls;
    h->status = status;
}

// host1x's work loop delivering a fence: its threshold reached, or its failure.
void deliver(size_t which, int status = 0)
{
    auto *fence = gAdded.at(which);
    fence->state = SWITCH_HOST1X_FENCE_IDLE;
    fence->signaled(fence, status);
}
}  // namespace

extern "C" int switch_gpu_backing_token_status(const switch_gpu_backing_token *token)
{
    return gStatusOf ? gStatusOf(token) : gStatus[key(*token)];
}

extern "C" int switch_gpu_backing_token_physical_status(const switch_gpu_backing_token *token, int external)
{
    EXPECT_EQ(external, 0);
    return gPhysicalOf ? gPhysicalOf(token) : gPhysical[key(*token)];
}

extern "C" uint32_t tegra_host1x_syncpt_read(uint32_t id)
{
    return gSyncpoint[id];
}

extern "C" void switch_host1x_fence_init(switch_host1x_fence *fence, uint32_t syncpt, uint32_t threshold,
                                         void (*signaled)(switch_host1x_fence *, int))
{
    *fence = {};
    fence->syncpt = syncpt;
    fence->threshold = threshold;
    fence->signaled = signaled;
}

extern "C" int switch_host1x_fence_add(switch_host1x_fence *fence, uint32_t boundUs)
{
    EXPECT_EQ(boundUs, 0u);
    if (!gAddsBeforeError--)
        return gAddError;
    fence->state = SWITCH_HOST1X_FENCE_QUEUED;
    gAdded.push_back(fence);
    return 0;
}

namespace {
struct TokenFence : testing::Test {
    void SetUp() override
    {
        gStatus.clear();
        gPhysical.clear();
        gStatusOf = gPhysicalOf = nullptr;
        gAdded.clear();
        gAddError = 0;
        gAddsBeforeError = UINT32_MAX;
    }

    void TearDown() override
    {
        for (auto &join : gJoins)
            EXPECT_FALSE(join.used);
    }
};

const switch_gpu_backing_token kDecode = {0x100000007ULL, 1, SWITCH_GPU_ENGINE_NVDEC, 162};
const switch_gpu_backing_token kConvert = {0x200000009ULL, 1, SWITCH_GPU_ENGINE_VIC, 161};

TEST_F(TokenFence, AVicFrameFlipsFromItsSyncpointInterruptAtTheJobsThreshold)
{
    Heard h;
    ASSERT_EQ(switch_host1x_token_fence(&kConvert, 1, heard, &h), 0);
    ASSERT_EQ(gAdded.size(), 1u);
    EXPECT_EQ(gAdded[0]->syncpt, 161u);
    EXPECT_EQ(gAdded[0]->threshold, 9u);
    EXPECT_EQ(h.calls, 0);
    deliver(0);
    EXPECT_EQ(h.calls, 1);
    EXPECT_EQ(h.status, 0);
}

TEST_F(TokenFence, TheFlipWaitsForEveryProducer)
{
    const switch_gpu_backing_token both[] = {kDecode, kConvert};
    Heard h;
    ASSERT_EQ(switch_host1x_token_fence(both, 2, heard, &h), 0);
    ASSERT_EQ(gAdded.size(), 2u);
    EXPECT_EQ(gAdded[0]->syncpt, 162u);
    EXPECT_EQ(gAdded[0]->threshold, 7u);
    deliver(1);
    EXPECT_EQ(h.calls, 0);
    deliver(0);
    EXPECT_EQ(h.calls, 1);
    EXPECT_EQ(h.status, 0);
}

TEST_F(TokenFence, AGpuProducerIsTheBridgesNotThisFences)
{
    const switch_gpu_backing_token gpu = {5, 1, SWITCH_GPU_ENGINE_GM20B, 3};
    Heard h;
    EXPECT_EQ(switch_host1x_token_fence(&gpu, 1, heard, &h), -22);
    EXPECT_TRUE(gAdded.empty());
}

TEST_F(TokenFence, AStaleOrFailedProducerIsRefusedWithNoCallback)
{
    Heard h;
    gStatus[key(kConvert)] = SWITCH_GPU_BACKING_STALE;
    EXPECT_EQ(switch_host1x_token_fence(&kConvert, 1, heard, &h), SWITCH_GPU_BACKING_STALE);
    gStatus[key(kConvert)] = -5;
    EXPECT_EQ(switch_host1x_token_fence(&kConvert, 1, heard, &h), -5);
    EXPECT_TRUE(gAdded.empty());
    EXPECT_EQ(h.calls, 0);
}

// A finished producer still answers on the work loop, never inside the call, on a threshold already reached.
TEST_F(TokenFence, AFinishedProducerIsArmedAtTheCurrentValue)
{
    gStatus[key(kDecode)] = 1;
    gSyncpoint[162] = 0x1234;
    Heard h;
    ASSERT_EQ(switch_host1x_token_fence(&kDecode, 1, heard, &h), 0);
    EXPECT_EQ(gAdded.at(0)->threshold, 0x1234u);
    EXPECT_EQ(h.calls, 0);
    deliver(0);
    EXPECT_EQ(h.calls, 1);
}

TEST_F(TokenFence, AFailureKnownWhenTheLastFenceLandsIsPassedOn)
{
    Heard h;
    ASSERT_EQ(switch_host1x_token_fence(&kConvert, 1, heard, &h), 0);
    gStatus[key(kConvert)] = -125;
    deliver(0);
    EXPECT_EQ(h.status, -125);

    ASSERT_EQ(switch_host1x_token_fence(&kDecode, 1, heard, &h), 0);
    deliver(1, TEGRA_HOST1X_ETIMEDOUT);
    EXPECT_EQ(h.status, TEGRA_HOST1X_ETIMEDOUT);
}

TEST_F(TokenFence, Host1xDownRefusesTheFlipAndAFenceRefusedLaterFailsItOnTheWorkLoop)
{
    Heard h;
    gAddError = TEGRA_HOST1X_ENODEV;
    gAddsBeforeError = 0;
    EXPECT_EQ(switch_host1x_token_fence(&kConvert, 1, heard, &h), TEGRA_HOST1X_ENODEV);
    const switch_gpu_backing_token both[] = {kDecode, kConvert};
    gAddsBeforeError = 1;
    ASSERT_EQ(switch_host1x_token_fence(both, 2, heard, &h), 0);
    EXPECT_EQ(h.calls, 0);
    deliver(0);
    EXPECT_EQ(h.calls, 1);
    EXPECT_EQ(h.status, TEGRA_HOST1X_ENODEV);
}

TEST_F(TokenFence, EveryFlipSlotInUseIsBusy)
{
    Heard h;
    for (uint32_t i = 0; i < kJoins; ++i)
        ASSERT_EQ(switch_host1x_token_fence(&kConvert, 1, heard, &h), 0);
    EXPECT_EQ(switch_host1x_token_fence(&kConvert, 1, heard, &h), -16);
    for (size_t i = 0; i < gAdded.size(); ++i)
        deliver(i);
    EXPECT_EQ(h.calls, static_cast<int>(kJoins));
}

// A producer its engine cancelled is recorded failed before the CPU releases its syncpoint; its result waits for its
// semantic outcome, which is retired later on another thread. The release that reaches the flip's fence fails it.
TEST_F(TokenFence, ACancelledProducersReleaseFailsTheFlipBeforeItsResultIsFiled)
{
    Heard h;
    ASSERT_EQ(switch_host1x_token_fence(&kDecode, 1, heard, &h), 0);
    gPhysical[key(kDecode)] = -5;
    deliver(0);
    EXPECT_EQ(h.calls, 1);
    EXPECT_EQ(h.status, -5);
}

// VIC's job that ran on a cancelled decode's picture (the overlay composite, a placement) has that failure as its
// hardware outcome when its own fence comes in, ahead of the flip's on VIC's syncpoint.
TEST_F(TokenFence, AVicJobThatRanOnACancelledDecodeFailsTheFlip)
{
    const switch_gpu_backing_token both[] = {kDecode, kConvert};
    Heard h;
    ASSERT_EQ(switch_host1x_token_fence(both, 2, heard, &h), 0);
    deliver(0);
    gPhysical[key(kConvert)] = -5;
    deliver(1);
    EXPECT_EQ(h.calls, 1);
    EXPECT_EQ(h.status, -5);
}

TEST_F(TokenFence, AProducerCancelledBeforeTheFlipIsArmedIsRefused)
{
    Heard h;
    gPhysical[key(kConvert)] = -5;
    EXPECT_EQ(switch_host1x_token_fence(&kConvert, 1, heard, &h), -5);
    EXPECT_TRUE(gAdded.empty());
    EXPECT_EQ(h.calls, 0);
}

SwitchHost1xResultState gDecodeState;
using DecodeResults = SwitchHost1xResults<SWITCH_GPU_ENGINE_NVDEC, gDecodeState>;

// The same over NVDEC's own records: switch_nvdec_jobs_cancel records the job failed (physical), then the CPU releases
// the syncpoint, while the job's result is filed only with its semantic outcome.
TEST_F(TokenFence, TheReleaseOfACancelledDecodeFailsItsFlipOverTheEnginesRecords)
{
    ASSERT_TRUE(DecodeResults::start(162));
    gStatusOf = DecodeResults::status;
    gPhysicalOf = DecodeResults::physicalStatus;
    auto *record = DecodeResults::allocate();
    switch_gpu_backing_token decode = {};
    ASSERT_EQ(DecodeResults::prepare(record, 41, 41, &decode), 0);
    Heard h;
    ASSERT_EQ(switch_host1x_token_fence(&decode, 1, heard, &h), 0);
    ASSERT_EQ(gAdded.size(), 1u);
    EXPECT_EQ(gAdded[0]->syncpt, 162u);
    EXPECT_EQ(gAdded[0]->threshold, 41u);
    DecodeResults::physical(record, -5);
    EXPECT_EQ(DecodeResults::status(&decode), 0) << "the result waits for the semantic outcome";
    deliver(0);
    EXPECT_EQ(h.calls, 1);
    EXPECT_EQ(h.status, -5);
    DecodeResults::complete(&record, -5);
}
}  // namespace
