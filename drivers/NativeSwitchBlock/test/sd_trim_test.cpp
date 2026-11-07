// drivers/NativeSwitchBlock/SwitchSdTrim.h: only whole AUs inside an unmap become due, a queued write cancels every AU
// it touches, and runs come from the cursor on, wrapping once.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../SwitchSdTrim.h"

namespace {
constexpr uint32_t kAu = 8192;  // 4 MiB, the console's card

struct Map {
    std::vector<uint64_t> storage;
    SwitchSdTrimMap m;

    Map(uint64_t first, uint64_t sectors) : storage(switchSdTrimWords(first, sectors, kAu))
    {
        switchSdTrimInit(&m, storage.data(), first, sectors, kAu);
    }
};

TEST(SwitchSdTrim, WholeAusOnly)
{
    Map map(1000, 200 * kAu);  // starts mid-AU: AU 1 is the first whole one
    EXPECT_EQ(map.m.firstAu, 1u);
    EXPECT_EQ(map.m.aus, 199u);
    switchSdTrimMark(&map.m, 1000, kAu);  // no whole AU
    EXPECT_EQ(map.m.pending, 0u);
    switchSdTrimMark(&map.m, kAu - 1, 3 * kAu + 2);  // AUs 1, 2, 3 whole
    EXPECT_EQ(map.m.pending, 3u);
    uint64_t lba = 0;
    uint32_t aus = 0;
    ASSERT_TRUE(switchSdTrimNext(&map.m, 15, &lba, &aus));
    EXPECT_EQ(lba, kAu);
    EXPECT_EQ(aus, 3u);
    EXPECT_FALSE(switchSdTrimNext(&map.m, 15, &lba, &aus));
}

TEST(SwitchSdTrim, WriteCancels)
{
    Map map(0, 100 * kAu);
    switchSdTrimMark(&map.m, 10 * kAu, 10 * kAu);
    switchSdTrimClear(&map.m, 12 * kAu + 5, 1);  // inside AU 12
    switchSdTrimClear(&map.m, 15 * kAu - 1, 2);  // AU 14 and 15
    EXPECT_EQ(map.m.pending, 7u);
    uint64_t lba = 0;
    uint32_t aus = 0;
    std::vector<std::pair<uint64_t, uint32_t>> runs;
    while (switchSdTrimNext(&map.m, 15, &lba, &aus))
        runs.push_back({lba / kAu, aus});
    const std::vector<std::pair<uint64_t, uint32_t>> want = {{10, 2}, {13, 1}, {16, 4}};
    EXPECT_EQ(runs, want);
}

TEST(SwitchSdTrim, CursorWrapsAndCaps)
{
    Map map(0, 300 * kAu);
    switchSdTrimMark(&map.m, 0, 300 * kAu);
    map.m.cursor = 290;
    uint64_t lba = 0;
    uint32_t aus = 0;
    ASSERT_TRUE(switchSdTrimNext(&map.m, 15, &lba, &aus));
    EXPECT_EQ(lba / kAu, 290u);
    EXPECT_EQ(aus, 10u);  // to the end of the partition
    EXPECT_EQ(map.m.cursor, 0u);
    ASSERT_TRUE(switchSdTrimNext(&map.m, 15, &lba, &aus));
    EXPECT_EQ(lba / kAu, 0u);
    EXPECT_EQ(aus, 15u);
    EXPECT_EQ(map.m.pending, 275u);
}

TEST(SwitchSdTrim, SparseScan)
{
    Map map(0, 4096 * kAu);
    switchSdTrimMark(&map.m, 4000 * kAu, kAu);
    uint64_t lba = 0;
    uint32_t aus = 0;
    ASSERT_TRUE(switchSdTrimNext(&map.m, 15, &lba, &aus));
    EXPECT_EQ(lba / kAu, 4000u);
    EXPECT_EQ(aus, 1u);
    EXPECT_EQ(map.m.pending, 0u);
}
}  // namespace
