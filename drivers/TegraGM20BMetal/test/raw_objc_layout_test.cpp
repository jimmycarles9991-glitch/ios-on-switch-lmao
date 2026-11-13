// SPDX-License-Identifier: MIT
#include "../raw/runtime/raw_objc_layout.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

// Metal's own render pass descriptor ivars as macOS 27's runtime encodes them (ivar_getTypeEncoding): the descriptor's
// _private at +48 of a 256-byte instance, an attachment's _private at +8 of a 184-byte one
const char *kDescriptorPrivate =
    "{MTLRenderPassDescriptorPrivate=\"attachments\"@\"MTLRenderPassColorAttachmentDescriptorArrayInternal\""
    "\"visibilityResultBuffer\"@\"<MTLBuffer>\"\"renderTargetWidth\"Q\"renderTargetHeight\"Q"
    "\"defaultColorSampleCount\"Q\"fineGrainedBackgroundVisibilityEnabled\"B\"skipEmptyTilesOnClearEnabled\"B"
    "\"ditherEnabled\"B\"openGLModeEnabled\"B\"renderTargetArrayLength\"Q\"tileWidth\"Q\"tileHeight\"Q"
    "\"\"(?=\"defaultSampleCount\"Q\"defaultRasterSampleCount\"Q)\"imageBlockSampleLength\"Q"
    "\"threadgroupMemoryLength\"Q\"customSamplePositions\"[8{?=\"x\"f\"y\"f}]\"numCustomSamplePositions\"Q"
    "\"rasterizationRateMap\"@\"<MTLRasterizationRateMap>\"\"sampleBufferAttachments\""
    "@\"MTLRenderPassSampleBufferAttachmentDescriptorArrayInternal\"\"pointCoordYFlipEnabled\"B"
    "\"visibilityResultType\"q\"supportColorAttachmentMapping\"B\"useExplicitRenderTargetDimensions\"B}";
const char *kAttachmentPrivate =
    "{MTLRenderPassAttachmentDescriptorPrivate=\"texture\"@\"<MTLTextureImplementation>\"\"level\"Q\"slice\"Q"
    "\"depthPlane\"Q\"loadAction\"Q\"storeAction\"Q\"storeActionOptions\"Q\"clearColor\"{?=\"red\"d\"green\"d"
    "\"blue\"d\"alpha\"d}\"clearDepth\"d\"clearStencil\"I\"stencilResolveFilter\"Q\"resolveTexture\""
    "@\"<MTLTextureImplementation>\"\"resolveLevel\"Q\"resolveSlice\"Q\"resolveDepthPlane\"Q\"resolveFilter\"Q"
    "\"width\"Q\"height\"Q\"yInvert\"B}";

TEST(RawObjcLayout, TheDescriptorsPrivateStructFillsItsInstance)
{
    std::vector<raw::objc::Field> fields;
    size_t size = 0;
    ASSERT_TRUE(raw::objc::structFields(kDescriptorPrivate, fields, size));
    EXPECT_EQ(size, 256u - 48u);
    const auto *attachments = raw::objc::field(fields, "attachments", '@');
    const auto *samples = raw::objc::field(fields, "sampleBufferAttachments", '@');
    const auto *map = raw::objc::field(fields, "rasterizationRateMap", '@');
    ASSERT_TRUE(attachments && samples && map);
    EXPECT_EQ(attachments->offset, 0u);
    EXPECT_EQ(raw::objc::field(fields, "renderTargetArrayLength")->offset, 48u);
    EXPECT_EQ(raw::objc::field(fields, "customSamplePositions")->size, 64u);
    EXPECT_EQ(map->offset, 168u);
    EXPECT_EQ(samples->offset, 176u);
    EXPECT_EQ(raw::objc::field(fields, "useExplicitRenderTargetDimensions")->offset, 201u);
}

TEST(RawObjcLayout, AnAttachmentsPrivateStructFillsItsInstance)
{
    std::vector<raw::objc::Field> fields;
    size_t size = 0;
    ASSERT_TRUE(raw::objc::structFields(kAttachmentPrivate, fields, size));
    EXPECT_EQ(size, 184u - 8u);
    EXPECT_EQ(raw::objc::field(fields, "texture", '@')->offset, 0u);
    EXPECT_EQ(raw::objc::field(fields, "clearColor", '{')->size, 32u);
    EXPECT_EQ(raw::objc::field(fields, "clearStencil")->offset, 96u);
    EXPECT_EQ(raw::objc::field(fields, "resolveTexture", '@')->offset, 112u);
    EXPECT_EQ(raw::objc::field(fields, "yInvert")->offset, 168u);
}

// A bare object field's quoted text after it is the next field's name, not a class name
TEST(RawObjcLayout, AnObjectWithNoClassNameIsFollowedByTheNextFieldsName)
{
    std::vector<raw::objc::Field> fields;
    size_t size = 0;
    ASSERT_TRUE(raw::objc::structFields("{S=\"a\"@\"b\"Q\"c\"@\"NSObject\"}", fields, size));
    ASSERT_EQ(fields.size(), 3u);
    EXPECT_EQ(fields[1].name, "b");
    EXPECT_EQ(fields[1].offset, 8u);
    EXPECT_EQ(fields[2].offset, 16u);
    EXPECT_EQ(size, 24u);
}

TEST(RawObjcLayout, WhatTheEncodingCannotLayOutIsRefused)
{
    std::vector<raw::objc::Field> fields;
    size_t size = 0;
    EXPECT_FALSE(raw::objc::structFields("{S=\"a\"b3\"b\"Q}", fields, size));
    EXPECT_FALSE(raw::objc::structFields("{S=\"a\"{Incomplete}}", fields, size));
    EXPECT_FALSE(raw::objc::structFields("Q", fields, size));
    EXPECT_TRUE(raw::objc::structFields("{S=\"p\"^{Incomplete}\"c\"c}", fields, size));
    EXPECT_EQ(size, 16u);
}

}  // namespace
