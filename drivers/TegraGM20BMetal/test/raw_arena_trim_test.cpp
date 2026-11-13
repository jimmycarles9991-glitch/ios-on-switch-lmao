// SPDX-License-Identifier: MIT
#include "raw_kernel_model.h"
#include "empty_pack.h"
#include "../raw/runtime/raw_arena_trim.h"
#include "../raw/runtime/raw_gpu.h"

#include <gtest/gtest.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr uint64_t kGranule = 0x10000, kQuarter = 0x40000;

TEST(ArenaHoles, FreeRangesShrinkToWholeGranules)
{
    raw::HeapPlacement p(4 * kQuarter);
    uint64_t a = 0, b = 0, c = 0;
    ASSERT_TRUE(p.place(kQuarter, 256, a));
    ASSERT_TRUE(p.place(kQuarter + 0x100, 256, b));
    ASSERT_TRUE(p.place(0x100, 256, c));
    p.release(a, kQuarter);
    // [0, 1/4) free; b takes [1/4, 1/2 + 0x100); c [1/2 + 0x100, 1/2 + 0x200); the rest free to the end
    const std::vector<raw::ArenaRange> holes = raw::arenaHoles(p, 4 * kQuarter, kGranule);
    ASSERT_EQ(holes.size(), 2u);
    EXPECT_EQ(holes[0], (raw::ArenaRange{0, kQuarter}));
    EXPECT_EQ(holes[1], (raw::ArenaRange{2 * kQuarter + kGranule, 2 * kQuarter - kGranule}));
    const std::vector<raw::ArenaRange> backed = raw::arenaBacked(holes, 4 * kQuarter);
    ASSERT_EQ(backed.size(), 1u);
    EXPECT_EQ(backed[0], (raw::ArenaRange{kQuarter, kQuarter + kGranule}));
}

TEST(ArenaHoles, TheTailPastTheHeapsSizeIsAHole)
{
    raw::HeapPlacement p(kQuarter + 0x1000);
    uint64_t at = 0;
    ASSERT_TRUE(p.place(kQuarter, 256, at));
    const std::vector<raw::ArenaRange> holes = raw::arenaHoles(p, kQuarter + kGranule, kGranule);
    ASSERT_EQ(holes.size(), 1u);
    EXPECT_EQ(holes[0], (raw::ArenaRange{kQuarter, kGranule}));
}

TEST(ArenaHoles, APlacementHeapHasNone)
{
    raw::HeapPlacement p(4 * kQuarter, true);
    EXPECT_TRUE(raw::arenaHoles(p, 4 * kQuarter, kGranule).empty());
}

TEST(ArenaHoles, APlacementTakesTheGranulesItTouches)
{
    std::vector<raw::ArenaRange> holes = {{0, kQuarter}, {2 * kQuarter, kQuarter}};
    const std::vector<raw::ArenaRange> taken = raw::takeHoles(holes, 0x100, kGranule, kGranule);
    ASSERT_EQ(taken.size(), 1u);
    EXPECT_EQ(taken[0], (raw::ArenaRange{0, 2 * kGranule}));
    ASSERT_EQ(holes.size(), 2u);
    EXPECT_EQ(holes[0], (raw::ArenaRange{2 * kGranule, kQuarter - 2 * kGranule}));
    EXPECT_EQ(holes[1], (raw::ArenaRange{2 * kQuarter, kQuarter}));
}

// The pages of [p, p + bytes) the process has resident
size_t residentPages(const void *p, size_t bytes)
{
    const size_t page = size_t(getpagesize());
    std::vector<char> in(bytes / page);
    mincore(p, bytes, in.data());
    size_t n = 0;
    for (char c : in)
        n += (c & MINCORE_INCORE) != 0;
    return n;
}

class ArenaTrim : public testing::Test {
protected:
    raw::Device dev;

    void SetUp() override
    {
        kernel_model::model = {};
        kernel_model::model.vaEnd = 1ull << 40;
        std::string err;
        ASSERT_TRUE(dev.open(emptyPack().c_str(), 0, err, raw::Device::Role::Client)) << err;
    }

    // The model's live imports at [from, from + bytes): each one's offset from `from` and its bytes
    static std::vector<raw::ArenaRange> importsIn(const uint8_t *from, uint64_t bytes)
    {
        std::vector<raw::ArenaRange> out;
        for (const auto &[handle, import] : kernel_model::model.imports) {
            (void)handle;
            if (import.first >= uintptr_t(from) && import.first < uintptr_t(from) + bytes)
                out.push_back({import.first - uintptr_t(from), import.second});
        }
        std::sort(out.begin(), out.end(), [](const raw::ArenaRange &x, const raw::ArenaRange &y) {
            return x.offset < y.offset;
        });
        return out;
    }
};

// A heap kept non-volatile gives back the ranges no resource is placed in: its other parts are imported again with their
// contents, the holes' pages are no longer the process's (nothing wrote them), and a placement into a hole binds it again
TEST_F(ArenaTrim, TheUnplacedRangesGoBackAndComeBackWithAPlacement)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, false, arena));
    raw::Buffer a, b, c;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, b));
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, c));
    uint8_t *base = a.cpu;
    memset(a.cpu, 0xA5, kQuarter);
    memset(b.cpu, 0x3C, kQuarter);
    memset(c.cpu, 0x5A, kQuarter);
    ASSERT_EQ(importsIn(base, 4 * kQuarter), (std::vector<raw::ArenaRange>{{0, 4 * kQuarter}}));
    dev.arenaRelease(b);

    std::atomic<uint32_t> committed{1};
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(base, 4 * kQuarter), (std::vector<raw::ArenaRange>{{0, 4 * kQuarter}}))
        << "a command buffer committed keeps the arena whole";
    committed = 0;
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(base, 4 * kQuarter), (std::vector<raw::ArenaRange>{{0, kQuarter}, {2 * kQuarter, kQuarter}}));
    EXPECT_EQ(residentPages(base + kQuarter, kQuarter), 0u);
    EXPECT_EQ(residentPages(base + 3 * kQuarter, kQuarter), 0u);
    std::vector<uint8_t> want(kQuarter, 0xA5);
    EXPECT_EQ(memcmp(a.cpu, want.data(), kQuarter), 0);
    want.assign(kQuarter, 0x5A);
    EXPECT_EQ(memcmp(c.cpu, want.data(), kQuarter), 0);
    uint64_t placed = 0, volatileBytes = 0, given = 0;
    dev.arenaUse(placed, volatileBytes, &given);
    EXPECT_EQ(given, 2 * kQuarter);
    EXPECT_EQ(dev.arenaBytes(), 2 * kQuarter);

    raw::Buffer d;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, d));
    EXPECT_EQ(d.cpu, base + kQuarter);
    EXPECT_EQ(importsIn(base, 4 * kQuarter),
              (std::vector<raw::ArenaRange>{{0, kQuarter}, {kQuarter, kQuarter}, {2 * kQuarter, kQuarter}}));
    bool bound = false;
    for (const auto &[va, at] : kernel_model::model.binds)
        bound = bound || va == d.va;
    EXPECT_TRUE(bound);
    dev.arenaUse(placed, volatileBytes, &given);
    EXPECT_EQ(given, kQuarter);
    EXPECT_EQ(residentPages(base + kQuarter, kQuarter), 0u) << "binding a hole again writes none of its pages";

    // nothing new to give back: the arena stays as it is
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(base, 4 * kQuarter).size(), 3u);
}

// A volatile heap's whole arena goes at the purge (raw_arena_purge.h), never trimmed
TEST_F(ArenaTrim, AVolatileHeapIsPurgedNotTrimmed)
{
    uint32_t arena = 0;
    ASSERT_TRUE(dev.createArena(4 * kQuarter, false, arena));
    raw::Buffer a;
    ASSERT_TRUE(dev.arenaBuffer(arena, kQuarter, a));
    ASSERT_TRUE(!dev.setArenaVolatile(arena, true));
    std::atomic<uint32_t> committed{0};
    dev.trimArenas(committed);
    EXPECT_EQ(importsIn(a.cpu, 4 * kQuarter), (std::vector<raw::ArenaRange>{{0, 4 * kQuarter}}));
}

}  // namespace
