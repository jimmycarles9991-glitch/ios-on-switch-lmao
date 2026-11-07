// drivers/TegraSMMU/TegraSMMUSpace.h on the host: first-fit placement in 64 KiB granules, reuse of freed runs, the
// refusals (no room, a window that is not one, a release of what was never handed out), and a full 1 GiB window.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../TegraSMMUSpace.h"

namespace {
constexpr uint64_t kBase = 0x40000000, kGranule = TegraSMMUSpace::kGranule;

struct Space {
    std::vector<uint32_t> bitmap;
    TegraSMMUSpace space;

    explicit Space(uint64_t bytes) : bitmap(TegraSMMUSpace::bitmapWords(bytes), 0xffffffffu)
    {
        EXPECT_TRUE(space.init(kBase, bytes, bitmap.data(), (uint32_t)bitmap.size()));
    }
};
}  // namespace

TEST(SmmuSpace, PlacesFirstFitInWholeGranules)
{
    Space s(16 * kGranule);
    EXPECT_EQ(s.space.bytesFree(), 16 * kGranule);
    EXPECT_EQ(s.space.alloc(1), kBase);
    EXPECT_EQ(s.space.alloc(kGranule + 1), kBase + kGranule);
    EXPECT_EQ(s.space.alloc(kGranule), kBase + 3 * kGranule);
    EXPECT_EQ(s.space.bytesFree(), 12 * kGranule);
}

TEST(SmmuSpace, ReusesTheLowestRunThatFits)
{
    Space s(8 * kGranule);
    const uint64_t a = s.space.alloc(2 * kGranule), b = s.space.alloc(kGranule), c = s.space.alloc(3 * kGranule);
    ASSERT_TRUE(a && b && c);
    ASSERT_TRUE(s.space.release(a, 2 * kGranule));
    EXPECT_EQ(s.space.alloc(3 * kGranule), 0u) << "two free at the start and two at the end are not three";
    EXPECT_EQ(s.space.alloc(kGranule), a);
    EXPECT_EQ(s.space.alloc(2 * kGranule), c + 3 * kGranule);
    EXPECT_EQ(s.space.alloc(kGranule), a + kGranule);
    EXPECT_EQ(s.space.bytesFree(), 0u);
    EXPECT_EQ(s.space.alloc(1), 0u);
}

TEST(SmmuSpace, RefusesWhatItNeverHandedOut)
{
    Space s(8 * kGranule);
    const uint64_t a = s.space.alloc(2 * kGranule);
    EXPECT_FALSE(s.space.release(a + kGranule, 2 * kGranule)) << "runs past what was taken";
    EXPECT_FALSE(s.space.release(a + 1, kGranule));
    EXPECT_FALSE(s.space.release(kBase - kGranule, kGranule));
    EXPECT_FALSE(s.space.release(a, 9 * kGranule));
    EXPECT_FALSE(s.space.release(a, 0));
    EXPECT_TRUE(s.space.release(a, 2 * kGranule));
    EXPECT_FALSE(s.space.release(a, 2 * kGranule)) << "twice";
    EXPECT_EQ(s.space.bytesFree(), 8 * kGranule);
}

TEST(SmmuSpace, RefusesWindowsThatAreNotOne)
{
    std::vector<uint32_t> bitmap(64);
    TegraSMMUSpace space;
    EXPECT_FALSE(space.init(kBase + 1, kGranule, bitmap.data(), 64));
    EXPECT_FALSE(space.init(kBase, kGranule + 1, bitmap.data(), 64));
    EXPECT_FALSE(space.init(kBase, 0, bitmap.data(), 64));
    EXPECT_FALSE(space.init(kBase, 64 * 32 * kGranule + kGranule, bitmap.data(), 64)) << "bitmap too small";
    EXPECT_FALSE(space.init(0xfff00000, 32 * kGranule, bitmap.data(), 64)) << "past 4 GiB";
    EXPECT_FALSE(space.init(kBase, kGranule, nullptr, 64));
    EXPECT_EQ(space.alloc(kGranule), 0u);
}

TEST(SmmuSpace, FillsAGibibyteWindowOfFourKSurfaces)
{
    Space s(1ull << 30);
    const uint64_t surface = 3840ull * 2176 * 3 / 2;  // a 4K HEVC picture, 12,533,760 bytes
    const uint64_t rounded = TegraSMMUSpace::round(surface);
    std::vector<uint64_t> taken;
    for (uint64_t iova; (iova = s.space.alloc(surface));) {
        if (!taken.empty())
            EXPECT_EQ(iova, taken.back() + rounded);
        taken.push_back(iova);
    }
    EXPECT_EQ(taken.size(), (size_t)((1ull << 30) / rounded));
    EXPECT_LT(s.space.bytesFree(), rounded);
    for (uint64_t iova : taken)
        ASSERT_TRUE(s.space.release(iova, surface));
    EXPECT_EQ(s.space.bytesFree(), 1ull << 30);
}
