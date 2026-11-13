// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_split_metadata.h"
#include <gtest/gtest.h>

namespace {
raw::PackSplit record(unsigned version, unsigned ubos, bool data)
{
    raw::PackSplit split = {};
    split.frontierBytes = 4096;
    split.frontierCbuf = 8;
    split.uboCount = ubos;
    for (unsigned i = 0; i < ubos && i < 8; ++i)
        split.uboCbuf[i] = i + 1;
    split.batchVersion = version;
    split.batchThreads = version ? 32 : 0;
    split.shaderDataCbuf = data ? (version ? 1 : 1 + ubos) : 0;
    split.prologueCbufs = version ? 1 + data : 1 + ubos + data;
    split.qmd[18] = (version ? 32u : 1u) << 16;
    split.qmd[19] = 0x10001;
    return split;
}

TEST(RawSplitMetadata, ScalarAndBatchKeepDistinctUniformAndShaderDataBindings)
{
    for (unsigned version : {0u, 1u})
        for (unsigned ubos = 0; ubos <= 6; ++ubos)
            for (bool data : {false, true})
                EXPECT_TRUE(raw::usableSplitMetadata(record(version, ubos, data)));
}

TEST(RawSplitMetadata, RejectsCountsBeforeTraversingUniformSlots)
{
    for (unsigned version : {0u, 1u})
        for (unsigned count = 7; count <= 255; ++count) {
            auto split = record(version, 6, true);
            split.uboCount = count;
            EXPECT_FALSE(raw::usableSplitMetadata(split));
        }
}

TEST(RawSplitMetadata, FragmentBindingsAreDistinctAndPrecedeFrontier)
{
    auto split = record(1, 2, false);
    split.uboCbuf[1] = split.uboCbuf[0];
    EXPECT_FALSE(raw::usableSplitMetadata(split));
    for (unsigned slot = split.frontierCbuf; slot <= 255; ++slot) {
        split.uboCbuf[1] = slot;
        EXPECT_FALSE(raw::usableSplitMetadata(split));
    }
    for (unsigned bytes : {0u, 1u, 4095u, 4112u, 65535u}) {
        split = record(1, 2, false);
        split.frontierBytes = bytes;
        EXPECT_FALSE(raw::usableSplitMetadata(split));
    }
    for (unsigned slot : {0u, 16u, 255u}) {
        split = record(1, 2, false);
        split.frontierCbuf = slot;
        EXPECT_FALSE(raw::usableSplitMetadata(split));
    }
}

TEST(RawSplitMetadata, ScalarConstantsCannotOverwriteUniforms)
{
    for (unsigned slot = 1; slot <= 6; ++slot) {
        auto split = record(0, 6, true);
        split.shaderDataCbuf = slot;
        EXPECT_FALSE(raw::usableSplitMetadata(split));
    }
    auto split = record(0, 6, true);
    --split.prologueCbufs;
    EXPECT_FALSE(raw::usableSplitMetadata(split));
    split = record(0, 0, false);
    split.batchThreads = 1;
    EXPECT_FALSE(raw::usableSplitMetadata(split));
}

TEST(RawSplitMetadata, BatchConstantsUseOnlySharedSecondBuffer)
{
    for (unsigned slot = 2; slot <= 255; ++slot) {
        auto split = record(1, 6, true);
        split.shaderDataCbuf = slot;
        EXPECT_FALSE(raw::usableSplitMetadata(split));
    }
    for (unsigned count : {0u, 2u, 3u, 8u, 255u}) {
        auto split = record(1, 6, false);
        split.prologueCbufs = count;
        EXPECT_FALSE(raw::usableSplitMetadata(split));
    }
}

TEST(RawSplitMetadata, RejectsUnknownOrMismatchedLaunchAbi)
{
    for (unsigned version : {2u, 255u, 65535u}) {
        auto split = record(1, 6, true);
        split.batchVersion = version;
        EXPECT_FALSE(raw::usableSplitMetadata(split));
    }
    for (unsigned threads : {0u, 31u, 33u, 1025u, 65535u}) {
        auto split = record(1, 6, true);
        split.batchThreads = threads;
        EXPECT_FALSE(raw::usableSplitMetadata(split));
    }
    for (unsigned yz : {0u, 1u, 0x10000u, 0x10002u, 0x20001u}) {
        auto split = record(1, 6, true);
        split.qmd[19] = yz;
        EXPECT_FALSE(raw::usableSplitMetadata(split));
    }
}

TEST(RawSplitMetadata, CostInputsAreVersionedAndBoundedByFrontier)
{
    auto split = record(1, 6, true);
    split.cost = {1, 0, 4000, 4000, 97, 81, 12, 0, 100};
    EXPECT_TRUE(raw::usableSplitMetadata(split));
    for (unsigned version : {2u, 255u, 65535u}) {
        split.cost.version = version;
        EXPECT_FALSE(raw::usableSplitMetadata(split));
    }
    split.cost.version = 1;
    split.cost.frontierValues = 1025;
    EXPECT_FALSE(raw::usableSplitMetadata(split));
    split.cost.frontierValues = 100;
    split.cost.savedPixelInstructions = 0;
    EXPECT_FALSE(raw::usableSplitMetadata(split));
    split.cost.savedPixelInstructions = 12;
    split.cost.reserved = 1;
    EXPECT_FALSE(raw::usableSplitMetadata(split));
    split.cost = {};
    EXPECT_TRUE(raw::usableSplitMetadata(split));
}
}  // namespace
