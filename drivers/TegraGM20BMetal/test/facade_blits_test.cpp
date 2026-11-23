// SPDX-License-Identifier: MIT
// Metal's copies between buffers and textures and between textures on the facade, run on the copy engine's model.
#include "facade_blit_engines.h"

namespace {

id<MTLDevice> device;

class FacadeBlits : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        device = harness::open(harness::writePack(harness::packKey(MTLPixelFormatBGRA8Unorm)));
        ASSERT_NE(device, nil);
    }

    static id<MTLTexture> texture(MTLPixelFormat format = MTLPixelFormatRGBA8Unorm, bool array = false)
    {
        auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:32 height:32 mipmapped:YES];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        if (array) {
            d.textureType = MTLTextureType2DArray;
            d.arrayLength = 2;
        }
        return [device newTextureWithDescriptor:d];
    }

    static id<MTLTexture> view(id<MTLTexture> parent)
    {
        return [parent newTextureViewWithPixelFormat:parent.pixelFormat];
    }

    static id<MTLBuffer> buffer()
    {
        return [device newBufferWithLength:32768 options:MTLResourceStorageModeShared];
    }

    static void upload(id<MTLBlitCommandEncoder> e, id<MTLBuffer> b, id<MTLTexture> t, NSUInteger level = 1,
                       NSUInteger slice = 0, NSUInteger offset = 0)
    {
        const NSUInteger w = 32 >> level;
        [e copyFromBuffer:b
                   sourceOffset:offset
              sourceBytesPerRow:w * 4
            sourceBytesPerImage:w * w * 4
                     sourceSize:MTLSizeMake(w, w, 1)
                      toTexture:t
               destinationSlice:slice
               destinationLevel:level
              destinationOrigin:MTLOriginMake(0, 0, 0)];
    }

    static void read(id<MTLBlitCommandEncoder> e, id<MTLTexture> t, id<MTLBuffer> b, NSUInteger level = 1,
                     NSUInteger slice = 0, NSUInteger offset = 0)
    {
        const NSUInteger w = 32 >> level;
        [e copyFromTexture:t
                         sourceSlice:slice
                         sourceLevel:level
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, w, 1)
                            toBuffer:b
                   destinationOffset:offset
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:w * w * 4];
    }

    static void execute(id<MTLCommandBuffer> cb, uint32_t mipSteps = 0, bool empty = false)
    {
        harness::EngineRun run;
        ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
        if (empty)
            EXPECT_EQ(run.copies, 0u);
        else
            EXPECT_GT(run.copies, 0u);
        EXPECT_EQ(run.twod, mipSteps);
    }
};

TEST_F(FacadeBlits, BlitB2TFormatViewMipWritesTheParentMip)
{
    auto parent = texture(), alias = view(parent);
    ASSERT_NE(alias, nil);
    auto in = buffer(), out = buffer();
    memset(in.contents, 0x53, 16 * 16 * 4);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    upload(e, in, alias);
    read(e, parent, out);
    [e endEncoding];
    execute(cb);
    EXPECT_EQ(memcmp(in.contents, out.contents, 16 * 16 * 4), 0);
}

TEST_F(FacadeBlits, BlitT2BFormatViewMipReadsTheParentMip)
{
    auto parent = texture(), alias = view(parent);
    ASSERT_NE(alias, nil);
    auto in = buffer(), out = buffer();
    memset(in.contents, 0x97, 16 * 16 * 4);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    upload(e, in, parent);
    read(e, alias, out);
    [e endEncoding];
    execute(cb);
    EXPECT_EQ(memcmp(in.contents, out.contents, 16 * 16 * 4), 0);
}

TEST_F(FacadeBlits, BlitTexCopiesTheViewMipAtAnOddOrigin)
{
    auto a = texture(), b = texture(), alias = view(b);
    auto in = buffer(), out = buffer(), before = buffer();
    memset(before.contents, 0xa6, 1024);
    auto *bytes = static_cast<uint8_t *>(in.contents);
    for (size_t k = 0; k < 1024; ++k)
        bytes[k] = uint8_t(k * 13 + k / 64);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    upload(e, in, a);
    upload(e, before, b);
    [e copyFromTexture:a
              sourceSlice:0
              sourceLevel:1
             sourceOrigin:MTLOriginMake(3, 5, 0)
               sourceSize:MTLSizeMake(7, 9, 1)
                toTexture:alias
         destinationSlice:0
         destinationLevel:1
        destinationOrigin:MTLOriginMake(1, 2, 0)];
    read(e, b, out);
    [e endEncoding];
    execute(cb);
    auto *got = static_cast<uint8_t *>(out.contents);
    for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x)
            for (unsigned c = 0; c < 4; ++c)
                EXPECT_EQ(got[(y * 16 + x) * 4 + c],
                          x >= 1 && x < 8 && y >= 2 && y < 11 ? bytes[((y + 3) * 16 + x + 2) * 4 + c] : 0xa6)
                    << x << ',' << y << ',' << c;
}

// Texture copies Metal's validation stops (MTLBlitCommandEncoder.h: a level past the texture's, a destination of
// another texel size) copy nothing, on the driver, beside the copies that land
TEST_F(FacadeBlits, TextureCopiesMetalStopsCopyNothing)
{
    auto a = texture(), b = texture(), narrow = texture(MTLPixelFormatR8Unorm);
    ASSERT_TRUE(((SwitchMetalTexture *)a)->_gm20bOk && ((SwitchMetalTexture *)narrow)->_gm20bOk);
    auto in = buffer(), before = buffer(), out = buffer();
    memset(in.contents, 0x3b, 1024);
    memset(before.contents, 0xa6, 1024);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    upload(e, in, a);
    upload(e, before, b);
    const auto copyFromA = [&](NSUInteger level, MTLOrigin origin, MTLSize size, id<MTLTexture> into) {
        [e copyFromTexture:a
                  sourceSlice:0
                  sourceLevel:level
                 sourceOrigin:origin
                   sourceSize:size
                    toTexture:into
             destinationSlice:0
             destinationLevel:1
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    };
    const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
    copyFromA(6, MTLOriginMake(0, 0, 0), MTLSizeMake(1, 1, 1), b);
    copyFromA(1, MTLOriginMake(0, 0, 0), MTLSizeMake(4, 4, 1), narrow);
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded);
    read(e, b, out);
    [e endEncoding];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
    execute(cb);
    EXPECT_EQ(memcmp(before.contents, out.contents, 1024), 0);
}

// Buffer copies Metal's validation stops (rows past the buffer, rows shorter than the copy's, a slice past the
// texture's) copy nothing, on the driver, beside the copies that land
TEST_F(FacadeBlits, BufferCopiesMetalStopsCopyNothing)
{
    auto t = texture();
    auto in = buffer(), junk = buffer(), out = buffer(), untouched = buffer();
    memset(in.contents, 0x3b, 1024);
    memset(junk.contents, 0xee, 32768);
    memset(untouched.contents, 0x5c, 32768);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    upload(e, in, t);
    const auto intoLevel1 = [&](NSUInteger offset, NSUInteger row, MTLSize size) {
        [e copyFromBuffer:junk
                   sourceOffset:offset
              sourceBytesPerRow:row
            sourceBytesPerImage:0
                     sourceSize:size
                      toTexture:t
               destinationSlice:0
               destinationLevel:1
              destinationOrigin:MTLOriginMake(0, 0, 0)];
    };
    const auto fromLevel1 = [&](NSUInteger slice, NSUInteger row) {
        [e copyFromTexture:t
                         sourceSlice:slice
                         sourceLevel:1
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(16, 16, 1)
                            toBuffer:untouched
                   destinationOffset:0
              destinationBytesPerRow:row
            destinationBytesPerImage:0];
    };
    const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
    intoLevel1(32768 - 512, 64, MTLSizeMake(16, 16, 1));
    fromLevel1(0, 32);
    fromLevel1(1, 64);
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded);
    read(e, t, out);
    [e endEncoding];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
    execute(cb);
    EXPECT_EQ(memcmp(in.contents, out.contents, 1024), 0);
    const auto *kept = static_cast<const uint8_t *>(untouched.contents);
    EXPECT_TRUE(std::all_of(kept, kept + 32768, [](uint8_t v) {
        return v == 0x5c;
    }));
}

// A texture copy from a region past its level, which Metal's validation stops, copies the part inside, as an M4 Max
// does (the rest it writes is no texel's: c38-audit/failed-counts/probe pat_region), skipped as call.blit
TEST_F(FacadeBlits, ATextureCopyFromARegionPastItsLevelCopiesThePartInside)
{
    auto a = texture(), b = texture();
    auto in = buffer(), before = buffer(), out = buffer();
    auto *bytes = static_cast<uint8_t *>(in.contents);
    for (NSUInteger i = 0; i < 1024; ++i)
        bytes[i] = uint8_t(i * 7 + 3);
    memset(before.contents, 0xa6, 1024);
    NSDictionary *counted = harness::counters(device);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    upload(e, in, a);
    upload(e, before, b);
    [e copyFromTexture:a
              sourceSlice:0
              sourceLevel:1
             sourceOrigin:MTLOriginMake(10, 0, 0)
               sourceSize:MTLSizeMake(8, 8, 1)
                toTexture:b
         destinationSlice:0
         destinationLevel:1
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    read(e, b, out);
    [e endEncoding];
    execute(cb);
    EXPECT_EQ(harness::skippedMoved(counted, harness::counters(device)), harness::skippedOnce("call.blit"));
    const auto *got = static_cast<const uint8_t *>(out.contents);
    for (NSUInteger y = 0; y < 16; ++y)
        for (NSUInteger x = 0; x < 16; ++x)
            for (NSUInteger c = 0; c < 4; ++c)
                EXPECT_EQ(got[(y * 16 + x) * 4 + c], x < 6 && y < 8 ? bytes[(y * 16 + x + 10) * 4 + c] : 0xa6)
                    << x << ',' << y << ',' << c;
}

// A buffer copy into a region past its level, which Metal's validation stops, writes the part inside the level, as an
// M4 Max writes it (c38-audit/failed-counts/probe buf2tex_region_past), skipped as call.blit
TEST_F(FacadeBlits, ABufferCopyIntoARegionPastItsLevelWritesThePartInside)
{
    auto t = texture();
    auto in = buffer(), junk = buffer(), out = buffer();
    memset(in.contents, 0x3b, 1024);
    memset(junk.contents, 0xee, 32768);
    NSDictionary *before = harness::counters(device);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    upload(e, in, t);
    [e copyFromBuffer:junk
               sourceOffset:0
          sourceBytesPerRow:128
        sourceBytesPerImage:0
                 sourceSize:MTLSizeMake(32, 32, 1)
                  toTexture:t
           destinationSlice:0
           destinationLevel:1
          destinationOrigin:MTLOriginMake(4, 0, 0)];
    read(e, t, out);
    [e endEncoding];
    execute(cb);
    EXPECT_EQ(harness::skippedMoved(before, harness::counters(device)), harness::skippedOnce("call.blit"));
    const auto *got = static_cast<const uint8_t *>(out.contents);
    for (NSUInteger y = 0; y < 16; ++y)
        for (NSUInteger x = 0; x < 16; ++x)
            EXPECT_EQ(got[(y * 16 + x) * 4], x >= 4 ? 0xee : 0x3b) << x << ',' << y;
}

TEST_F(FacadeBlits, BlitWholeCopiesAllViewLevels)
{
    auto a = texture(), b = texture(), alias = view(b);
    auto in = buffer(), out = buffer();
    auto *bytes = static_cast<uint8_t *>(in.contents);
    for (NSUInteger l = 0; l < 6; ++l)
        memset(bytes + l * 4096, int(0x31 + l), 4096);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    for (NSUInteger l = 0; l < 6; ++l) {
        const NSUInteger w = 32 >> l;
        [e copyFromBuffer:in
                   sourceOffset:l * 4096
              sourceBytesPerRow:w * 4
            sourceBytesPerImage:w * w * 4
                     sourceSize:MTLSizeMake(w, w, 1)
                      toTexture:a
               destinationSlice:0
               destinationLevel:l
              destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    [e copyFromTexture:a toTexture:alias];
    for (NSUInteger l = 0; l < 6; ++l) {
        const NSUInteger w = 32 >> l;
        [e copyFromTexture:b
                         sourceSlice:0
                         sourceLevel:l
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, w, 1)
                            toBuffer:out
                   destinationOffset:l * 4096
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:w * w * 4];
    }
    [e endEncoding];
    execute(cb);
    for (NSUInteger l = 0; l < 6; ++l) {
        const NSUInteger w = 32 >> l;
        EXPECT_EQ(memcmp(bytes + l * 4096, static_cast<uint8_t *>(out.contents) + l * 4096, w * w * 4), 0) << l;
    }
}

TEST_F(FacadeBlits, NestedViewsKeepTheParentMipAndArraySlice)
{
    auto parent = texture(MTLPixelFormatRGBA8Unorm, true);
    auto first = [parent newTextureViewWithPixelFormat:parent.pixelFormat
                                           textureType:MTLTextureType2DArray
                                                levels:NSMakeRange(1, 4)
                                                slices:NSMakeRange(1, 1)];
    ASSERT_NE(first, nil);
    auto nested = [first newTextureViewWithPixelFormat:first.pixelFormat
                                           textureType:MTLTextureType2D
                                                levels:NSMakeRange(1, 2)
                                                slices:NSMakeRange(0, 1)];
    ASSERT_NE(nested, nil);
    auto alias = view(nested);
    ASSERT_TRUE(((SwitchMetalTexture *)alias)->_gm20bOk);
    auto in = buffer(), out = buffer();
    memset(in.contents, 0x71, 128);
    memset(static_cast<uint8_t *>(in.contents) + 64, 0xd2, 64);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    [e copyFromBuffer:in
               sourceOffset:0
          sourceBytesPerRow:16
        sourceBytesPerImage:64
                 sourceSize:MTLSizeMake(4, 4, 1)
                  toTexture:parent
           destinationSlice:0
           destinationLevel:3
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e copyFromBuffer:in
               sourceOffset:64
          sourceBytesPerRow:16
        sourceBytesPerImage:64
                 sourceSize:MTLSizeMake(4, 4, 1)
                  toTexture:alias
           destinationSlice:0
           destinationLevel:1
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    for (NSUInteger slice = 0; slice < 2; ++slice)
        [e copyFromTexture:parent
                         sourceSlice:slice
                         sourceLevel:3
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(4, 4, 1)
                            toBuffer:out
                   destinationOffset:slice * 64
              destinationBytesPerRow:16
            destinationBytesPerImage:64];
    [e copyFromTexture:alias
                     sourceSlice:0
                     sourceLevel:1
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(4, 4, 1)
                        toBuffer:out
               destinationOffset:128
          destinationBytesPerRow:16
        destinationBytesPerImage:64];
    [e endEncoding];
    execute(cb);
    EXPECT_EQ(memcmp(in.contents, out.contents, 128), 0);
    EXPECT_EQ(memcmp(static_cast<uint8_t *>(in.contents) + 64, static_cast<uint8_t *>(out.contents) + 128, 64), 0);
}

TEST_F(FacadeBlits, X32StencilBufferCopiesKeepDepthAndNeighborSubresources)
{
    for (bool selected : {false, true}) {
        SCOPED_TRACE(selected);
        auto parent = texture(MTLPixelFormatDepth32Float_Stencil8, true);
        ASSERT_NE(parent, nil);
        id<MTLTexture> part = parent;
        if (selected)
            part = [parent newTextureViewWithPixelFormat:parent.pixelFormat
                                             textureType:MTLTextureType2D
                                                  levels:NSMakeRange(1, 3)
                                                  slices:NSMakeRange(1, 1)];
        ASSERT_NE(part, nil);
        auto stencil = [part newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        ASSERT_NE(stencil, nil);
        ASSERT_TRUE(((SwitchMetalTexture *)stencil)->_gm20bOk);
        const auto &root = ((SwitchMetalTexture *)parent)->_raw;
        ASSERT_EQ(root.stencilTemp, nullptr);
        const auto storage = root.storageRange();
        auto *bytes = kernel_model::model.host(storage.va, storage.bytes);
        ASSERT_NE(bytes, nullptr);
        std::vector<uint8_t> expected(storage.bytes);
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = uint8_t(i * 17 + i / 31);
        memcpy(bytes, expected.data(), expected.size());
        gm20b::Texture level;
        ASSERT_TRUE(gm20b::chainLevelImage(root, 2, level));
        const uint64_t base = level.va + level.layerBytes - storage.va;
        auto in = buffer(), out = buffer();
        std::vector<uint8_t> source(in.length, 0xb6), output(out.length, 0xc7);
        constexpr uint32_t width = 5, height = 3, x0 = 1, y0 = 2, inOffset = 3, inPitch = 11, outOffset = 5,
                           outPitch = 13;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const uint8_t value = uint8_t(0x30 + y * 11 + x * 7);
                source[inOffset + y * inPitch + x] = value;
                output[outOffset + y * outPitch + x] = value;
                const uint64_t at = base + tgm_layout_offset_of(level.pitch, level.blockHeight, (x0 + x) * 8, y0 + y);
                ASSERT_LT(at + 7, storage.bytes);
                expected[at + 4] = value;
            }
        memcpy(in.contents, source.data(), source.size());
        memset(out.contents, 0xc7, out.length);
        auto cb = [[device newCommandQueue] commandBuffer];
        auto e = [cb blitCommandEncoder];
        [e copyFromBuffer:in
                   sourceOffset:inOffset
              sourceBytesPerRow:inPitch
            sourceBytesPerImage:inPitch * height
                     sourceSize:MTLSizeMake(width, height, 1)
                      toTexture:stencil
               destinationSlice:selected ? 0 : 1
               destinationLevel:selected ? 1 : 2
              destinationOrigin:MTLOriginMake(x0, y0, 0)];
        [e copyFromTexture:stencil
                         sourceSlice:selected ? 0 : 1
                         sourceLevel:selected ? 1 : 2
                        sourceOrigin:MTLOriginMake(x0, y0, 0)
                          sourceSize:MTLSizeMake(width, height, 1)
                            toBuffer:out
                   destinationOffset:outOffset
              destinationBytesPerRow:outPitch
            destinationBytesPerImage:outPitch * height];
        [e endEncoding];
        execute(cb);
        EXPECT_EQ(memcmp(in.contents, source.data(), source.size()), 0);
        EXPECT_EQ(memcmp(out.contents, output.data(), output.size()), 0);
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                const uint64_t at = base + tgm_layout_offset_of(level.pitch, level.blockHeight, (x0 + x) * 8, y0 + y);
                // Metal exposes depth and stencil; bytes 5–7 are undefined padding.
                for (uint32_t pad = 5; pad < 8; ++pad)
                    expected[at + pad] = bytes[at + pad];
            }
        EXPECT_EQ(memcmp(bytes, expected.data(), expected.size()), 0);
    }
}

TEST_F(FacadeBlits, StencilCopyTemporaryLivesWithTheRootAcrossViews)
{
    auto out = buffer();
    gm20b::Texture unrelated;
    uint64_t temporary = 0;
    __weak SwitchMetalTexture *held = nil;
    @autoreleasepool {
        id<MTLTexture> parent = texture(MTLPixelFormatDepth32Float_Stencil8);
        ASSERT_NE(parent, nil);
        id<MTLTexture> first = [parent newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        id<MTLTexture> second = [parent newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        ASSERT_NE(first, nil);
        ASSERT_NE(second, nil);
        __unsafe_unretained SwitchMetalTexture *root = (SwitchMetalTexture *)parent;
        held = root;
        const auto storage = root->_raw.storageRange();
        auto *bytes = kernel_model::model.host(storage.va, storage.bytes);
        ASSERT_NE(bytes, nullptr);
        memset(bytes, 0x5a, storage.bytes);
        id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
        [e copyFromTexture:first
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(2, 3, 0)
                          sourceSize:MTLSizeMake(3, 2, 1)
                            toBuffer:out
                   destinationOffset:3
              destinationBytesPerRow:11
            destinationBytesPerImage:22
                             options:MTLBlitOptionStencilFromDepthStencil];
        [e endEncoding];
        execute(cb);
        ASSERT_NE(root->_raw.stencilTemp, nullptr);
        EXPECT_EQ(((SwitchMetalTexture *)first)->_raw.stencilTemp, nullptr);
        EXPECT_EQ(((SwitchMetalTexture *)second)->_raw.stencilTemp, nullptr);
        temporary = root->_raw.stencilTemp->va;
        ASSERT_TRUE(gRM->raw->createTexture(32, 32, 30, unrelated));
        EXPECT_NE(unrelated.va, temporary);
        e = nil;
        cb = nil;
        first = nil;
        parent = nil;
        {
            SwitchMetalTexture *retained = held;
            ASSERT_NE(retained, nil);
            EXPECT_EQ(retained->_raw.stencilTemp->va, temporary);
        }
        memset(out.contents, 0xc7, out.length);
        cb = [[device newCommandQueue] commandBuffer];
        e = [cb blitCommandEncoder];
        [e copyFromTexture:second
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(2, 3, 0)
                          sourceSize:MTLSizeMake(3, 2, 1)
                            toBuffer:out
                   destinationOffset:3
              destinationBytesPerRow:11
            destinationBytesPerImage:22];
        [e endEncoding];
        execute(cb);
        for (NSUInteger i = 0; i < out.length; ++i)
            EXPECT_EQ(static_cast<uint8_t *>(out.contents)[i],
                      i >= 3 && (i - 3) / 11 < 2 && (i - 3) % 11 < 3 ? 0x5a : 0xc7)
                << i;
        e = nil;
        cb = nil;
        second = nil;
    }
    EXPECT_EQ(held, nil);
    gm20b::Texture reused;
    ASSERT_TRUE(gRM->raw->createTexture(32, 32, 30, reused));
    EXPECT_EQ(reused.va, temporary);
    gRM->raw->destroyTexture(reused);
    gRM->raw->destroyTexture(unrelated);
}

}  // namespace
