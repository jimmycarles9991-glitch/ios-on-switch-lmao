// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_surfaces.h"
#include <gtest/gtest.h>
#include <map>

TEST(RawSurfaces, PreservesEveryDependencyBeyondTheOldExecSurfacesLimit)
{
    raw::SurfaceList uses;
    for (uint64_t handle = 1; handle <= 200; ++handle)
        uses.add(handle, false);
    uses.add(3, true);
    ASSERT_TRUE(uses.valid());
    ASSERT_EQ(uses.uses().size(), 200u);
    EXPECT_EQ(uses.uses()[2].access, uint32_t(TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite));
}

TEST(RawSurfaces, CapacityFailureSticksUntilANewJob)
{
    raw::SurfaceList uses;
    for (uint64_t handle = 1; handle <= TegraGM20BMaxJobUses; ++handle)
        uses.add(handle, false);
    uses.add(1, true);
    EXPECT_TRUE(uses.valid());
    uses.add(TegraGM20BMaxJobUses + 1, false);
    EXPECT_FALSE(uses.valid());
    uses.add(1, true);
    EXPECT_FALSE(uses.valid());
    uses.clear();
    uses.add(7, false);
    EXPECT_TRUE(uses.valid());
    ASSERT_EQ(uses.uses().size(), 1u);
    EXPECT_EQ(uses.uses()[0].access, uint32_t(TegraGM20BSurfaceRead));
}

TEST(RawSurfaces, AddressAndHandleNamespacesStayDistinct)
{
    raw::SurfaceList uses;
    uses.add(4096, false);
    uses.addAccess(4096, TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceWrite);
    uses.addAccess(4096, TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRead);
    ASSERT_EQ(uses.uses().size(), 2u);
    EXPECT_EQ(uses.uses()[0].access, uint32_t(TegraGM20BSurfaceRead));
    EXPECT_EQ(uses.uses()[1].access,
              uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite));
    uses.clear();
    uses.add(0, true);
    EXPECT_FALSE(uses.valid());
}

TEST(RawSurfaces, BufferRangesFoldThousandsOfInteriorCopiesIntoOneBackingUse)
{
    raw::SurfaceList uses;
    uses.addAddress(0x100000, 0x100000, false);
    for (uint64_t at = 0; at < 10000; ++at)
        uses.addAddress(0x100000 + at * 32, 1, true);
    ASSERT_TRUE(uses.valid());
    ASSERT_EQ(uses.uses().size(), 1u);
    EXPECT_EQ(uses.uses()[0].access,
              uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceRead | TegraGM20BSurfaceWrite));
    uses.addAddress(0x200000, 32, false);
    EXPECT_EQ(uses.uses().size(), 2u);
    uses.addAddress(UINT64_MAX - 16, 32, false);
    EXPECT_FALSE(uses.valid());
}

TEST(RawSurfaces, GeometryReadChecksTheWholeBackingAndAddressNamespace)
{
    raw::SurfaceList uses;
    uses.addAddress(0x100040, 64, false);
    EXPECT_FALSE(uses.writesBacking(7, 0x100000, 0x1000));
    uses.addAddress(0x100800, 64, true);  // a different pooled resource, same backing
    EXPECT_TRUE(uses.writesBacking(7, 0x100000, 0x1000));
    EXPECT_FALSE(uses.writesBacking(0x100800, 0x200000, 0x1000));
    uses.add(9, true);
    EXPECT_TRUE(uses.writesBacking(9, 0x300000, 0x1000));
    uses.clear();
    EXPECT_FALSE(uses.writesBacking(9, 0x300000, 0x1000));
}

TEST(RawSurfaces, InterleavedResourcesMatchAnIndependentAccessUnion)
{
    raw::SurfaceList uses;
    std::map<std::pair<uint64_t, bool>, uint32_t> expected;
    for (unsigned round = 0; round < 40; ++round)
        for (unsigned i = 0; i < 1024; ++i) {
            const unsigned at = (i * 509 + round * 37) & 1023;
            const uint64_t handle = 0x100000000ull + uint64_t(at / 2) * 0x20000;
            const bool address = at & 1;
            const uint32_t access = (address ? TegraGM20BSurfaceGpuAddress : 0) |
                                    ((round + at) % 3 ? TegraGM20BSurfaceRead : TegraGM20BSurfaceWrite);
            uses.addAccess(handle, access);
            expected[{handle, address}] |= access;
        }
    ASSERT_TRUE(uses.valid());
    ASSERT_EQ(uses.uses().size(), expected.size());
    for (const auto &use : uses.uses())
        EXPECT_EQ(use.access, expected.at({use.handle, bool(use.access & TegraGM20BSurfaceGpuAddress)}));
    uses.clear();
    uses.addAddress(0x100000000, 32, false);
    ASSERT_EQ(uses.uses().size(), 1u);
    EXPECT_EQ(uses.uses()[0].access, uint32_t(TegraGM20BSurfaceRead | TegraGM20BSurfaceGpuAddress));
}

TEST(RawSurfaces, AccessMetadataMergesAndGrowingRangesCoverLaterInteriorUses)
{
    constexpr uint32_t hostWritten = 1u << 3;
    raw::SurfaceList uses;
    uses.addAccess(7, TegraGM20BSurfaceRead);
    uses.addAccess(7, TegraGM20BSurfaceRead | hostWritten);
    ASSERT_EQ(uses.uses().size(), 1u);
    EXPECT_EQ(uses.uses()[0].access, uint32_t(TegraGM20BSurfaceRead | hostWritten));
    uses.clear();
    uses.addAddress(0x100000, 1, false);
    uses.addAddress(0x100000, 4096, false);
    uses.addAddress(0x100000, 8192, false);
    uses.addAddress(0x101f00, 256, true);
    ASSERT_EQ(uses.uses().size(), 1u);
    EXPECT_TRUE(uses.writesBacking(1, 0x100000, 8192));
}

TEST(RawSurfaces, ExactRangesPreserveDisjointWritesAndMergeCoveredAliases)
{
    raw::SurfaceList uses;
    uses.addAddress(0x100000, 4096, true, TegraGM20BSurfaceRange);
    uses.addAddress(0x102000, 8192, true, TegraGM20BSurfaceRange);
    uses.addAddress(0x102080, 128, true, TegraGM20BSurfaceRange);
    ASSERT_TRUE(uses.valid());
    ASSERT_EQ(uses.uses().size(), 2u);
    EXPECT_EQ(uses.uses()[0].reserved, 4096u);
    EXPECT_EQ(uses.uses()[1].reserved, 8192u);
    for (const auto &use : uses.uses())
        EXPECT_EQ(use.access, uint32_t(TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceWrite | TegraGM20BSurfaceRange));
}

TEST(RawSurfaces, UnknownReadsDoNotInvalidateKnownWriteExtents)
{
    for (bool first : {false, true}) {
        raw::SurfaceList uses;
        if (first)
            uses.addAddress(0x100000, 1, false);
        uses.addAddress(0x100000, 64, true, TegraGM20BSurfaceRange | TegraGM20BSurfaceHostWritten);
        if (!first)
            uses.addAddress(0x100000, 1, false);
        uses.addAddress(0x100000, 4096, true, TegraGM20BSurfaceRange);
        uses.addAddress(0x100080, 64, false);
        ASSERT_EQ(uses.uses().size(), 1u);
        EXPECT_EQ(uses.uses()[0].reserved, 4096u);
        EXPECT_TRUE(uses.uses()[0].access & TegraGM20BSurfaceRange);
        EXPECT_TRUE(uses.uses()[0].access & TegraGM20BSurfaceHostWritten);
    }
}

TEST(RawSurfaces, UnknownWritesRemainUnknownAfterExactAliasesAndReads)
{
    for (bool first : {false, true}) {
        raw::SurfaceList uses;
        if (first)
            uses.addAddress(0x100000, 1, true);
        uses.addAddress(0x100000, 4096, true, TegraGM20BSurfaceRange);
        if (!first)
            uses.addAddress(0x100080, 1, true);
        uses.addAddress(0x100000, 4096, false, TegraGM20BSurfaceRange);
        ASSERT_TRUE(uses.valid());
        ASSERT_EQ(uses.uses().size(), 1u);
        EXPECT_EQ(uses.uses()[0].reserved, 0u);
        EXPECT_FALSE(uses.uses()[0].access & TegraGM20BSurfaceRange);
    }
    raw::SurfaceList uses;
    uses.addAddress(0x100000, 4096, true, TegraGM20BSurfaceRange);
    uses.addAccess(0x100000, TegraGM20BSurfaceGpuAddress | TegraGM20BSurfaceWrite);
    EXPECT_EQ(uses.uses()[0].reserved, 0u);
    EXPECT_FALSE(uses.uses()[0].access & TegraGM20BSurfaceRange);
}

TEST(RawSurfaces, ExtentOverflowCannotBeSerializedAsAPartialWrite)
{
    raw::SurfaceList uses;
    uses.addAddress(0x100000000, uint64_t(UINT32_MAX) + 1, true, TegraGM20BSurfaceRange);
    EXPECT_FALSE(uses.valid());
    uses.clear();
    uses.addAddress(0x100000000, uint64_t(UINT32_MAX) + 1, false);
    uses.addAddress(0x100000000, 64, true, TegraGM20BSurfaceRange);
    EXPECT_FALSE(uses.valid());
}
