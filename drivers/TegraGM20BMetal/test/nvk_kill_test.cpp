// SPDX-License-Identifier: MIT
// nvkmd_darwin's view of channels the kernel killed (third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin_kill.h) on
// the host: a fence word's kill record names the seqnos that never ran to their end (seqno wrap included) and nothing of a
// live channel or of the channel made after it in the same slot; a record kept past its channel answers for it alone; a
// timeline keeps its failed values in ranges, the oldest going first.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

#include "../../../third_party/mesa/src/nouveau/vulkan/nvkmd/darwin/nvkmd_darwin_kill.h"

namespace {

TEST(NvkKill, ALiveChannelHasNoRecord)
{
    const TegraGM20BAbiFence fence = {7, 0, 0, 0};
    darwin_kill kill = {};
    EXPECT_FALSE(darwin_kill_read(&fence, 1, &kill));
}

TEST(NvkKill, TheRecordNamesWhatNeverRan)
{
    // the kext: 10 completed, 13 submitted, then the kill moved the fence to 13
    const TegraGM20BAbiFence fence = {13, 11, 13, 1};
    darwin_kill kill = {};
    ASSERT_TRUE(darwin_kill_read(&fence, 2, &kill));
    EXPECT_EQ(kill.error, 1u);
    EXPECT_FALSE(darwin_kill_covers(&kill, 2, 10));
    for (uint32_t s = 11; s <= 13; s++)
        EXPECT_TRUE(darwin_kill_covers(&kill, 2, s));
    EXPECT_FALSE(darwin_kill_covers(&kill, 2, 14)) << "the next channel in the slot continues past it";
    EXPECT_FALSE(darwin_kill_covers(&kill, 3, 12)) << "another channel's seqno";
}

TEST(NvkKill, NothingInFlightNamesNoSeqno)
{
    const TegraGM20BAbiFence fence = {9, 10, 9, 3};
    darwin_kill kill = {};
    ASSERT_TRUE(darwin_kill_read(&fence, 1, &kill));
    for (uint32_t s = 0; s < 20; s++)
        EXPECT_FALSE(darwin_kill_covers(&kill, 1, s));
}

TEST(NvkKill, SeqnosWrap)
{
    const darwin_kill kill = {1, 0xfffffffeu, 1, 2};
    EXPECT_TRUE(darwin_kill_covers(&kill, 1, 0xfffffffeu));
    EXPECT_TRUE(darwin_kill_covers(&kill, 1, 0xffffffffu));
    EXPECT_TRUE(darwin_kill_covers(&kill, 1, 0));
    EXPECT_TRUE(darwin_kill_covers(&kill, 1, 1));
    EXPECT_FALSE(darwin_kill_covers(&kill, 1, 2));
    EXPECT_FALSE(darwin_kill_covers(&kill, 1, 0xfffffffdu));
}

TEST(NvkKill, KeptRecordsAnswerForTheirChannelAlone)
{
    darwin_kills kills = {};
    const darwin_kill first = {1, 5, 6, 1};
    const darwin_kill second = {1, 20, 20, 4};
    darwin_kills_add(&kills, &first);
    darwin_kills_add(&kills, &second);
    EXPECT_EQ(darwin_kills_error(&kills, 1, 5), 1u);
    EXPECT_EQ(darwin_kills_error(&kills, 1, 6), 1u);
    EXPECT_EQ(darwin_kills_error(&kills, 1, 7), 0u) << "the channel that replaced it ran 7";
    EXPECT_EQ(darwin_kills_error(&kills, 1, 20), 4u);
    EXPECT_EQ(darwin_kills_error(&kills, 2, 5), 0u);
    for (uint32_t i = 0; i < DARWIN_KILLS; i++) {
        const darwin_kill later = {2, 100 + i, 100 + i, 3};
        darwin_kills_add(&kills, &later);
    }
    EXPECT_EQ(darwin_kills_error(&kills, 1, 5), 0u) << "the oldest went first";
    EXPECT_EQ(darwin_kills_error(&kills, 2, 100 + DARWIN_KILLS - 1), 3u);
}

TEST(NvkKill, ATimelineKeepsItsFailedValues)
{
    darwin_failures *failures = nullptr;
    EXPECT_EQ(darwin_failures_find(failures, 3), 0u);
    darwin_failures_add(&failures, 3, 1);
    darwin_failures_add(&failures, 4, 1);
    darwin_failures_add(&failures, 5, 1);
    darwin_failures_add(&failures, 9, 4);
    ASSERT_NE(failures, nullptr);
    EXPECT_EQ(failures->count, 2u) << "one kill's values are one range";
    EXPECT_EQ(darwin_failures_find(failures, 2), 0u);
    for (uint64_t v = 3; v <= 5; v++)
        EXPECT_EQ(darwin_failures_find(failures, v), 1u);
    EXPECT_EQ(darwin_failures_find(failures, 6), 0u);
    EXPECT_EQ(darwin_failures_find(failures, 9), 4u);
    darwin_failures_add(&failures, 9, 4);
    EXPECT_EQ(failures->count, 2u) << "a value folded twice";
    for (uint64_t v = 0; v < DARWIN_FAILURES; v++)
        darwin_failures_add(&failures, 100 + 2 * v, 2);
    EXPECT_EQ(failures->count, uint32_t(DARWIN_FAILURES));
    EXPECT_EQ(darwin_failures_find(failures, 3), 0u) << "the oldest went first";
    EXPECT_EQ(darwin_failures_find(failures, 100 + 2 * (DARWIN_FAILURES - 1)), 2u);
    free(failures);
}

}  // namespace
