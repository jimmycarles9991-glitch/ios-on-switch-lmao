// SwitchScratch.cpp against a stand-in descriptor that keeps XNU's ownership rule: a scratch for the kernel asked for on
// the thread of a task that may not move memory to the kernel (iconservicesagent's, SwitchHWTest's) is made and charged
// to that task instead of refused, the kernel's own threads and an entitled task still charge the kernel, and a kept
// scratch goes to whichever task takes it next.
#include "kits/SwitchScratch/SwitchScratch.h"

#include <gtest/gtest.h>

namespace {
switch_test_task gApp{"iconservicesagent", false}, gOther{"SwitchHWTest", false}, gCompositor{"backboardd", true};

class SwitchScratch : public ::testing::Test {
protected:
    void TearDown() override
    {
        gCurrentTask = kernel_task;
    }
};

// One size per test: a kept scratch serves sizes from half its own up, so no test takes another's
constexpr uint64_t kIcon = 110 * 110 * 8, kTarget = 720 * 1280 * 4, kLock = 4ULL << 20, kKernel = 1ULL << 20;

TEST_F(SwitchScratch, AKernelScratchOnAnUnentitledTasksThreadIsThatTasks)
{
    gCurrentTask = &gApp;
    switch_scratch *s = switch_scratch_get(kIcon, kernel_task);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->bytes, 128u << 10);
    EXPECT_EQ(s->npages, 32u);
    EXPECT_EQ(s->memory->owner, &gApp);
    EXPECT_EQ(s->memory->tag, kIOMemoryLedgerTagGraphics);
    EXPECT_TRUE(s->memory->prepared);
    switch_scratch_put(s);
    EXPECT_EQ(s->memory->owner, &gApp);
    EXPECT_EQ(s->memory->purgeable, kIOMemoryPurgeableVolatile);
}

TEST_F(SwitchScratch, TheKernelsThreadsAndAnEntitledTaskChargeTheKernel)
{
    switch_scratch *s = switch_scratch_get(kKernel, kernel_task);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->memory->owner, kernel_task);
    switch_scratch_put(s);

    gCurrentTask = &gCompositor;
    switch_scratch *t = switch_scratch_get(kTarget, kernel_task);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->memory->owner, kernel_task);
    switch_scratch_put(t);
    EXPECT_EQ(t->memory->owner, kernel_task);
}

TEST_F(SwitchScratch, AKeptScratchGoesToTheTaskThatTakesIt)
{
    gCurrentTask = &gApp;
    switch_scratch *s = switch_scratch_get(kLock, &gApp);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->memory->owner, &gApp);
    switch_scratch_put(s);
    gCurrentTask = &gOther;
    switch_scratch *again = switch_scratch_get(kLock, &gOther);
    ASSERT_EQ(again, s);
    EXPECT_EQ(again->memory->owner, &gOther);
    EXPECT_EQ(again->memory->purgeable, kIOMemoryPurgeableNonVolatile);
    switch_scratch_discard(again);
    uint64_t held = 0, peak = 0;
    switch_scratch_stats(&held, &peak);
    EXPECT_EQ(held, 0u);
}

// A lock that gives its rows back and locks again gets the same scratch, its rows kept unless the VM emptied it; one
// another get took meanwhile is not reclaimed, whatever it holds now.
TEST_F(SwitchScratch, AScratchGivenBackIsReclaimedOnlyIfNoOneTookItMeanwhile)
{
    constexpr uint64_t kRows = 6ULL << 20;
    gCurrentTask = &gApp;
    switch_scratch *s = switch_scratch_get(kRows, &gApp);
    ASSERT_NE(s, nullptr);
    const uint64_t handout = s->handout;
    switch_scratch_put(s);
    bool intact = false;
    ASSERT_EQ(switch_scratch_reclaim(s, handout, &gApp, &intact), s);
    EXPECT_TRUE(intact);
    EXPECT_EQ(s->memory->purgeable, kIOMemoryPurgeableNonVolatile);
    EXPECT_EQ(s->memory->owner, &gApp);
    EXPECT_NE(s->handout, handout);
    const uint64_t again = s->handout;
    switch_scratch_put(s);
    s->memory->purgeable = kIOMemoryPurgeableEmpty;
    ASSERT_EQ(switch_scratch_reclaim(s, again, &gApp, &intact), s);
    EXPECT_FALSE(intact);
    const uint64_t third = s->handout;
    switch_scratch_put(s);
    switch_scratch *other = switch_scratch_get(kRows, &gOther);
    ASSERT_EQ(other, s);
    EXPECT_EQ(switch_scratch_reclaim(s, third, &gApp, &intact), nullptr);
    switch_scratch_put(other);
    EXPECT_EQ(switch_scratch_reclaim(s, third, &gApp, &intact), nullptr);
    switch_scratch_discard(switch_scratch_get(kRows, &gApp));
}
}  // namespace
