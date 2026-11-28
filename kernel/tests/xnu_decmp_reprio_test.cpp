// SPDX-License-Identifier: GPL-2.0-only
// vm_decmp_upl_reprioritize from both trees: the busy-page hook's form (Z_NOWAIT) never makes an allocation that waits,
// and an allocation that fails drops only its own raise; Apple's form (Z_WAITOK) still raises every block.
#include <gtest/gtest.h>

#include <cstdint>

#include "xnu_decmp_reprio_harness.h"

namespace {

constexpr uint64_t Block(uint64_t blkno, uint32_t pages)
{
    return blkno | static_cast<uint64_t>(pages * 16384u) << 32;
}

// Three blocks of one page each in a three-page real I/O UPL.
constexpr uint64_t kInfo[] = {Block(100, 1), Block(200, 1), Block(300, 1)};

class DecmpReprio : public testing::TestWithParam<int> {};

TEST_P(DecmpReprio, TheHooksRaiseNeverWaitsForMemory)
{
    xnu_decmp_reprio_run run = {};
    xnu_decmp_reprio(GetParam(), 1, kInfo, 3, &run);
    EXPECT_EQ(run.blocking_allocs, 0u);
    EXPECT_EQ(run.nowait_allocs, 4u);
    ASSERT_EQ(run.enqueued, 3u);
    EXPECT_EQ(run.enqueued_blknos[0], 100u);
    EXPECT_EQ(run.enqueued_blknos[2], 300u);
}

TEST_P(DecmpReprio, ARequestThatFindsNoMemoryDropsOnlyItsRaise)
{
    xnu_decmp_reprio_run run = {};
    run.fail_req_at = 2;
    xnu_decmp_reprio(GetParam(), 1, kInfo, 3, &run);
    EXPECT_EQ(run.blocking_allocs, 0u);
    ASSERT_EQ(run.enqueued, 2u);
    EXPECT_EQ(run.enqueued_blknos[0], 100u);
    EXPECT_EQ(run.enqueued_blknos[1], 300u);
}

TEST_P(DecmpReprio, NoReprioCopyMeansNoRaiseAndNoWait)
{
    xnu_decmp_reprio_run run = {};
    run.fail_kalloc = 1;
    xnu_decmp_reprio(GetParam(), 1, kInfo, 3, &run);
    EXPECT_EQ(run.blocking_allocs, 0u);
    EXPECT_EQ(run.enqueued, 0u);
}

TEST_P(DecmpReprio, ApplesOwnRaiseStillWaitsAndRaisesEveryBlock)
{
    xnu_decmp_reprio_run run = {};
    xnu_decmp_reprio(GetParam(), 0, kInfo, 3, &run);
    EXPECT_EQ(run.blocking_allocs, 4u);
    EXPECT_EQ(run.nowait_allocs, 0u);
    EXPECT_EQ(run.enqueued, 3u);
}

INSTANTIATE_TEST_SUITE_P(BothTrees, DecmpReprio, testing::Values(10063, 12377));

}  // namespace
