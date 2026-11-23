// SPDX-License-Identifier: MIT
// The colour targets a pass binds for the attachments the pass-begin sites sent to m2v (attachment_target.h): a slice of
// a linear 2D array as one pitch layer (a shadow where its address is off 128), a texture buffer's row (none past 16384
// texels), and a plane of a 3D texture laid out several planes a block, which no target addresses (the pass renders an
// image of it) and no 2D header reads.
#include "device_harness.h"
#include "../attachment_target.h"

#include <gtest/gtest.h>

namespace {

constexpr uint32_t kRGBA8 = 70, kA1BGR5 = 41;

gm20b::Texture linearArray(uint64_t va, uint32_t pitch, uint32_t layers)
{
    gm20b::Texture t;
    t.va = va;
    t.width = t.texelWidth = 64;
    t.height = t.texelHeight = 32;
    t.format = kRGBA8;
    t.bpp = 4;
    t.pitch = pitch;
    t.layers = layers;
    t.layerBytes = uint64_t(pitch) * 32;
    t.type = gm20b::fx::kTexTwoDArray;
    return t;
}

TEST(DevicePassTargets, ASliceOfALinearArrayIsOnePitchLayer)
{
    const gm20b::Texture t = linearArray(0x100000, 256, 3);
    gm20b::Texture part;
    ASSERT_TRUE(gm20b::attachmentTarget(t, 0, 2, part));
    EXPECT_EQ(part.va, t.va + 2 * t.layerBytes);
    EXPECT_FALSE(part.blockLinear);
    EXPECT_EQ(part.layers, 1u);
    EXPECT_EQ(part.type, gm20b::fx::kTexTwoD);
    EXPECT_TRUE(part.renderable);
    uint32_t w[8];
    EXPECT_FALSE(gm20b::attachmentHeader(part, 0, false, w)) << "a block-linear header of a pitch slice";
    EXPECT_FALSE(gm20b::attachmentTarget(t, 0, 3, part));
    EXPECT_FALSE(gm20b::attachmentTarget(t, 1, 0, part));
    // rows 288 bytes apart: a pitch off 128, rendered through a block-linear shadow
    const gm20b::Texture odd = linearArray(0x100000, 288, 2);
    ASSERT_TRUE(gm20b::attachmentTarget(odd, 0, 1, part));
    EXPECT_FALSE(part.renderable);
    EXPECT_TRUE(part.shadowTarget);
}

TEST(DevicePassTargets, ATextureBufferIsItsRowUpTo16384Texels)
{
    gm20b::Texture t;
    t.va = 0x200000;
    t.format = kRGBA8;
    t.bpp = 4;
    t.width = t.texelWidth = 4096;
    t.height = t.texelHeight = 1;
    t.pitch = 4096 * 4;
    t.type = gm20b::fx::kTexOneDBuffer;
    gm20b::Texture row;
    ASSERT_TRUE(gm20b::texelBufferTarget(t, row));
    EXPECT_EQ(row.type, gm20b::fx::kTexTwoD);
    EXPECT_EQ(row.height, 1u);
    EXPECT_EQ(row.va, t.va);
    EXPECT_TRUE(row.renderable);
    t.va += 16;
    ASSERT_TRUE(gm20b::texelBufferTarget(t, row));
    EXPECT_TRUE(row.shadowTarget);
    t.width = 16385;
    EXPECT_FALSE(gm20b::texelBufferTarget(t, row));
}

// A YCbCr texture buffer's image wider than 16384 texels: rows of 16384 back to back, which its conversion draws as one
// pitch target of its rows, an M4 Max reading texel i at its plane's (i % 16384, i / 16384)
TEST(DevicePassTargets, AWideTextureBufferIsRowsOf16384Texels)
{
    gm20b::Texture t;
    t.va = 0x200000;
    t.format = kRGBA8;
    t.bpp = 4;
    t.width = t.texelWidth = 20000;
    t.height = t.texelHeight = 1;
    t.pitch = 16384 * 4;
    t.type = gm20b::fx::kTexOneDBuffer;
    gm20b::Texture rows;
    ASSERT_TRUE(gm20b::texelBufferRows(t, rows));
    EXPECT_EQ(rows.type, gm20b::fx::kTexTwoD);
    EXPECT_EQ(rows.width, 16384u);
    EXPECT_EQ(rows.height, 2u);
    EXPECT_EQ(rows.pitch, 16384u * 4);
    EXPECT_TRUE(rows.renderable);
    EXPECT_FALSE(gm20b::texelBufferTarget(t, rows)) << "no pass of the app's renders into one past 16384 texels";
}

class DevicePassTargetsVolume : public harness::DeviceTest {};

TEST_F(DevicePassTargetsVolume, APlaneInsideABlockOfSeveralIsRenderedThroughAnImage)
{
    gm20b::Texture deep, flat, standIn;
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, false, deep));
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kRGBA8, true, flat));
    ASSERT_TRUE(dev.createVolume(16, 16, 8, kA1BGR5, false, standIn));
    ASSERT_GT(deep.blockDepth, 0u);
    EXPECT_TRUE(gm20b::planeInBlocks(deep, 0, 7));
    EXPECT_TRUE(gm20b::planeInBlocks(standIn, 0, 1));
    EXPECT_FALSE(gm20b::planeInBlocks(deep, 0, 8));
    EXPECT_FALSE(gm20b::planeInBlocks(flat, 0, 3)) << "one plane a block: a part of its own";
    gm20b::Texture part;
    EXPECT_TRUE(gm20b::attachmentTarget(flat, 0, 3, part));
    EXPECT_FALSE(gm20b::attachmentTarget(deep, 0, 3, part));
}

}  // namespace
