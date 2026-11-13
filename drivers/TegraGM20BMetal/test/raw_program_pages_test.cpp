// SPDX-License-Identifier: MIT
// raw/runtime/raw_program_pages.h: the grains of a file a program run in place needs bound in the program window. A slot
// keeps its alignment there, each grain is the same fixed piece of the file whichever slot asks for it and is asked for
// once, and nothing past the file's mapping is ever asked for.
#include "../raw/runtime/raw_program_pages.h"

#include <gtest/gtest.h>

namespace {

constexpr uint64_t kPage = 16384, kGrain = raw::kProgramGrain, kSlack = raw::kProgramPrefetchBytes;
// pack27's heap section starts 256-aligned, not page-aligned; the file ends mid-grain
constexpr uint64_t kHeapAt = 0x2393500, kMapped = (0x12434500 + 19256 + kPage - 1) / kPage * kPage;

raw::ProgramPages packLike()
{
    raw::ProgramPages p;
    p.reset(kHeapAt, kMapped, kMapped, 0);
    return p;
}

TEST(RawProgramPages, ASlotKeepsItsAlignmentInTheWindow)
{
    const raw::ProgramPages p = packLike();
    for (uint64_t slot : {uint64_t(0), uint64_t(0x100), uint64_t(0x1a3700), uint64_t(26836 * 1024)}) {
        const uint64_t window = p.windowOffset(kHeapAt + slot);
        EXPECT_EQ(window % 0x100, 0u) << "slot " << slot;
        EXPECT_EQ(window % kGrain, (kHeapAt + slot) % kGrain) << "a grain is bound at its own place in the file";
        EXPECT_EQ(p.fileOffset(window), kHeapAt + slot);
    }
}

TEST(RawProgramPages, ASlotNeedsTheFixedGrainsItAndItsPrefetchLieIn)
{
    const raw::ProgramPages p = packLike();
    std::vector<raw::ProgramPages::Grain> grains;
    const uint64_t slot = kHeapAt + 0x10000, bytes = 340 * 1024;
    ASSERT_TRUE(p.missing(slot, bytes + kSlack, grains));
    ASSERT_FALSE(grains.empty());
    for (size_t i = 0; i < grains.size(); i++) {
        EXPECT_EQ(grains[i].offset % kGrain, 0u);
        EXPECT_EQ(grains[i].bytes, kGrain);
        if (i)
            EXPECT_EQ(grains[i].offset, grains[i - 1].offset + kGrain);
    }
    EXPECT_LE(grains.front().offset, slot);
    EXPECT_GE(grains.back().offset + kGrain, slot + bytes + kSlack) << "the prefetch past the end is bound too";
    EXPECT_LT(grains.back().offset, slot + bytes + kSlack);
}

TEST(RawProgramPages, BoundGrainsAreNotAskedForAgain)
{
    raw::ProgramPages p = packLike();
    std::vector<raw::ProgramPages::Grain> grains;
    const uint64_t first = kHeapAt + 3 * kGrain;
    ASSERT_TRUE(p.missing(first, kGrain, grains));
    ASSERT_EQ(grains.size(), 2u) << "a grain's worth from mid-grain spans two";
    for (const auto &g : grains)
        p.bind(g);
    EXPECT_TRUE(p.bound(first, kGrain));
    EXPECT_TRUE(p.bound(first + 100, 1000)) << "a small slot inside bound grains runs in place with no import";
    ASSERT_TRUE(p.missing(first - kGrain, 4 * kGrain, grains));
    ASSERT_EQ(grains.size(), 3u) << "the grains on either side, not the bound ones between";
    EXPECT_EQ(grains[1].offset - grains[0].offset, 3 * kGrain) << "the gap is the two bound grains";
    const uint64_t before = p.boundBytes();
    for (const auto &g : grains)
        p.bind(g);
    p.bind(grains[0]);
    EXPECT_EQ(p.boundBytes(), before + 3 * kGrain) << "a grain counts once";
}

TEST(RawProgramPages, TheLastGrainEndsWithTheMapping)
{
    const raw::ProgramPages p = packLike();
    std::vector<raw::ProgramPages::Grain> grains;
    ASSERT_NE(kMapped % kGrain, 0u);
    ASSERT_TRUE(p.missing(kMapped - 0x100, 0x100, grains));
    ASSERT_EQ(grains.size(), 1u);
    EXPECT_EQ(grains[0].offset + grains[0].bytes, kMapped) << "never past the mapping's last page";
    EXPECT_EQ(grains[0].bytes % kPage, 0u);
    EXPECT_FALSE(p.missing(kMapped - 0x100, 0x200, grains)) << "past the mapping: copied instead";
    EXPECT_FALSE(p.missing(kHeapAt - kGrain - 1, 16, grains)) << "before the region's first grain";
    EXPECT_FALSE(p.missing(kHeapAt, 0, grains));
    EXPECT_FALSE(raw::ProgramPages().missing(0, 16, grains)) << "no region";
}

TEST(RawProgramPages, TwoProcessesImportTheSameGrains)
{
    const raw::ProgramPages a = packLike(), b = packLike();
    std::vector<raw::ProgramPages::Grain> wide, narrow;
    ASSERT_TRUE(a.missing(kHeapAt + 5 * kGrain + 0x300, 3 * kGrain, wide));
    ASSERT_TRUE(b.missing(kHeapAt + 6 * kGrain + 0x900, 0x200, narrow));
    ASSERT_EQ(narrow.size(), 1u);
    bool same = false;
    for (const auto &g : wide)
        same |= g.offset == narrow[0].offset && g.bytes == narrow[0].bytes;
    EXPECT_TRUE(same) << "another slot in the same grain imports exactly that grain's pages";
}

TEST(RawProgramPages, TheWindowSpansTheRegionFromItsBase)
{
    raw::ProgramPages p;
    p.reset(kHeapAt, kMapped, kMapped, 0x40000000);
    EXPECT_TRUE(p.holdsWindow(p.windowOffset(kHeapAt)));
    EXPECT_FALSE(p.holdsWindow(0x40000000 - 1));
    EXPECT_FALSE(p.holdsWindow(p.windowEnd()));
    EXPECT_EQ(p.windowEnd() % kGrain, 0u);
    EXPECT_EQ(p.windowOffset(kHeapAt - kHeapAt % kGrain), 0x40000000u);
}

}  // namespace
