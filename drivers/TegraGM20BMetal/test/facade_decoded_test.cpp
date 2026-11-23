// SPDX-License-Identifier: MIT Textures of the formats decoded on upload (decoded.h: PVRTC, ASTC 4x2 and 8x4, ASTC
// HDR where the identity makes it) through the facade over the kernel model: made as asked, their pixel format,
// levels and storage mode Metal's, over a block-linear image; a PVRTC one not square and a power of two nil, as Metal
// makes none; views of another form made, but not of a texture GM20B decodes itself; getBytes giving kept blocks back;
// an upload into part of a PVRTC level skipped as Metal's validation stops it, and a whole level's drawn by the pack's
// decode state (this test's pack holds none, so its command buffer fails where a copy of the blocks would have
// completed).
#include "facade_harness.h"
#include "../device_answers.h"
#include "../TegraGM20BMetalDecoded.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>
#include <objc/message.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {

using namespace harness;

NSString *facts(id<MTLDevice> device, id<MTLTexture> texture)
{
    return ((NSString * (*)(id, SEL, id)) objc_msgSend)((id)device, sel_registerName("switchMetalProbeFacts:"),
                                                        texture);
}

id<MTLTexture> made(id<MTLDevice> device, uint32_t format, NSUInteger width, NSUInteger height, bool mipmapped,
                    MTLStorageMode storage, MTLTextureUsage usage = MTLTextureUsageShaderRead)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(format)
                                                                                 width:width
                                                                                height:height
                                                                             mipmapped:mipmapped];
    d.storageMode = storage;
    d.usage = usage;
    return [device newTextureWithDescriptor:d];
}

void decodedTexturesAreMadeAsAsked()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);

    struct Ask {
        uint32_t format;
        NSUInteger width, height;
        bool mipmapped;
        MTLStorageMode storage;
    };

    for (const Ask &a : {Ask{166, 64, 64, true, MTLStorageModeShared}, Ask{161, 32, 32, false, MTLStorageModePrivate},
                         Ask{203, 30, 10, true, MTLStorageModePrivate}, Ask{209, 64, 32, false, MTLStorageModeShared},
                         Ask{185, 8, 2, false, MTLStorageModeShared}}) {
        id<MTLTexture> t = made(device, a.format, a.width, a.height, a.mipmapped, a.storage);
        ASSERT_TRUE(t != nil) << a.format;
        EXPECT_EQ(uint32_t(t.pixelFormat), a.format);
        EXPECT_EQ(t.storageMode, a.storage) << a.format;
        EXPECT_EQ(t.mipmapLevelCount, a.mipmapped ? NSUInteger(std::log2(std::max(a.width, a.height))) + 1 : 1u)
            << a.format;
        EXPECT_TRUE([facts(device, t) containsString:@"block_linear=1"]) << a.format << facts(device, t).UTF8String;
    }
    EXPECT_EQ(made(device, 166, 64, 32, false, MTLStorageModeShared), nil) << "not square";
    EXPECT_EQ(made(device, 166, 48, 48, false, MTLStorageModeShared), nil) << "not a power of two";
    MTLTextureDescriptor *cube =
        [MTLTextureDescriptor textureCubeDescriptorWithPixelFormat:MTLPixelFormat(222) size:16 mipmapped:YES];
    cube.usage = MTLTextureUsageShaderRead;
    EXPECT_EQ([device newTextureWithDescriptor:cube] != nil, gm20b::answers::kThisKind.astcHdr)
        << "an ASTC HDR cube: the identity's answer";
    EXPECT_EQ(made(device, 221, 16, 8, false, MTLStorageModePrivate) != nil, gm20b::answers::kThisKind.astcHdr)
        << "ASTC 4x2 HDR: the identity's answer too";
}

void viewsOfTheSameDecodeAlone()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> pvrtc = made(device, 166, 32, 32, false, MTLStorageModePrivate, usage);
    ASSERT_TRUE(pvrtc != nil);
    id<MTLTexture> srgb = [pvrtc newTextureViewWithPixelFormat:MTLPixelFormat(167)];
    ASSERT_TRUE(srgb != nil) << "PVRTC's sRGB form reads the same codes";
    EXPECT_EQ(uint32_t(srgb.pixelFormat), 167u);
    id<MTLTexture> narrow = made(device, 203, 32, 32, true, MTLStorageModePrivate, usage);
    ASSERT_TRUE(narrow != nil);
    id<MTLTexture> codes = [narrow newTextureViewWithPixelFormat:MTLPixelFormat(185)];
    ASSERT_TRUE(codes != nil) << "the sRGB form: an image of its own, of the kept blocks' other decode";
    EXPECT_EQ(uint32_t(codes.pixelFormat), 185u);
    EXPECT_EQ(codes.mipmapLevelCount, narrow.mipmapLevelCount);
    id<MTLTexture> level = [narrow newTextureViewWithPixelFormat:MTLPixelFormat(185)
                                                     textureType:MTLTextureType2D
                                                          levels:NSMakeRange(1, 2)
                                                          slices:NSMakeRange(0, 1)];
    ASSERT_TRUE(level != nil) << "of two of its levels";
    EXPECT_EQ(level.width, 16u);
    EXPECT_EQ([narrow newTextureViewWithPixelFormat:MTLPixelFormat(221)] != nil, gm20b::answers::kThisKind.astcHdr)
        << "its HDR form: the identity's answer, as for an HDR texture";
    EXPECT_EQ([narrow newTextureViewWithPixelFormat:MTLPixelFormat(185)
                                        textureType:MTLTextureType2D
                                             levels:NSMakeRange(5, 3)
                                             slices:NSMakeRange(0, 1)],
              nil)
        << "levels the texture does not have";
    id<MTLTexture> native = made(device, 204, 32, 32, false, MTLStorageModePrivate, usage);
    ASSERT_TRUE(native != nil);
    EXPECT_EQ([native newTextureViewWithPixelFormat:MTLPixelFormat(222)], nil)
        << "a texture GM20B decodes itself keeps no blocks to decode as HDR";
}

// getBytes of a PVRTC texture, which keeps no blocks, moves nothing; of an ASTC one it gives its kept blocks back: a
// new texture's are zeros
void getBytesGivesKeptBlocksBack()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    id<MTLTexture> t = made(device, 166, 32, 32, false, MTLStorageModeShared);
    ASSERT_TRUE(t != nil);
    std::vector<uint8_t> bytes(512, 0xa5);
    [t getBytes:bytes.data() bytesPerRow:0 fromRegion:MTLRegionMake2D(0, 0, 32, 32) mipmapLevel:0];
    EXPECT_EQ(std::count(bytes.begin(), bytes.end(), 0xa5), 512);
    id<MTLTexture> astc = made(device, 209, 32, 16, false, MTLStorageModeShared);
    ASSERT_TRUE(astc != nil);
    std::vector<uint8_t> blocks(4 * 4 * 16, 0xa5);
    [astc getBytes:blocks.data() bytesPerRow:4 * 16 fromRegion:MTLRegionMake2D(0, 0, 32, 16) mipmapLevel:0];
    EXPECT_EQ(std::count(blocks.begin(), blocks.end(), 0), 256);
}

// The command buffer of a blit of a buffer's PVRTC rows into `t`, waited for
id<MTLCommandBuffer> uploaded(id<MTLDevice> device, id<MTLTexture> t, MTLOrigin origin, MTLSize size, NSUInteger row)
{
    id<MTLBuffer> blocks = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:blocks
               sourceOffset:0
          sourceBytesPerRow:row
        sourceBytesPerImage:row * 8
                 sourceSize:size
                  toTexture:t
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:origin
                    options:MTLBlitOptionRowLinearPVRTC];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb;
}

void uploadsAreDecodedByThePacksState()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    id<MTLTexture> t = made(device, 166, 32, 32, false, MTLStorageModePrivate);
    ASSERT_TRUE(t != nil);
    EXPECT_EQ(uploaded(device, t, MTLOriginMake(16, 16, 0), MTLSizeMake(16, 16, 1), 32).status,
              MTLCommandBufferStatusCompleted)
        << "part of a PVRTC level: skipped, nothing recorded";
    EXPECT_EQ(uploaded(device, t, MTLOriginMake(0, 0, 0), MTLSizeMake(32, 32, 1), 64).status,
              MTLCommandBufferStatusError)
        << "the whole level: drawn by a decode state, which this pack lacks";
}

// What HW Test's decode cost and lifetime tests read (debug builds): switchMetalMemory's textures holds a decoded
// texture's image and kept blocks, and its view of another form's image, while they live, and is back where it began
// once they go (the view's once the blit that made its image completed); switchMetalCounters' rings_outgrown is the
// device's count of rings a command buffer outgrew
void debugKeysCountTexturesAndRings()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const auto textures = [&] {
        return [memory(device)[@"textures"] unsignedLongLongValue];
    };
    const MTLTextureUsage usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    const uint64_t before = textures();
    uint64_t held = 0;
    @autoreleasepool {
        id<MTLTexture> narrow = made(device, 203, 32, 32, true, MTLStorageModePrivate, usage);
        ASSERT_TRUE(narrow != nil);
        held = textures();
        EXPECT_GT(held, before) << "its image and its kept blocks";
    }
    EXPECT_EQ(textures(), before) << "its image and kept blocks went with it";
    @autoreleasepool {
        id<MTLTexture> narrow = made(device, 203, 32, 32, true, MTLStorageModePrivate, usage);
        ASSERT_TRUE(narrow != nil);
        id<MTLTexture> codes = [narrow newTextureViewWithPixelFormat:MTLPixelFormat(185)];
        ASSERT_TRUE(codes != nil);
        EXPECT_GT(textures(), held) << "the view's image of its own";
    }
    id<MTLCommandBuffer> after = [gm20bDecodeQueue(device) commandBuffer];
    [after commit];
    [after waitUntilCompleted];
    EXPECT_EQ(textures(), before) << "the family went with its last reference";
    const uint64_t rings = [counters(device)[@"rings_outgrown"] unsignedLongLongValue];
    gRM->raw->stats.ringsOutgrown += 3;
    EXPECT_EQ([counters(device)[@"rings_outgrown"] unsignedLongLongValue], rings + 3);
}

TEST(GM20BFacadeDecoded, DecodedTexturesAreMadeAsAsked)
{
    switch_ios::test::ExpectPassesInFreshProcess(decodedTexturesAreMadeAsAsked);
}

TEST(GM20BFacadeDecoded, ViewsOfTheSameDecodeAlone)
{
    switch_ios::test::ExpectPassesInFreshProcess(viewsOfTheSameDecodeAlone);
}

TEST(GM20BFacadeDecoded, GetBytesGivesKeptBlocksBack)
{
    switch_ios::test::ExpectPassesInFreshProcess(getBytesGivesKeptBlocksBack);
}

TEST(GM20BFacadeDecoded, UploadsAreDecodedByThePacksState)
{
    switch_ios::test::ExpectPassesInFreshProcess(uploadsAreDecodedByThePacksState);
}

TEST(GM20BFacadeDecoded, DebugKeysCountTexturesAndRings)
{
    switch_ios::test::ExpectPassesInFreshProcess(debugKeysCountTexturesAndRings);
}

}  // namespace
