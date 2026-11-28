// SPDX-License-Identifier: MIT
#include "facade_blit_engines.h"
#include "../TegraGM20BMetalInternal.h"
#include "../blit_overlap.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {

using namespace harness;

id<MTLDevice> gDevice;
id<MTLCommandQueue> gQueue;

class FacadeBlitOverlap : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
        ASSERT_NE(gDevice, nil);
        gQueue = [gDevice newCommandQueue];
        ASSERT_NE(gQueue, nil);
    }

    static id<MTLTexture> makePitchTexture(NSUInteger w, NSUInteger h, MTLPixelFormat fmt = MTLPixelFormatRGBA8Unorm)
    {
        const NSUInteger pitch = (w * 4 + 255) & ~NSUInteger(255);
        id<MTLBuffer> buf = [gDevice newBufferWithLength:pitch * h options:MTLResourceStorageModeShared];
        EXPECT_NE(buf, nil);
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:NO];
        d.storageMode = MTLStorageModeShared;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        return [buf newTextureWithDescriptor:d offset:0 bytesPerRow:pitch];
    }

    static id<MTLTexture> makeTiledTexture(NSUInteger w, NSUInteger h, MTLPixelFormat fmt = MTLPixelFormatRGBA8Unorm,
                                           bool mipmapped = true, NSUInteger arrayLength = 1)
    {
        MTLTextureDescriptor *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt width:w height:h mipmapped:mipmapped];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        if (arrayLength > 1) {
            d.textureType = MTLTextureType2DArray;
            d.arrayLength = arrayLength;
        }
        return [gDevice newTextureWithDescriptor:d];
    }

    static std::vector<uint8_t> fillPattern(NSUInteger w, NSUInteger h, NSUInteger bpp, uint8_t seed = 0)
    {
        std::vector<uint8_t> out(w * h * bpp);
        for (NSUInteger y = 0; y < h; ++y)
            for (NSUInteger x = 0; x < w; ++x)
                for (NSUInteger c = 0; c < bpp; ++c)
                    out[(y * w + x) * bpp + c] = uint8_t((x * 17 + y * 53 + c * 7 + seed * 31 + 11) & 0xff);
        return out;
    }

    static void applyCpuMove(std::vector<uint8_t> &img, NSUInteger w, NSUInteger bpp, NSUInteger sx, NSUInteger sy,
                             NSUInteger dx, NSUInteger dy, NSUInteger cw, NSUInteger ch)
    {
        std::vector<uint8_t> snap(cw * ch * bpp);
        for (NSUInteger y = 0; y < ch; ++y)
            memcpy(snap.data() + y * cw * bpp, img.data() + ((sy + y) * w + sx) * bpp, cw * bpp);
        for (NSUInteger y = 0; y < ch; ++y)
            memcpy(img.data() + ((dy + y) * w + dx) * bpp, snap.data() + y * cw * bpp, cw * bpp);
    }

    static void uploadRect(id<MTLTexture> t, NSUInteger level, NSUInteger slice, NSUInteger w, NSUInteger h,
                           NSUInteger bpp, const std::vector<uint8_t> &data, NSUInteger blockW = 1,
                           NSUInteger blockH = 1, MTLBlitOption opt = MTLBlitOptionNone)
    {
        const NSUInteger bw = (w + blockW - 1) / blockW, bh = (h + blockH - 1) / blockH;
        const NSUInteger row = bw * bpp, bytes = row * bh;
        id<MTLBuffer> buf =
            [gDevice newBufferWithLength:std::max<NSUInteger>(bytes, 256) options:MTLResourceStorageModeShared];
        ASSERT_NE(buf, nil);
        memcpy(buf.contents, data.data(), bytes);
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:buf
                   sourceOffset:0
              sourceBytesPerRow:row
            sourceBytesPerImage:bytes
                     sourceSize:MTLSizeMake(w, h, 1)
                      toTexture:t
               destinationSlice:slice
               destinationLevel:level
              destinationOrigin:MTLOriginMake(0, 0, 0)
                        options:opt];
        [e endEncoding];
        EngineRun run;
        ASSERT_NO_FATAL_FAILURE(commitOnRaw(cb, run));
    }

    static std::vector<uint8_t> readbackRect(id<MTLTexture> t, NSUInteger level, NSUInteger slice, NSUInteger w,
                                             NSUInteger h, NSUInteger bpp, NSUInteger blockW = 1, NSUInteger blockH = 1,
                                             MTLBlitOption opt = MTLBlitOptionNone)
    {
        const NSUInteger bw = (w + blockW - 1) / blockW, bh = (h + blockH - 1) / blockH;
        const NSUInteger row = bw * bpp, bytes = row * bh;
        id<MTLBuffer> buf =
            [gDevice newBufferWithLength:std::max<NSUInteger>(bytes, 256) options:MTLResourceStorageModeShared];
        EXPECT_NE(buf, nil);
        memset(buf.contents, 0xcc, bytes);
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:t
                         sourceSlice:slice
                         sourceLevel:level
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:buf
                   destinationOffset:0
              destinationBytesPerRow:row
            destinationBytesPerImage:bytes
                             options:opt];
        [e endEncoding];
        EngineRun run;
        commitOnRaw(cb, run);
        std::vector<uint8_t> out(bytes);
        memcpy(out.data(), buf.contents, bytes);
        return out;
    }

    static EngineRun runCopy(id<MTLTexture> src, NSUInteger ss, NSUInteger sl, MTLOrigin so, MTLSize sz,
                             id<MTLTexture> dst, NSUInteger ds, NSUInteger dl, MTLOrigin dorg,
                             size_t *planAdded = nullptr)
    {
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        const size_t before = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
        [e copyFromTexture:src
                  sourceSlice:ss
                  sourceLevel:sl
                 sourceOrigin:so
                   sourceSize:sz
                    toTexture:dst
             destinationSlice:ds
             destinationLevel:dl
            destinationOrigin:dorg];
        if (planAdded)
            *planAdded = ((SwitchMetalCommandBuffer *)cb)->_plan.size() - before;
        [e endEncoding];
        EngineRun run;
        commitOnRaw(cb, run);
        return run;
    }
};

TEST(BlitOverlapMath, CopySideAndVolumeOverlapClassification)
{
    using gm20b::ArenaStorage;
    using gm20b::CopyOverlap;
    using gm20b::CopySurface;
    using gm20b::VolumeOverlap;

    const CopySurface p0{0x10000, 256};
    EXPECT_EQ(gm20b::copySideOverlap(p0, p0, 4, 4, 4, 4), CopyOverlap::Identity);
    EXPECT_EQ(gm20b::copySideOverlap(p0, {0x10000, 512}, 4, 1, 4, 4), CopyOverlap::Identity);
    EXPECT_EQ(gm20b::copySideOverlap(p0, {0x10000, 512}, 4, 2, 4, 4), CopyOverlap::Overlap);
    EXPECT_EQ(gm20b::copySideOverlap(p0, p0, 4, 4, 4, 4, false), CopyOverlap::Overlap);
    EXPECT_EQ(gm20b::copySideOverlap(p0, {0x10000 + 16, 256}, 4, 4, 4, 4), CopyOverlap::None);
    EXPECT_EQ(gm20b::copySideOverlap(p0, {0x10000 + 12, 256}, 4, 4, 4, 4), CopyOverlap::Overlap);
    EXPECT_EQ(gm20b::copySideOverlap({0x10000 + 16, 256}, {0x10000 + 256, 256}, 4, 4, 4, 4), CopyOverlap::None);
    EXPECT_EQ(gm20b::copySideOverlap({0x10000 + 12, 256}, {0x10000 + 256, 256}, 4, 4, 4, 4), CopyOverlap::Overlap);
    EXPECT_EQ(gm20b::copySideOverlap({0x10000 + 256, 256}, {0x10000 + 12, 256}, 4, 4, 4, 4), CopyOverlap::Overlap);

    const CopySurface bl0{0x20000, 256, 4, 4, 32, true, 1, 1, 0, 0};
    EXPECT_EQ(gm20b::copySideOverlap(bl0, bl0, 8, 8, 4, 4), CopyOverlap::Identity);
    EXPECT_EQ(gm20b::copySideOverlap(bl0, {0x20000, 256, 4, 4, 32, true, 1, 1, 1, 0}, 8, 8, 4, 4), CopyOverlap::None);
    EXPECT_EQ(gm20b::copySideOverlap(bl0, {0x20000, 256, 12, 4, 32, true, 1, 1, 0, 0}, 8, 8, 4, 4), CopyOverlap::None);
    EXPECT_EQ(gm20b::copySideOverlap(bl0, {0x20000, 256, 11, 11, 32, true, 1, 1, 0, 0}, 8, 8, 4, 4),
              CopyOverlap::Overlap);
    EXPECT_EQ(gm20b::copySideOverlap(bl0, {0x20000, 512, 0, 0, 32, true, 1, 1, 0, 0}, 8, 8, 4, 4),
              CopyOverlap::Overlap);
    EXPECT_EQ(gm20b::copySideOverlap(bl0, {0x80000, 512, 0, 0, 32, true, 1, 1, 0, 0}, 8, 8, 4, 4), CopyOverlap::None);

    ArenaStorage a1, a2, a3;
    a1.arenaId = a2.arenaId = a3.arenaId = 7;
    a1.kind = 0;
    a1.offset = 0;
    a1.first = {0x10000, 4096};
    a2.kind = 0xfe;
    a2.offset = 1024;
    a2.first = {0x90000, 4096};
    a3.kind = 0xfe;
    a3.offset = 8192;
    a3.first = {0xa0000, 4096};
    EXPECT_EQ(gm20b::copySideOverlap(p0, {0x10000 + 16, 256}, 4, 4, 4, 4, true, &a1, &a1), CopyOverlap::None);
    EXPECT_EQ(gm20b::copySideOverlap(p0, {0x90000, 256, 0, 0, 16, true, 0}, 4, 4, 4, 4, true, &a1, &a2),
              CopyOverlap::Overlap);
    EXPECT_EQ(gm20b::copySideOverlap(p0, {0xa0000, 256, 0, 0, 16, true, 0}, 4, 4, 4, 4, true, &a1, &a3),
              CopyOverlap::None);

    gm20b::Texture vol;
    vol.va = 0x40000;
    vol.width = vol.height = 16;
    vol.depth = 4;
    vol.pitch = 64;
    vol.bpp = 4;
    vol.blockLinear = true;
    vol.blockHeight = 1;
    vol.blockDepth = 1;
    EXPECT_EQ(gm20b::volumePlanesOverlap(vol, 0, 0, 0, vol, 0, 0, 1, 8, 8, 2), VolumeOverlap::Reverse);
    EXPECT_EQ(gm20b::volumePlanesOverlap(vol, 0, 0, 1, vol, 0, 0, 0, 8, 8, 2), VolumeOverlap::Forward);
    EXPECT_EQ(gm20b::volumePlanesOverlap(vol, 0, 0, 0, vol, 2, 2, 0, 8, 8, 2), VolumeOverlap::None);
    EXPECT_EQ(gm20b::volumePlanesOverlap(vol, 0, 0, 0, vol, 0, 0, 2, 8, 8, 2), VolumeOverlap::None);
    EXPECT_EQ(gm20b::volumePlanesOverlap(vol, 0, 0, 0, vol, 8, 0, 1, 8, 8, 2), VolumeOverlap::None);
}

TEST_F(FacadeBlitOverlap, PitchOverlappingShiftsAllEightDirections)
{
    constexpr NSUInteger kW = 16, kH = 16, kCW = 8, kCH = 8;
    id<MTLTexture> tex = makePitchTexture(kW, kH);
    ASSERT_NE(tex, nil);
    ASSERT_FALSE(((SwitchMetalTexture *)tex)->_raw.blockLinear);

    const std::pair<int, int> shifts[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {2, 3}, {2, -3}, {-2, 3}, {-2, -3}};
    for (const auto &[dx, dy] : shifts) {
        const NSUInteger sx = 4, sy = 4, tx = NSUInteger(int(sx) + dx), ty = NSUInteger(int(sy) + dy);
        auto expected = fillPattern(kW, kH, 4, uint8_t(dx * 5 + dy));
        uploadRect(tex, 0, 0, kW, kH, 4, expected);
        const EngineRun run =
            runCopy(tex, 0, 0, MTLOriginMake(sx, sy, 0), MTLSizeMake(kCW, kCH, 1), tex, 0, 0, MTLOriginMake(tx, ty, 0));
        EXPECT_EQ(run.copies, 3u) << dx << ',' << dy;
        applyCpuMove(expected, kW, 4, sx, sy, tx, ty, kCW, kCH);
        EXPECT_EQ(readbackRect(tex, 0, 0, kW, kH, 4), expected) << dx << ',' << dy;
    }
}

TEST_F(FacadeBlitOverlap, BlockLinearOverlappingShiftsAllEightDirections)
{
    constexpr NSUInteger kW = 32, kH = 32;
    id<MTLTexture> tex = makeTiledTexture(kW, kH);
    ASSERT_NE(tex, nil);
    ASSERT_TRUE(((SwitchMetalTexture *)tex)->_raw.blockLinear);

    const std::pair<int, int> shifts[] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {3, 2}, {3, -2}, {-3, 2}, {-3, -2}};
    for (NSUInteger level : {NSUInteger(0), NSUInteger(1)}) {
        const NSUInteger lw = kW >> level, lh = kH >> level, cw = lw / 2, ch = lh / 2;
        const NSUInteger sx = lw / 4, sy = lh / 4;
        for (const auto &[dx, dy] : shifts) {
            const NSUInteger tx = NSUInteger(int(sx) + dx), ty = NSUInteger(int(sy) + dy);
            auto expected = fillPattern(lw, lh, 4, uint8_t(level * 13 + dx * 3 + dy));
            uploadRect(tex, level, 0, lw, lh, 4, expected);
            const EngineRun run = runCopy(tex, 0, level, MTLOriginMake(sx, sy, 0), MTLSizeMake(cw, ch, 1), tex, 0,
                                          level, MTLOriginMake(tx, ty, 0));
            EXPECT_EQ(run.copies, 3u) << "level=" << level << ' ' << dx << ',' << dy;
            applyCpuMove(expected, lw, 4, sx, sy, tx, ty, cw, ch);
            EXPECT_EQ(readbackRect(tex, level, 0, lw, lh, 4), expected) << "level=" << level << ' ' << dx << ',' << dy;
        }
    }
}

TEST_F(FacadeBlitOverlap, ExactIdentityCopiesAreElided)
{
    id<MTLTexture> pitch = makePitchTexture(16, 16);
    ASSERT_NE(pitch, nil);
    ASSERT_FALSE(((SwitchMetalTexture *)pitch)->_raw.blockLinear);
    auto pitchData = fillPattern(16, 16, 4, 1);
    uploadRect(pitch, 0, 0, 16, 16, 4, pitchData);
    size_t added = 99;
    EngineRun run =
        runCopy(pitch, 0, 0, MTLOriginMake(2, 3, 0), MTLSizeMake(8, 8, 1), pitch, 0, 0, MTLOriginMake(2, 3, 0), &added);
    EXPECT_EQ(added, 0u);
    EXPECT_EQ(run.copies, 0u);
    EXPECT_EQ(readbackRect(pitch, 0, 0, 16, 16, 4), pitchData);

    id<MTLTexture> tiled = makeTiledTexture(32, 32, MTLPixelFormatRGBA8Unorm, true, 2);
    id<MTLTexture> formatView = [tiled newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
    id<MTLTexture> partView = [tiled newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                       textureType:MTLTextureType2D
                                                            levels:NSMakeRange(1, 1)
                                                            slices:NSMakeRange(1, 1)];
    ASSERT_NE(formatView, nil);
    ASSERT_NE(partView, nil);
    auto mipData = fillPattern(16, 16, 4, 2);
    uploadRect(tiled, 1, 1, 16, 16, 4, mipData);
    run = runCopy(tiled, 1, 1, MTLOriginMake(1, 2, 0), MTLSizeMake(10, 10, 1), formatView, 1, 1, MTLOriginMake(1, 2, 0),
                  &added);
    EXPECT_EQ(added, 0u);
    EXPECT_EQ(run.copies, 0u);
    run = runCopy(partView, 0, 0, MTLOriginMake(1, 2, 0), MTLSizeMake(10, 10, 1), tiled, 1, 1, MTLOriginMake(1, 2, 0),
                  &added);
    EXPECT_EQ(added, 0u);
    EXPECT_EQ(run.copies, 0u);
    EXPECT_EQ(readbackRect(tiled, 1, 1, 16, 16, 4), mipData);

    id<MTLBuffer> buf = [gDevice newBufferWithLength:16384 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *bd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:16
                                                                                 height:16
                                                                              mipmapped:NO];
    bd.storageMode = MTLStorageModeShared;
    bd.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> bufTexA = [buf newTextureWithDescriptor:bd offset:0 bytesPerRow:256];
    id<MTLTexture> bufTexB = [buf newTextureWithDescriptor:bd offset:0 bytesPerRow:256];
    ASSERT_NE(bufTexA, nil);
    ASSERT_NE(bufTexB, nil);
    uploadRect(bufTexA, 0, 0, 16, 16, 4, pitchData);
    run = runCopy(bufTexA, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(16, 16, 1), bufTexB, 0, 0, MTLOriginMake(0, 0, 0),
                  &added);
    EXPECT_EQ(added, 0u);
    EXPECT_EQ(run.copies, 0u);
    EXPECT_EQ(readbackRect(bufTexB, 0, 0, 16, 16, 4), pitchData);
}

TEST_F(FacadeBlitOverlap, NonOverlappingSameTextureCopiesStaySinglePass)
{
    id<MTLTexture> pitch = makePitchTexture(16, 16);
    ASSERT_NE(pitch, nil);
    ASSERT_FALSE(((SwitchMetalTexture *)pitch)->_raw.blockLinear);
    auto expected = fillPattern(16, 16, 4, 3);
    uploadRect(pitch, 0, 0, 16, 16, 4, expected);

    const struct {
        NSUInteger sx, sy, dx, dy, w, h;
    } pitchCases[] = {{0, 0, 8, 0, 8, 8}, {0, 0, 0, 8, 8, 8}, {8, 0, 0, 2, 8, 8}};

    for (const auto &c : pitchCases) {
        const EngineRun run = runCopy(pitch, 0, 0, MTLOriginMake(c.sx, c.sy, 0), MTLSizeMake(c.w, c.h, 1), pitch, 0, 0,
                                      MTLOriginMake(c.dx, c.dy, 0));
        EXPECT_EQ(run.copies, 1u);
        applyCpuMove(expected, 16, 4, c.sx, c.sy, c.dx, c.dy, c.w, c.h);
        EXPECT_EQ(readbackRect(pitch, 0, 0, 16, 16, 4), expected);
    }

    id<MTLTexture> tiled = makeTiledTexture(32, 32, MTLPixelFormatRGBA8Unorm, true, 2);
    auto slice0 = fillPattern(32, 32, 4, 4), slice1 = fillPattern(32, 32, 4, 5), mip1 = fillPattern(16, 16, 4, 6);
    uploadRect(tiled, 0, 0, 32, 32, 4, slice0);
    uploadRect(tiled, 0, 1, 32, 32, 4, slice1);
    uploadRect(tiled, 1, 0, 16, 16, 4, mip1);
    EXPECT_EQ(
        runCopy(tiled, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(8, 8, 1), tiled, 0, 0, MTLOriginMake(8, 0, 0)).copies,
        1u);
    applyCpuMove(slice0, 32, 4, 0, 0, 8, 0, 8, 8);
    EXPECT_EQ(readbackRect(tiled, 0, 0, 32, 32, 4), slice0);
    EXPECT_EQ(runCopy(tiled, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(16, 16, 1), tiled, 1, 0, MTLOriginMake(0, 0, 0))
                  .copies,
              1u);
    EXPECT_EQ(runCopy(tiled, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(16, 16, 1), tiled, 0, 1, MTLOriginMake(0, 0, 0))
                  .copies,
              1u);
    auto gotMip1 = readbackRect(tiled, 1, 0, 16, 16, 4);
    for (NSUInteger y = 0; y < 16; ++y)
        EXPECT_EQ(memcmp(gotMip1.data() + y * 16 * 4, slice0.data() + y * 32 * 4, 16 * 4), 0) << y;
}

TEST_F(FacadeBlitOverlap, TextureViewsAndBufferAliasesOverSameBackingOverlap)
{
    id<MTLTexture> parent = makeTiledTexture(32, 32, MTLPixelFormatRGBA8Unorm, true, 2);
    id<MTLTexture> formatView = [parent newTextureViewWithPixelFormat:MTLPixelFormatR32Uint];
    id<MTLTexture> partView = [parent newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                        textureType:MTLTextureType2D
                                                             levels:NSMakeRange(1, 1)
                                                             slices:NSMakeRange(1, 1)];
    ASSERT_NE(formatView, nil);
    ASSERT_NE(partView, nil);

    auto data0 = fillPattern(32, 32, 4, 7);
    uploadRect(parent, 0, 0, 32, 32, 4, data0);
    EngineRun run =
        runCopy(parent, 0, 0, MTLOriginMake(4, 4, 0), MTLSizeMake(12, 12, 1), formatView, 0, 0, MTLOriginMake(6, 7, 0));
    EXPECT_EQ(run.copies, 3u);
    applyCpuMove(data0, 32, 4, 4, 4, 6, 7, 12, 12);
    EXPECT_EQ(readbackRect(parent, 0, 0, 32, 32, 4), data0);

    auto dataMip = fillPattern(16, 16, 4, 8);
    uploadRect(parent, 1, 1, 16, 16, 4, dataMip);
    run = runCopy(partView, 0, 0, MTLOriginMake(2, 2, 0), MTLSizeMake(8, 8, 1), parent, 1, 1, MTLOriginMake(4, 5, 0));
    EXPECT_EQ(run.copies, 3u);
    applyCpuMove(dataMip, 16, 4, 2, 2, 4, 5, 8, 8);
    EXPECT_EQ(readbackRect(parent, 1, 1, 16, 16, 4), dataMip);

    constexpr NSUInteger kPitch = 256;
    id<MTLBuffer> buf = [gDevice newBufferWithLength:kPitch * 32 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *bd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:24
                                                                                 height:16
                                                                              mipmapped:NO];
    bd.storageMode = MTLStorageModeShared;
    bd.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texA = [buf newTextureWithDescriptor:bd offset:0 bytesPerRow:kPitch];
    id<MTLTexture> texB = [buf newTextureWithDescriptor:bd offset:2 * kPitch + 8 * 4 bytesPerRow:kPitch];
    ASSERT_NE(texA, nil);
    ASSERT_NE(texB, nil);
    auto bufExpected = fillPattern(24, 16, 4, 9);
    uploadRect(texA, 0, 0, 24, 16, 4, bufExpected);
    run = runCopy(texA, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(12, 8, 1), texB, 0, 0, MTLOriginMake(0, 0, 0));
    EXPECT_EQ(run.copies, 3u);
    applyCpuMove(bufExpected, 24, 4, 0, 0, 8, 2, 12, 8);
    EXPECT_EQ(readbackRect(texA, 0, 0, 24, 16, 4), bufExpected);
}

TEST_F(FacadeBlitOverlap, CompressedBlockLinearOverlapAndIdentity)
{
    constexpr NSUInteger kW = 32, kH = 32, kBW = 4, kBH = 4, kBpp = 16;
    id<MTLTexture> tex = makeTiledTexture(kW, kH, MTLPixelFormatASTC_4x4_LDR, false);
    ASSERT_NE(tex, nil);
    ASSERT_TRUE(((SwitchMetalTexture *)tex)->_gm20bOk);
    ASSERT_TRUE(((SwitchMetalTexture *)tex)->_raw.blockLinear);

    auto expected = fillPattern(kW / kBW, kH / kBH, kBpp, 10);
    uploadRect(tex, 0, 0, kW, kH, kBpp, expected, kBW, kBH);
    EXPECT_EQ(
        runCopy(tex, 0, 0, MTLOriginMake(4, 4, 0), MTLSizeMake(16, 16, 1), tex, 0, 0, MTLOriginMake(4, 4, 0)).copies,
        0u);
    const EngineRun run =
        runCopy(tex, 0, 0, MTLOriginMake(4, 4, 0), MTLSizeMake(16, 16, 1), tex, 0, 0, MTLOriginMake(8, 8, 0));
    EXPECT_EQ(run.copies, 3u);
    applyCpuMove(expected, kW / kBW, kBpp, 1, 1, 2, 2, 4, 4);
    EXPECT_EQ(readbackRect(tex, 0, 0, kW, kH, kBpp, kBW, kBH), expected);
}

TEST_F(FacadeBlitOverlap, StencilViewOverlapPreservesDepth)
{
    constexpr NSUInteger kW = 16, kH = 16;
    id<MTLTexture> zeta = makeTiledTexture(kW, kH, MTLPixelFormatDepth32Float_Stencil8, false);
    ASSERT_NE(zeta, nil);
    id<MTLTexture> stencil = [zeta newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    ASSERT_NE(stencil, nil);

    std::vector<uint8_t> depthBytes(kW * kH * 4);
    for (NSUInteger i = 0; i < kW * kH; ++i) {
        const float v = float(i + 1) / float(kW * kH);
        memcpy(depthBytes.data() + i * 4, &v, 4);
    }
    auto stencilBytes = fillPattern(kW, kH, 1, 11);
    uploadRect(zeta, 0, 0, kW, kH, 4, depthBytes, 1, 1, MTLBlitOptionDepthFromDepthStencil);
    uploadRect(zeta, 0, 0, kW, kH, 1, stencilBytes, 1, 1, MTLBlitOptionStencilFromDepthStencil);

    const EngineRun run =
        runCopy(stencil, 0, 0, MTLOriginMake(2, 2, 0), MTLSizeMake(8, 8, 1), stencil, 0, 0, MTLOriginMake(4, 5, 0));
    EXPECT_EQ(run.copies, 3u);
    applyCpuMove(stencilBytes, kW, 1, 2, 2, 4, 5, 8, 8);
    EXPECT_EQ(readbackRect(zeta, 0, 0, kW, kH, 1, 1, 1, MTLBlitOptionStencilFromDepthStencil), stencilBytes);
    EXPECT_EQ(readbackRect(zeta, 0, 0, kW, kH, 4, 1, 1, MTLBlitOptionDepthFromDepthStencil), depthBytes);
}

TEST_F(FacadeBlitOverlap, Volume3DOverlappingZShifts)
{
    constexpr NSUInteger kW = 16, kH = 16, kD = 4;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = MTLTextureType3D;
    d.pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.width = kW;
    d.height = kH;
    d.depth = kD;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> vol = [gDevice newTextureWithDescriptor:d];
    ASSERT_NE(vol, nil);

    auto uploadVol = [&](const std::vector<uint8_t> &all) {
        id<MTLBuffer> buf = [gDevice newBufferWithLength:all.size() options:MTLResourceStorageModeShared];
        memcpy(buf.contents, all.data(), all.size());
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromBuffer:buf
                   sourceOffset:0
              sourceBytesPerRow:kW * 4
            sourceBytesPerImage:kW * kH * 4
                     sourceSize:MTLSizeMake(kW, kH, kD)
                      toTexture:vol
               destinationSlice:0
               destinationLevel:0
              destinationOrigin:MTLOriginMake(0, 0, 0)];
        [e endEncoding];
        EngineRun run;
        ASSERT_NO_FATAL_FAILURE(commitOnRaw(cb, run));
    };
    auto readVol = [&]() {
        const NSUInteger total = kW * kH * kD * 4;
        id<MTLBuffer> buf = [gDevice newBufferWithLength:total options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = [gQueue commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:vol
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(kW, kH, kD)
                            toBuffer:buf
                   destinationOffset:0
              destinationBytesPerRow:kW * 4
            destinationBytesPerImage:kW * kH * 4];
        [e endEncoding];
        EngineRun run;
        commitOnRaw(cb, run);
        std::vector<uint8_t> out(total);
        memcpy(out.data(), buf.contents, total);
        return out;
    };
    auto makeVol = [&](uint8_t baseSeed) {
        std::vector<uint8_t> all;
        all.reserve(kW * kH * kD * 4);
        for (NSUInteger z = 0; z < kD; ++z) {
            auto plane = fillPattern(kW, kH, 4, uint8_t(baseSeed + z));
            all.insert(all.end(), plane.begin(), plane.end());
        }
        return all;
    };
    auto moveVol = [&](std::vector<uint8_t> &all, NSUInteger sx, NSUInteger sy, NSUInteger sz, NSUInteger dx,
                       NSUInteger dy, NSUInteger dz, NSUInteger cw, NSUInteger ch, NSUInteger cd) {
        std::vector<uint8_t> snap(cw * ch * cd * 4);
        for (NSUInteger z = 0; z < cd; ++z)
            for (NSUInteger y = 0; y < ch; ++y)
                memcpy(snap.data() + ((z * ch + y) * cw) * 4, all.data() + (((sz + z) * kH + (sy + y)) * kW + sx) * 4,
                       cw * 4);
        for (NSUInteger z = 0; z < cd; ++z)
            for (NSUInteger y = 0; y < ch; ++y)
                memcpy(all.data() + (((dz + z) * kH + (dy + y)) * kW + dx) * 4, snap.data() + ((z * ch + y) * cw) * 4,
                       cw * 4);
    };

    for (const auto &[sx, sy, sz, dx, dy, dz, seed] :
         std::vector<std::tuple<NSUInteger, NSUInteger, NSUInteger, NSUInteger, NSUInteger, NSUInteger, uint8_t>>{
             {2, 2, 0, 2, 2, 1, 20},
             {2, 2, 1, 2, 2, 0, 30},
             {2, 2, 0, 5, 4, 0, 40},
         }) {
        auto expected = makeVol(seed);
        uploadVol(expected);
        runCopy(vol, 0, 0, MTLOriginMake(sx, sy, sz), MTLSizeMake(8, 8, 2), vol, 0, 0, MTLOriginMake(dx, dy, dz));
        moveVol(expected, sx, sy, sz, dx, dy, dz, 8, 8, 2);
        EXPECT_EQ(readVol(), expected) << sz << "->" << dz;
    }
}

TEST_F(FacadeBlitOverlap, BackToBackOverlappingCopiesInSingleBlitEncoder)
{
    constexpr NSUInteger kW = 16, kH = 16, kBytes = kW * kH * 4;
    id<MTLTexture> texA = makePitchTexture(kW, kH);
    id<MTLTexture> texB = makeTiledTexture(kW, kH, MTLPixelFormatRGBA8Unorm, false);
    auto expA = fillPattern(kW, kH, 4, 50), expB = fillPattern(kW, kH, 4, 60);
    id<MTLBuffer> inA = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> inB = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> outA = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> outB = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    memcpy(inA.contents, expA.data(), kBytes);
    memcpy(inB.contents, expB.data(), kBytes);

    id<MTLCommandBuffer> cb = [gQueue commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromBuffer:inA
               sourceOffset:0
          sourceBytesPerRow:kW * 4
        sourceBytesPerImage:kBytes
                 sourceSize:MTLSizeMake(kW, kH, 1)
                  toTexture:texA
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e copyFromBuffer:inB
               sourceOffset:0
          sourceBytesPerRow:kW * 4
        sourceBytesPerImage:kBytes
                 sourceSize:MTLSizeMake(kW, kH, 1)
                  toTexture:texB
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e copyFromTexture:texA
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(1, 1, 0)
               sourceSize:MTLSizeMake(10, 10, 1)
                toTexture:texA
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(3, 4, 0)];
    [e copyFromTexture:texB
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(2, 2, 0)
               sourceSize:MTLSizeMake(10, 10, 1)
                toTexture:texB
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(5, 3, 0)];
    [e copyFromTexture:texA
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(kW, kH, 1)
                        toBuffer:outA
               destinationOffset:0
          destinationBytesPerRow:kW * 4
        destinationBytesPerImage:kBytes];
    [e copyFromTexture:texB
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(kW, kH, 1)
                        toBuffer:outB
               destinationOffset:0
          destinationBytesPerRow:kW * 4
        destinationBytesPerImage:kBytes];
    [e endEncoding];
    EngineRun run;
    ASSERT_NO_FATAL_FAILURE(commitOnRaw(cb, run));

    applyCpuMove(expA, kW, 4, 1, 1, 3, 4, 10, 10);
    applyCpuMove(expB, kW, 4, 2, 2, 5, 3, 10, 10);
    EXPECT_EQ(memcmp(outA.contents, expA.data(), kBytes), 0);
    EXPECT_EQ(memcmp(outB.contents, expB.data(), kBytes), 0);
}

TEST(BlitOverlap, Linear2DArraySliceCopyNotIdentityAndDifferentLayerBytesNotDropped)
{
    gm20b::Texture arr;
    arr.va = 0x100000;
    arr.pitch = 64;
    arr.width = 16;
    arr.height = 8;
    arr.bpp = 4;
    arr.layers = 2;
    arr.layerBytes = 512;
    arr.blockLinear = false;

    const gm20b::CopySurface s0 = gm20b::copySideOf(arr, 2, 1, 0);
    const gm20b::CopySurface s1 = gm20b::copySideOf(arr, 2, 1, 1);
    EXPECT_EQ(s0.va + 512, s1.va);
    EXPECT_EQ(gm20b::copySideOverlap(s0, s1, 4, 4, 4, 4), gm20b::CopyOverlap::None);
    EXPECT_EQ(gm20b::copySideOverlap(s0, s0, 4, 4, 4, 4), gm20b::CopyOverlap::Identity);
}

}  // namespace
