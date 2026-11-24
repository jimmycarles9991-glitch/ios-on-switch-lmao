// SPDX-License-Identifier: MIT
#include "../split_batch.h"
#include <gtest/gtest.h>
#include <cstring>
#include <set>

TEST(GM20BSplitBatch, InterleavedProgramsCoverEveryRecordExactlyOnce)
{
    gm20b::SplitBatchPlanner planner;
    planner.clear(10000);
    for (uint32_t i = 0; i < 10000; ++i)
        planner.append({i % 3, 0x1000, 256, 64}, i);
    std::set<uint32_t> covered;
    for (const auto &run : planner.runs()) {
        ASSERT_GT(run.count, 0u);
        ASSERT_LE(run.count, 1024u);
        uint32_t count = 0, previous = 0;
        for (uint32_t i = run.first; i != gm20b::SplitBatchPlanner::kEnd; i = planner.next(i)) {
            ASSERT_LT(i, 10000u);
            EXPECT_TRUE(covered.insert(i).second);
            EXPECT_EQ(i % 3, run.key.program);
            if (count)
                EXPECT_GT(i, previous);
            previous = i;
            ASSERT_LE(++count, 1024u);
        }
        EXPECT_EQ(count, run.count);
        EXPECT_EQ(previous, run.last);
    }
    EXPECT_EQ(covered.size(), 10000u);
    EXPECT_EQ(planner.runs().size(), 12u);
}

TEST(GM20BSplitBatch, ImmutableProgramDataAndThreadShapeDefineCompatibility)
{
    gm20b::SplitBatchPlanner planner;
    planner.clear(6);
    planner.append({1, 0x1000, 256, 64}, 0);
    planner.append({2, 0x1000, 256, 64}, 1);
    planner.append({1, 0x2000, 256, 64}, 2);
    planner.append({1, 0x1000, 128, 64}, 3);
    planner.append({1, 0x1000, 256, 128}, 4);
    planner.append({1, 0x1000, 256, 64}, 5);
    ASSERT_EQ(planner.runs().size(), 5u);
    EXPECT_EQ(planner.runs()[0].count, 2u);
    EXPECT_EQ(planner.next(0), 5u);
    EXPECT_EQ(planner.next(5), gm20b::SplitBatchPlanner::kEnd);
}

TEST(GM20BSplitBatch, ResetCannotCarryRecordsIntoAnotherSubmission)
{
    gm20b::SplitBatchPlanner planner;
    planner.clear(1025);
    for (uint32_t i = 0; i < 1025; ++i)
        planner.append({1, 0, 0, 64}, i);
    ASSERT_EQ(planner.runs().size(), 2u);
    EXPECT_EQ(planner.runs()[0].count, 1024u);
    EXPECT_EQ(planner.runs()[1].count, 1u);
    planner.clear(2);
    planner.append({1, 0, 0, 64}, 1);
    ASSERT_EQ(planner.runs().size(), 1u);
    EXPECT_EQ(planner.runs()[0].first, 1u);
    EXPECT_EQ(planner.runs()[0].count, 1u);
    EXPECT_EQ(planner.next(1), gm20b::SplitBatchPlanner::kEnd);
}

TEST(GM20BSplitBatch, WireLayoutPreservesEveryCapturedPushAndBuffer)
{
    gm20b::SplitBatchRecord record;
    for (uint32_t i = 0; i < 32; ++i)
        record.push[i] = 0xa1230000 + i;
    record.frontier = 0x123456789abc0000;
    for (uint32_t i = 0; i < 8; ++i)
        record.buffers[i] = {0x123400000000 + uint64_t(i) * 256, 128 + 16 * i, 0};
    uint8_t bytes[272];
    memcpy(bytes, &record, sizeof bytes);
    auto u32 = [&](size_t offset) {
        uint32_t value;
        memcpy(&value, bytes + offset, sizeof value);
        return value;
    };
    auto u64 = [&](size_t offset) {
        uint64_t value;
        memcpy(&value, bytes + offset, sizeof value);
        return value;
    };
    for (uint32_t i = 0; i < 32; ++i)
        EXPECT_EQ(u32(4 * i), 0xa1230000 + i);
    EXPECT_EQ(u64(128), 0x123456789abc0000u);
    EXPECT_EQ(u64(136), 0u);
    for (uint32_t i = 0; i < 8; ++i) {
        EXPECT_EQ(u64(144 + 16 * i), 0x123400000000 + uint64_t(i) * 256);
        EXPECT_EQ(u32(152 + 16 * i), 128 + 16 * i);
        EXPECT_EQ(u32(156 + 16 * i), 0u);
    }
}

TEST(GM20BSplitBatch, ManyKeysAndRepeatedResetsKeepEveryRecord)
{
    gm20b::SplitBatchPlanner planner;
    for (uint32_t round = 0; round < 4; ++round) {
        constexpr uint32_t kKeys = 64, kRecords = kKeys * 3;
        planner.clear(kRecords);
        for (uint32_t i = 0; i < kRecords; ++i)
            planner.append({(i + round) % kKeys, 0x1000ull * ((i + round) % kKeys), 256, 64}, i);
        ASSERT_EQ(planner.runs().size(), kKeys);
        std::set<uint32_t> seen;
        for (const auto &run : planner.runs()) {
            EXPECT_EQ(run.count, 3u);
            for (uint32_t i = run.first; i != gm20b::SplitBatchPlanner::kEnd; i = planner.next(i))
                EXPECT_TRUE(seen.insert(i).second);
        }
        EXPECT_EQ(seen.size(), kRecords);
    }
}
