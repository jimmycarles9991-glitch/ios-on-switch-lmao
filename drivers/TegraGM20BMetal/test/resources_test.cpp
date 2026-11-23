// SPDX-License-Identifier: MIT
// What the resource and pass records work out themselves (resources.h, pass_desc.h): a block-compressed copy's texels as
// whole blocks, an attachment's layers from a slice, and the image of a written two-plane texture past a pass's inline
// ones. pass_desc.h comes first, so it and resources.h compile with nothing included before them.
#include "../pass_desc.h"

#include <gtest/gtest.h>

namespace {

// Metal's copy of a block-compressed texture starts on a block and may end inside one, at the texture's edge
TEST(GM20BResources, ABlockCopyStartsOnABlockAndTakesTheBlockItsEndReaches)
{
    gm20b::Texture blocks;
    blocks.blockW = 4;
    blocks.blockH = 4;
    uint32_t x = 8, y = 4, w = 9, h = 5;
    ASSERT_TRUE(gm20b::toElements(blocks, x, y, w, h));
    EXPECT_EQ(x, 2u);
    EXPECT_EQ(y, 1u);
    EXPECT_EQ(w, 3u);
    EXPECT_EQ(h, 2u);
    uint32_t inside = 6, row = 4, ow = 4, oh = 4;
    EXPECT_FALSE(gm20b::toElements(blocks, inside, row, ow, oh));
    gm20b::Texture plain;
    uint32_t px = 3, py = 5, pw = 7, ph = 9;
    ASSERT_TRUE(gm20b::toElements(plain, px, py, pw, ph));
    EXPECT_EQ(px, 3u);
    EXPECT_EQ(py, 5u);
    EXPECT_EQ(pw, 7u);
    EXPECT_EQ(ph, 9u);
}

TEST(GM20BResources, AnAttachmentsLayersFromASliceHaveNoHeaderOfTheirParents)
{
    gm20b::Texture array;
    array.va = 0x100000;
    array.layerBytes = 0x10000;
    array.layers = 6;
    array.tic = 7;
    array.width = 64;
    array.format = 70;
    const gm20b::Texture part = gm20b::layersFrom(array, 2);
    EXPECT_EQ(part.va, 0x120000u);
    EXPECT_EQ(part.layers, 4u);
    EXPECT_EQ(part.tic, 0u) << "the array's header reads from its layer 0";
    EXPECT_EQ(part.width, 64u);
    EXPECT_EQ(part.format, 70u);
}

TEST(GM20BResources, APassFindsAWrittenTexturesImagePastItsInlineOnes)
{
    gm20b::Texture inlineTexture, inlineImage, other, more, moreImage, absent;
    gm20b::PassDesc pass;
    pass.writtenPlanes[3] = {&inlineTexture, &inlineImage};
    const gm20b::PassDesc::WrittenPlanes extra[2] = {{&other, &inlineImage}, {&more, &moreImage}};
    pass.morePlanes = extra;
    pass.morePlaneCount = 2;
    EXPECT_EQ(pass.writtenImage(inlineTexture), &inlineImage);
    EXPECT_EQ(pass.writtenImage(more), &moreImage);
    EXPECT_EQ(pass.writtenImage(absent), nullptr);
}

}  // namespace
