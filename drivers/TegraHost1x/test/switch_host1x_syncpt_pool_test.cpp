// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchHost1xSyncptPool.h: what is reserved is never handed out, a freed id comes back only after every other free one
// has been handed out since, and a free of an id not handed out changes nothing.
#include <gtest/gtest.h>

#include <set>
#include <vector>

#include "../SwitchHost1xSyncptPool.h"

namespace {
SwitchHost1xSyncptPool pool()
{
    return {{1ULL << 0 | 1ULL << 9, 0, 0}, {}, {}, {}, 0, 0, 2};
}
}  // namespace

TEST(Host1xSyncptPool, EveryIdButTheReservedOnesIsHandedOutOnceTheOldRangeFirst)
{
    SwitchHost1xSyncptPool p = pool();
    ASSERT_TRUE(p.reserve(40)) << "the boot chain left 40 counting";
    EXPECT_EQ(p.capacity(), 189u);
    std::set<uint32_t> ids;
    uint32_t id = 0;
    for (uint32_t i = 0; i < 189; ++i) {
        ASSERT_TRUE(p.alloc(&id));
        if (i < 32)
            EXPECT_EQ(id, 160 + i) << "the ids every boot used before come first";
        ids.insert(id);
    }
    EXPECT_EQ(ids.size(), 189u);
    EXPECT_EQ(ids.count(0) + ids.count(9) + ids.count(40), 0u);
    EXPECT_FALSE(p.alloc(&id)) << "all in use";
    EXPECT_FALSE(p.reserve(170)) << "too late: handed out";
}

TEST(Host1xSyncptPool, AFreedIdComesBackOnlyAfterEveryOtherFreeOne)
{
    SwitchHost1xSyncptPool p = pool();
    uint32_t first = 0, id = 0;
    ASSERT_TRUE(p.alloc(&first));
    EXPECT_TRUE(p.owns(first));
    ASSERT_TRUE(p.free(first));
    EXPECT_FALSE(p.owns(first));
    EXPECT_TRUE(p.everOwned(first)) << "SC7 still saves it";
    for (uint32_t i = 0; i < 189; ++i) {
        ASSERT_TRUE(p.alloc(&id));
        EXPECT_NE(id, first) << i;
    }
    ASSERT_TRUE(p.alloc(&id));
    EXPECT_EQ(id, first);
}

TEST(Host1xSyncptPool, AFreeOfAnIdNotHandedOutChangesNothing)
{
    SwitchHost1xSyncptPool p = pool();
    uint32_t id = 0;
    EXPECT_FALSE(p.free(9)) << "reserved";
    EXPECT_FALSE(p.free(170)) << "never handed out";
    EXPECT_FALSE(p.free(192));
    ASSERT_TRUE(p.alloc(&id));
    ASSERT_TRUE(p.free(id));
    EXPECT_FALSE(p.free(id)) << "freed already";
    EXPECT_EQ(p.inUse, 0u);
    EXPECT_EQ(p.capacity(), 190u);
}

// Review-dma2 D6: freed ids come back in the order they were freed, and only after every id never handed out, wherever
// they sit in the scan. The hard case: an id freed just past where the last allocation ended.
TEST(Host1xSyncptPool, FreedIdsComeBackInTheOrderFreedAfterEveryUnusedOne)
{
    SwitchHost1xSyncptPool p = pool();
    uint32_t id = 0;
    std::vector<uint32_t> first;
    for (uint32_t i = 0; i < 20; ++i) {
        ASSERT_TRUE(p.alloc(&id));
        first.push_back(id);
    }
    ASSERT_TRUE(p.free(first[19]));  // the last one handed out: next to where the scan goes on
    ASSERT_TRUE(p.free(first[3]));
    std::set<uint32_t> fresh;
    for (uint32_t i = 0; i < 190 - 20; ++i) {
        ASSERT_TRUE(p.alloc(&id));
        EXPECT_NE(id, first[19]) << i;
        EXPECT_NE(id, first[3]) << i;
        fresh.insert(id);
    }
    EXPECT_EQ(fresh.size(), 170u);
    ASSERT_TRUE(p.alloc(&id));
    EXPECT_EQ(id, first[19]) << "freed first";
    ASSERT_TRUE(p.alloc(&id));
    EXPECT_EQ(id, first[3]);
    EXPECT_FALSE(p.alloc(&id));
}
