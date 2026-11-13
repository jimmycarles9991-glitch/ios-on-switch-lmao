// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <limits>
#include <vector>
#include "../raw/runtime/raw_batch.h"

TEST(PushBatches, ContiguousMergingPreservesExplicitWorkBoundaries)
{
    raw::PushBatches plan;
    plan.append({0x1000, 16, 0});
    plan.append({0x1040, 16, 0});
    plan.finishBatch();
    plan.append({0x1080, 16, 0});
    plan.append({0x10c0, 16, 0});
    ASSERT_EQ(plan.size(), 2u);
    EXPECT_EQ(plan[0].dwords, 32u);
    EXPECT_EQ(plan[1].dwords, 32u);
    EXPECT_EQ(plan.next(0, 1024), 1u);
    EXPECT_EQ(plan.next(1, 1024), 1u);
    EXPECT_TRUE(plan.valid(1024));
}

TEST(PushBatches, IndirectHeaderAndGpuOwnedArgumentsAreIndivisible)
{
    raw::PushBatches plan;
    plan.append({0x1000, 16, 0});
    plan.append({0x2000, 16, TegraGM20BPushIncomplete});
    plan.finishBatch();  // cannot expose an unfinished MME header
    plan.append({0x3000, 4, TegraGM20BPushNoPrefetch}, false);
    plan.finishBatch();
    plan.append({0x4000, 16, 0});
    EXPECT_EQ(plan.next(0, 2), 1u);
    EXPECT_EQ(plan.next(1, 2), 2u);
    EXPECT_EQ(plan.next(3, 2), 1u);
    EXPECT_TRUE(plan.valid(2));
    EXPECT_FALSE(plan.valid(1));
    EXPECT_FALSE(plan.valid(0));
    EXPECT_EQ(plan[2].flags, TegraGM20BPushNoPrefetch);
}

TEST(PushBatches, SlmAndSplitPrologueInsertionPreservesWorkCuts)
{
    raw::PushBatches plan;
    plan.append({0x1000, 64, 0});
    plan.finishBatch();
    plan.append({0x2000, 16, 0});
    const auto head = plan[0];
    plan[0].dwords = 8;
    plan.insert(1, {head.va + 32, head.dwords - 8, head.flags});
    plan.insert(1, {0x3000, 16, 0});
    plan.insert(0, {0x4000, 16, 0});
    ASSERT_EQ(plan.size(), 5u);
    EXPECT_EQ(plan.next(0, 1024), 4u);
    EXPECT_EQ(plan.next(4, 1024), 1u);
    EXPECT_EQ(plan[0].va, 0x4000u);
    EXPECT_EQ(plan[1].va, 0x1000u);
    EXPECT_EQ(plan[2].va, 0x3000u);
    EXPECT_EQ(plan[3].va, 0x1020u);
    EXPECT_TRUE(plan.valid(3));
}

TEST(PushBatches, FinalVisibilityAndStoresDoNotCreateAnExtraWorkBatch)
{
    raw::PushBatches plan;
    for (uint32_t i = 0; i < 3; ++i) {
        plan.append({0x1000ull + i * 0x100, 16, 0});
        plan.finishBatch();
    }
    plan.append({0x2000, 16, 0});
    plan.includeTail();
    EXPECT_EQ(plan.next(0, 1024), 1u);
    EXPECT_EQ(plan.next(1, 1024), 1u);
    EXPECT_EQ(plan.next(2, 1024), 2u);
}

TEST(PushBatches, RefusesIncompleteTailBeforeAnyPublication)
{
    raw::PushBatches plan;
    plan.append({0x1000, 16, 0});
    plan.finishBatch();
    plan.append({0x2000, 16, TegraGM20BPushIncomplete});
    EXPECT_FALSE(plan.valid(1024));
    plan.clear();
    EXPECT_TRUE(plan.valid(0));
    EXPECT_EQ(plan.next(0, 1024), 0u);
    plan.append({0x3000, 16, 0});
    plan.append({0x3040, 16, 0});
    EXPECT_EQ(plan.size(), 1u);
}

TEST(PushBatches, ManyCutsAndCapacitySplitsPreserveEveryWordExactlyOnce)
{
    raw::PushBatches plan;
    for (uint32_t i = 0; i < 5000; ++i) {
        plan.append({0x1000ull + i * 0x100, i + 1, 0}, false);
        if (i % 173 == 0)
            plan.finishBatch();
    }
    ASSERT_TRUE(plan.valid(127));
    uint64_t words = 0;
    for (size_t at = 0; at < plan.size();) {
        const size_t n = plan.next(at, 127);
        ASSERT_GT(n, 0u);
        ASSERT_LE(n, 127u);
        for (size_t k = at; k < at + n; ++k) {
            EXPECT_EQ(plan[k].va, 0x1000ull + k * 0x100);
            words += plan[k].dwords;
        }
        at += n;
    }
    EXPECT_EQ(words, 5000ull * 5001 / 2);
}

TEST(BatchWork, OverflowAndZeroDimensionsCannotWrapTheBudget)
{
    using W = raw::BatchWork;
    EXPECT_EQ(W::product(UINT64_MAX, UINT64_MAX), W::kSaturated);
    EXPECT_EQ(W::product(UINT64_MAX, 0), 0u);
    EXPECT_EQ(W::product(0, UINT64_MAX), 0u);
    EXPECT_EQ(W::product(1024, 1024), 1024u * 1024);
    W work;
    EXPECT_FALSE(work.add(W::kLimit - 1));
    EXPECT_TRUE(work.add(1));
    work = {};
    for (uint32_t i = 1; i < W::kOperations; ++i)
        EXPECT_FALSE(work.add(1));
    EXPECT_TRUE(work.add(1));
}

// A glass frame's draws as admission sees them: full-screen scissors at ~70 fragment instructions. A client's batch ends
// at each; the render server's holds sixteen, and a saturated estimate (an indirect draw) still ends it.
TEST(BatchWork, TheRenderServerCutsSixteenTimesLessOften)
{
    using W = raw::BatchWork;
    const uint64_t fullScreen = W::product(1280 * 720, 73);
    W client, server;
    EXPECT_TRUE(client.add(fullScreen));
    uint32_t draws = 0;
    while (!server.add(fullScreen, W::kRenderServerLimit))
        ++draws;
    EXPECT_EQ(draws, 15u);
    W indirect;
    EXPECT_TRUE(indirect.add(W::kSaturated, W::kRenderServerLimit));
}

namespace {
raw::PushBatches batches(std::initializer_list<uint32_t> sizes)
{
    raw::PushBatches plan;
    uint64_t va = 0x1000;
    for (uint32_t size : sizes) {
        for (uint32_t i = 0; i < size; ++i, va += 0x1000)
            plan.append({va, 16, 0}, false);
        plan.finishBatch();
    }
    return plan;
}

struct Submission {
    size_t at, n;
    std::vector<size_t> fences;
};

std::vector<Submission> submit(raw::PushBatches &plan, uint32_t maxPushes, uint32_t maxFences)
{
    std::vector<Submission> made;
    EXPECT_TRUE(plan.submit(maxPushes, maxFences, [&](size_t at, size_t n) {
        Submission s{at, n, {}};
        for (size_t k = at; k < at + n; ++k)
            if (plan[k].flags & TegraGM20BPushFence)
                s.fences.push_back(k);
        made.push_back(s);
        return true;
    }));
    return made;
}
}  // namespace

TEST(PushBatches, EveryBatchGoesInOneSubmissionWithAFenceAtEachBatchEnd)
{
    auto plan = batches({2, 1, 3});
    const auto made = submit(plan, 1024, TegraGM20BMaxJobFences);
    ASSERT_EQ(made.size(), 1u);
    EXPECT_EQ(made[0].at, 0u);
    EXPECT_EQ(made[0].n, 6u);
    EXPECT_EQ(made[0].fences, (std::vector<size_t>{1, 2}));
}

TEST(PushBatches, AKernelWithoutInternalFencesGetsASubmissionPerBatch)
{
    auto plan = batches({2, 1, 3});
    const auto made = submit(plan, 1024, 0);
    ASSERT_EQ(made.size(), 3u);
    EXPECT_EQ(made[0].n, 2u);
    EXPECT_EQ(made[1].n, 1u);
    EXPECT_EQ(made[2].n, 3u);
    for (const auto &s : made)
        EXPECT_TRUE(s.fences.empty());
}

TEST(PushBatches, MoreBatchesThanASubmissionsFencesContinueInTheNext)
{
    auto plan = batches({1, 1, 1, 1, 1});
    const auto made = submit(plan, 1024, 2);
    ASSERT_EQ(made.size(), 2u);
    EXPECT_EQ(made[0].n, 3u);
    EXPECT_EQ(made[0].fences, (std::vector<size_t>{0, 1}));
    EXPECT_EQ(made[1].at, 3u);
    EXPECT_EQ(made[1].n, 2u);
    EXPECT_EQ(made[1].fences, (std::vector<size_t>{3}));
}

TEST(PushBatches, AFenceNeverEndsASubmissionOrSplitsAnIndirectDraw)
{
    raw::PushBatches plan;
    plan.append({0x1000, 16, 0});
    plan.finishBatch();
    plan.append({0x2000, 16, TegraGM20BPushIncomplete});
    plan.finishBatch();
    plan.append({0x3000, 4, TegraGM20BPushNoPrefetch}, false);
    plan.finishBatch();
    plan.append({0x4000, 16, 0});
    const auto made = submit(plan, 1024, TegraGM20BMaxJobFences);
    ASSERT_EQ(made.size(), 1u);
    EXPECT_EQ(made[0].fences, (std::vector<size_t>{0, 2}));
    EXPECT_FALSE(plan[1].flags & TegraGM20BPushFence);
    EXPECT_FALSE(plan[3].flags & TegraGM20BPushFence);
}

TEST(PushBatches, CapacityStillSplitsAndCarriesEveryPushOnce)
{
    raw::PushBatches plan;
    for (uint32_t i = 0; i < 5000; ++i) {
        plan.append({0x1000ull + i * 0x100, i + 1, 0}, false);
        if (i % 173 == 0)
            plan.finishBatch();
    }
    size_t covered = 0;
    for (const auto &s : submit(plan, 127, 8)) {
        EXPECT_EQ(s.at, covered);
        EXPECT_LE(s.n, 127u);
        EXPECT_LE(s.fences.size(), 8u);
        if (!s.fences.empty())
            EXPECT_LT(s.fences.back(), s.at + s.n - 1);
        covered += s.n;
    }
    EXPECT_EQ(covered, plan.size());
}
