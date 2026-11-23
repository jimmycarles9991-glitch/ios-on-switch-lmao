// SPDX-License-Identifier: MIT
#include "../split_metadata.h"
#include <gtest/gtest.h>

namespace {
gm20b::PackSplit record(unsigned version, unsigned ubos, bool data)
{
    gm20b::PackSplit split = {};
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

TEST(GM20BSplitMetadata, ScalarAndBatchKeepDistinctUniformAndShaderDataBindings)
{
    for (unsigned version : {0u, 1u})
        for (unsigned ubos = 0; ubos <= 6; ++ubos)
            for (bool data : {false, true})
                EXPECT_TRUE(gm20b::usableSplitMetadata(record(version, ubos, data)));
}

TEST(GM20BSplitMetadata, RejectsCountsBeforeTraversingUniformSlots)
{
    for (unsigned version : {0u, 1u})
        for (unsigned count = 7; count <= 255; ++count) {
            auto split = record(version, 6, true);
            split.uboCount = count;
            EXPECT_FALSE(gm20b::usableSplitMetadata(split));
        }
}

TEST(GM20BSplitMetadata, FragmentBindingsAreDistinctAndPrecedeFrontier)
{
    auto split = record(1, 2, false);
    split.uboCbuf[1] = split.uboCbuf[0];
    EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    for (unsigned slot = split.frontierCbuf; slot <= 255; ++slot) {
        split.uboCbuf[1] = slot;
        EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    }
    for (unsigned bytes : {0u, 1u, 4095u, 4104u, 65528u, 65535u}) {
        split = record(1, 2, false);
        split.frontierBytes = uint16_t(bytes);
        EXPECT_FALSE(gm20b::usableSplitMetadata(split)) << bytes;
    }
    for (unsigned slot : {0u, 16u, 255u}) {
        split = record(1, 2, false);
        split.frontierCbuf = slot;
        EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    }
}

// One constant buffer's worth, as the chip binds one (cbuf.h): every 16-byte frontier up to 64 KiB less 16
TEST(GM20BSplitMetadata, AFrontierIsUsableUpToAConstantBuffersLastSixteenBytes)
{
    for (unsigned bytes : {16u, 4096u, 4112u, 8192u, 0xFFF0u}) {
        auto split = record(1, 2, false);
        split.frontierBytes = uint16_t(bytes);
        EXPECT_TRUE(gm20b::usableSplitMetadata(split)) << bytes;
    }
}

TEST(GM20BSplitMetadata, ScalarConstantsCannotOverwriteUniforms)
{
    for (unsigned slot = 1; slot <= 6; ++slot) {
        auto split = record(0, 6, true);
        split.shaderDataCbuf = slot;
        EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    }
    auto split = record(0, 6, true);
    --split.prologueCbufs;
    EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    split = record(0, 0, false);
    split.batchThreads = 1;
    EXPECT_FALSE(gm20b::usableSplitMetadata(split));
}

TEST(GM20BSplitMetadata, BatchConstantsUseOnlySharedSecondBuffer)
{
    for (unsigned slot = 2; slot <= 255; ++slot) {
        auto split = record(1, 6, true);
        split.shaderDataCbuf = slot;
        EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    }
    for (unsigned count : {0u, 2u, 3u, 8u, 255u}) {
        auto split = record(1, 6, false);
        split.prologueCbufs = count;
        EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    }
}

TEST(GM20BSplitMetadata, RejectsUnknownOrMismatchedLaunchAbi)
{
    for (unsigned version : {2u, 255u, 65535u}) {
        auto split = record(1, 6, true);
        split.batchVersion = version;
        EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    }
    for (unsigned threads : {0u, 31u, 33u, 1025u, 65535u}) {
        auto split = record(1, 6, true);
        split.batchThreads = threads;
        EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    }
    for (unsigned yz : {0u, 1u, 0x10000u, 0x10002u, 0x20001u}) {
        auto split = record(1, 6, true);
        split.qmd[19] = yz;
        EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    }
}

TEST(GM20BSplitMetadata, CostInputsAreVersionedAndBoundedByFrontier)
{
    auto split = record(1, 6, true);
    split.cost = {1, 0, 4000, 4000, 97, 81, 12, 0, 100};
    EXPECT_TRUE(gm20b::usableSplitMetadata(split));
    for (unsigned version : {2u, 255u, 65535u}) {
        split.cost.version = version;
        EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    }
    split.cost.version = 1;
    split.cost.frontierValues = 1025;
    EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    split.cost.frontierValues = 100;
    split.cost.savedPixelInstructions = 0;
    EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    split.cost.savedPixelInstructions = 12;
    split.cost.reserved = 1;
    EXPECT_FALSE(gm20b::usableSplitMetadata(split));
    split.cost = {};
    EXPECT_TRUE(gm20b::usableSplitMetadata(split));
}
}  // namespace
