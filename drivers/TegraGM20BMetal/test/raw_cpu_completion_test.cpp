// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include "../raw/runtime/raw_cpu_completion.h"

namespace {
raw::CpuCompletion completion()
{
    raw::CpuCompletion result;
    result.channelUid = (uint64_t(7) << 48) | 2;
    result.channel = 9;
    result.seqno = 0;
    return result;
}

TEST(CpuCompletion, GpuOnlyWritesMakeNoCall)
{
    unsigned calls = 0;
    auto submit = [&](const auto &, auto &) {
        ++calls;
        return false;
    };
    auto batch = completion();
    EXPECT_EQ(raw::completeCpuRanges(batch, submit), raw::CpuCompletionResult::Current);
    EXPECT_EQ(calls, 0u);
}

TEST(CpuCompletion, AliasedAndAdjacentRangesMergeWithoutWideningGaps)
{
    auto batch = completion();
    batch.ranges = {{2, 256, 64}, {1, 64, 64}, {1, 0, 128}, {2, 0, 64}, {1, 128, 64}, {1, 0, 64}};
    batch.coalesce();
    ASSERT_TRUE(batch.valid);
    ASSERT_EQ(batch.ranges.size(), 3u);
    EXPECT_EQ(batch.ranges[0].handle, 1u);
    EXPECT_EQ(batch.ranges[0].offset, 0u);
    EXPECT_EQ(batch.ranges[0].bytes, 192u);
    EXPECT_EQ(batch.ranges[1].offset, 0u);
    EXPECT_EQ(batch.ranges[2].offset, 256u);
}

TEST(CpuCompletion, LogicalJobsKeepExactIdentityAcrossBoundedCallsIncludingWrappedZero)
{
    auto batch = completion();
    for (uint64_t handle = 1; handle <= 2 * TegraGM20BMaxJobUses + 1; ++handle)
        batch.ranges.push_back({handle, handle * 64, 64});
    size_t observed = 0;
    unsigned calls = 0;
    EXPECT_EQ(raw::completeCpuRanges(batch,
                                     [&](const auto &request, auto &reply) {
                                         EXPECT_EQ(request.channel_uid, batch.channelUid);
                                         EXPECT_EQ(request.channel, 9u);
                                         EXPECT_EQ(request.seqno, 0u);
                                         EXPECT_EQ(request.reserved, 0u);
                                         EXPECT_LE(request.range_count, uint32_t(TegraGM20BMaxJobUses));
                                         const auto *ranges = reinterpret_cast<const TegraGM20BAbiCpuRange *>(
                                             uintptr_t(request.ranges_address));
                                         for (uint32_t i = 0; i < request.range_count; ++i) {
                                             EXPECT_EQ(ranges[i].handle, observed + i + 1);
                                             EXPECT_EQ(ranges[i].offset, (observed + i + 1) * 64);
                                         }
                                         observed += request.range_count;
                                         ++calls;
                                         reply.current_count = request.range_count;
                                         return true;
                                     }),
              raw::CpuCompletionResult::Current);
    EXPECT_EQ(observed, batch.ranges.size());
    EXPECT_EQ(calls, 3u);
}

TEST(CpuCompletion, NewerWritersNeverBecomeCpuCurrent)
{
    auto batch = completion();
    batch.ranges = {{1, 0, 64}, {2, 0, 128}};
    EXPECT_EQ(raw::completeCpuRanges(batch,
                                     [](const auto &, auto &reply) {
                                         reply.current_count = 1;
                                         reply.stale = 1;
                                         return true;
                                     }),
              raw::CpuCompletionResult::Stale);
}

TEST(CpuCompletion, FailedPendingAndMalformedRepliesCannotPublishSuccess)
{
    auto batch = completion();
    batch.ranges = {{1, 0, 64}};
    const TegraGM20BAbiCpuCompleteReply replies[] = {
        {0, 0, 1, 0, -16, 0}, {0, 0, 0, 1, -5, 0},         {}, {1, 1, 0, 0, 0, 0}, {1, 0, 0, 0, 0, 1},
        {1, 0, 0, 0, -12, 0}, {UINT32_MAX, 2, 0, 0, 0, 0},
    };
    for (const auto expected : replies)
        EXPECT_EQ(raw::completeCpuRanges(batch,
                                         [&](const auto &, auto &reply) {
                                             reply = expected;
                                             return true;
                                         }),
                  raw::CpuCompletionResult::Failed);
    EXPECT_EQ(raw::completeCpuRanges(batch,
                                     [](const auto &, auto &) {
                                         return false;
                                     }),
              raw::CpuCompletionResult::Failed);
}

TEST(CpuCompletion, InvalidTrailingRangesAreRefusedBeforeAnyMaintenance)
{
    for (const TegraGM20BAbiCpuRange invalid :
         {TegraGM20BAbiCpuRange{0, 0, 64}, TegraGM20BAbiCpuRange{1, UINT64_MAX - 31, 64},
          TegraGM20BAbiCpuRange{1, 0, 0}}) {
        auto batch = completion();
        batch.ranges.assign(TegraGM20BMaxJobUses, {1, 0, 64});
        batch.ranges.push_back(invalid);
        unsigned calls = 0;
        EXPECT_EQ(raw::completeCpuRanges(batch,
                                         [&](const auto &, auto &) {
                                             ++calls;
                                             return true;
                                         }),
                  raw::CpuCompletionResult::Failed);
        EXPECT_EQ(calls, 0u);
        batch.coalesce();
        EXPECT_FALSE(batch.valid);
    }
    for (bool uid : {false, true}) {
        auto batch = completion();
        batch.ranges.push_back({1, 0, 64});
        if (uid)
            batch.channelUid = 0;
        else
            batch.channel = 0;
        EXPECT_EQ(raw::completeCpuRanges(batch,
                                         [](const auto &, auto &) {
                                             ADD_FAILURE();
                                             return true;
                                         }),
                  raw::CpuCompletionResult::Failed);
    }
}
}  // namespace
