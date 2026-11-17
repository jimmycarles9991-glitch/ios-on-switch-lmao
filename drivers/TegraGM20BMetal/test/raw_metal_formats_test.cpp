// SPDX-License-Identifier: MIT
// raw/runtime/raw_metal_formats.h, every kind: each format the device gives a linear alignment has a texel Metal describes,
// so a linear row is the device's own answer; formats of blocks and depth formats answer no row; the description agrees
// with the capability table's bytes and blocks wherever both have the format.
#include "../raw/runtime/raw_device_answers.h"
#include "../raw/runtime/raw_metal_formats.h"

#include <gtest/gtest.h>

#include <cstring>

namespace {

namespace mf = raw::metal_formats;

uint64_t linearAlignment(const raw::answers::Kind &k, uint64_t format)
{
    for (size_t i = 0; i < k.argCount; i++)
        if (!strcmp(k.args[i].selector, "minimumLinearTextureAlignmentForPixelFormat:") && format >= k.args[i].first &&
            format <= k.args[i].last)
            return k.args[i].value;
    return 0;
}

TEST(RawMetalFormats, EveryFormatWithALinearAlignmentHasATexel)
{
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        for (uint32_t format = 0; format < 2048; format++)
            if (linearAlignment(k, format))
                EXPECT_GT(mf::texelBytes(format), 0u) << k.kind << " format " << format;
}

TEST(RawMetalFormats, FormatsOfBlocksAndDepthFormatsHaveNoRow)
{
    constexpr uint32_t kBC1 = 130, kASTC4x4 = 204, kGBGR422 = 240, kDepth32Float = 252, kUndescribed = 2047;
    for (uint32_t format : {kBC1, kASTC4x4, kGBGR422, kUndescribed})
        EXPECT_EQ(mf::texelBytes(format), 0u) << format;
    EXPECT_EQ(mf::described(kASTC4x4)->blockW, 4u);
    EXPECT_EQ(mf::described(kGBGR422)->blockW, 2u);
    EXPECT_EQ(mf::described(kGBGR422)->blockH, 1u);
    EXPECT_EQ(mf::texelBytes(kDepth32Float), 4u);
    for (const raw::answers::Kind &k : raw::answers::kKinds)
        EXPECT_EQ(mf::linearRow(100, mf::texelBytes(kDepth32Float), linearAlignment(k, kDepth32Float)), 0u) << k.kind;
}

TEST(RawMetalFormats, ARowIsItsTexelsRoundedUpToTheAlignment)
{
    EXPECT_EQ(mf::linearRow(100, 1, 32), 128u);
    EXPECT_EQ(mf::linearRow(100, 4, 32), 416u);
    EXPECT_EQ(mf::linearRow(8, 4, 32), 32u);
    EXPECT_EQ(mf::linearRow(1, 16, 256), 256u);
    EXPECT_EQ(mf::linearRow(100, 4, 0), 0u);
    EXPECT_EQ(mf::linearRow(100, 0, 32), 0u);
}

// Depth and stencil formats aside: the table's X32_Stencil8 is Mesa's whole Z32_S8X24 texel where Metal describes the
// stencil view's byte
TEST(RawMetalFormats, TheCapabilityTableAgreesWhereBothHaveAFormat)
{
    size_t compared = 0;
    for (const switch_gm20b_format &f : switch_gm20b_formats) {
        const mf::Described *d = mf::described(f.mtl);
        if (!f.bytes || !d || (f.mtl >= 250 && f.mtl <= 262))
            continue;
        compared++;
        EXPECT_EQ(d->bytes, f.bytes) << f.mtl;
        EXPECT_EQ(d->blockW, f.block_w) << f.mtl;
        EXPECT_EQ(d->blockH, f.block_h) << f.mtl;
    }
    EXPECT_GT(compared, 100u);
}

}  // namespace
