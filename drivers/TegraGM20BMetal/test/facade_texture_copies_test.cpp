// SPDX-License-Identifier: MIT
// Metal's copies between textures as the facade records them (facade_harness.h), run on the copy engine's model:
// Metal's copy units (a stencil view's stencil with a texture of bytes, a combined depth/stencil format into its own
// alone), plain Metal's blit options, and the whole-texture copy's levels
#include "facade_blit_engines.h"
#include "../TegraGM20BMetalInternal.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {

using namespace harness;

id<MTLDevice> gDevice;

class FacadeTextureCopies : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
        ASSERT_TRUE(gDevice != nil);
    }
};

// A stencil view's stencil copies to and from a texture of one-byte texels, Metal's copy unit on both sides (m2v
// blit_plan.rs texture_unit, probes on an M3 Max and an M4)
TEST_F(FacadeTextureCopies, AStencilViewCopiesItsStencilWithATextureOfBytes)
{
    constexpr NSUInteger side = 4;
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                           width:side
                                                          height:side
                                                       mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
    id<MTLTexture> zeta = [gDevice newTextureWithDescriptor:d];
    id<MTLTexture> stencil = [zeta newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    d.pixelFormat = MTLPixelFormatR8Uint;
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> bytes = [gDevice newTextureWithDescriptor:d];
    ASSERT_TRUE(stencil != nil && bytes != nil);
    ASSERT_TRUE(((SwitchMetalTexture *)stencil)->_gm20bOk && ((SwitchMetalTexture *)bytes)->_gm20bOk);
    id<MTLBuffer> in = [gDevice newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [gDevice newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    auto *source = static_cast<uint8_t *>(in.contents);
    for (NSUInteger i = 0; i < side * side; ++i)
        source[i] = uint8_t(0x21 + 7 * i);
    for (NSUInteger i = 0; i < side * side; ++i)
        source[64 + i] = uint8_t(0x90 + 3 * i);
    auto copy = [&](id<MTLBlitCommandEncoder> e, id<MTLTexture> from, id<MTLTexture> to) {
        [e copyFromTexture:from
                  sourceSlice:0
                  sourceLevel:0
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(side, side, 1)
                    toTexture:to
             destinationSlice:0
             destinationLevel:0
            destinationOrigin:MTLOriginMake(0, 0, 0)];
    };
    auto rows = [&](id<MTLBlitCommandEncoder> e, id<MTLTexture> t, NSUInteger at, bool into) {
        if (into)
            [e copyFromBuffer:in
                       sourceOffset:at
                  sourceBytesPerRow:side
                sourceBytesPerImage:side * side
                         sourceSize:MTLSizeMake(side, side, 1)
                          toTexture:t
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        else
            [e copyFromTexture:t
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(side, side, 1)
                                toBuffer:out
                       destinationOffset:at
                  destinationBytesPerRow:side
                destinationBytesPerImage:side * side];
    };
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    rows(e, stencil, 0, true);
    copy(e, stencil, bytes);
    rows(e, bytes, 0, false);
    rows(e, bytes, 64, true);
    copy(e, bytes, stencil);
    rows(e, stencil, 64, false);
    [e endEncoding];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    EXPECT_EQ(memcmp(out.contents, in.contents, side * side), 0) << "the stencil into the bytes";
    EXPECT_EQ(memcmp(static_cast<uint8_t *>(out.contents) + 64, source + 64, side * side), 0) << "and back";
}

// Depth32Float_Stencil8 copies into its own format alone (m2v blit_plan.rs): a copy into RG32Float, of the same texel
// size, records nothing and completes on raw
TEST_F(FacadeTextureCopies, ACombinedDepthStencilCopiesIntoNoOtherFormat)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                           width:4
                                                          height:4
                                                       mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> zeta = [gDevice newTextureWithDescriptor:d];
    d.pixelFormat = MTLPixelFormatRG32Float;
    id<MTLTexture> colour = [gDevice newTextureWithDescriptor:d];
    ASSERT_TRUE(zeta != nil && colour != nil);
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
    [e copyFromTexture:zeta
              sourceSlice:0
              sourceLevel:0
             sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(4, 4, 1)
                toTexture:colour
         destinationSlice:0
         destinationLevel:0
        destinationOrigin:MTLOriginMake(0, 0, 0)];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded);
    [e endEncoding];
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    EXPECT_EQ(run.copies, 0u);
}

// Plain Metal's blit options (m2v copy_plan.rs, EV SPCrp-probe1-m4): Depth32Float_Stencil8 with no option copies its depth
TEST_F(FacadeTextureCopies, ACombinedDepthStencilsCopyWithNoOptionMovesItsDepth)
{
    constexpr NSUInteger side = 4;
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
                                                           width:side
                                                          height:side
                                                       mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    id<MTLTexture> zeta = [gDevice newTextureWithDescriptor:d];
    ASSERT_TRUE(zeta != nil && ((SwitchMetalTexture *)zeta)->_gm20bOk);
    id<MTLBuffer> in = [gDevice newBufferWithLength:side * side * 4 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [gDevice newBufferWithLength:side * side * 4 options:MTLResourceStorageModeShared];
    auto *depth = static_cast<float *>(in.contents);
    for (NSUInteger i = 0; i < side * side; ++i)
        depth[i] = float(i) / 16.0f;
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromBuffer:in
               sourceOffset:0
          sourceBytesPerRow:side * 4
        sourceBytesPerImage:side * side * 4
                 sourceSize:MTLSizeMake(side, side, 1)
                  toTexture:zeta
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e copyFromTexture:zeta
                     sourceSlice:0
                     sourceLevel:0
                    sourceOrigin:MTLOriginMake(0, 0, 0)
                      sourceSize:MTLSizeMake(side, side, 1)
                        toBuffer:out
               destinationOffset:0
          destinationBytesPerRow:side * 4
        destinationBytesPerImage:side * side * 4
                         options:MTLBlitOptionDepthFromDepthStencil];
    [e endEncoding];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    EXPECT_EQ(memcmp(in.contents, out.contents, side * side * 4), 0);
}

// MTLBlitCommandEncoder.h's whole-texture copy: every level of a size both textures have, whatever their types; where
// neither has a level of the other's first level's size, which an M4 Max's driver aborts on
// (c38-audit/failed-counts/probe, tex2tex_whole_nolevel), the command buffer fails InvalidResource as it is recorded
TEST_F(FacadeTextureCopies, AWholeTextureCopyTakesTheLevelsOfOneSize)
{
    auto make = [&](MTLTextureType type, NSUInteger side, bool mipmapped) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                     width:side
                                                                                    height:side
                                                                                 mipmapped:mipmapped];
        d.textureType = type;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead;
        return [gDevice newTextureWithDescriptor:d];
    };
    id<MTLTexture> array = make(MTLTextureType2DArray, 16, true), flat = make(MTLTextureType2D, 16, true),
                   odd = make(MTLTextureType2D, 24, false);
    ASSERT_TRUE(array != nil && flat != nil && odd != nil);
    id<MTLBuffer> in = [gDevice newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> out = [gDevice newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    auto *bytes = static_cast<uint8_t *>(in.contents);
    for (NSUInteger i = 0; i < 4096; ++i)
        bytes[i] = uint8_t(i * 13 + i / 64);
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    for (NSUInteger level = 0; level < 2; ++level) {
        const NSUInteger w = 16 >> level;
        [e copyFromBuffer:in
                   sourceOffset:level * 1024
              sourceBytesPerRow:w * 4
            sourceBytesPerImage:w * w * 4
                     sourceSize:MTLSizeMake(w, w, 1)
                      toTexture:array
               destinationSlice:0
               destinationLevel:level
              destinationOrigin:MTLOriginMake(0, 0, 0)];
    }
    [e copyFromTexture:array toTexture:flat];
    for (NSUInteger level = 0; level < 2; ++level) {
        const NSUInteger w = 16 >> level;
        [e copyFromTexture:flat
                         sourceSlice:0
                         sourceLevel:level
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, w, 1)
                            toBuffer:out
                   destinationOffset:level * 1024
              destinationBytesPerRow:w * 4
            destinationBytesPerImage:w * w * 4];
    }
    [e endEncoding];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    for (NSUInteger level = 0; level < 2; ++level) {
        const NSUInteger w = 16 >> level;
        EXPECT_EQ(memcmp(bytes + level * 1024, static_cast<uint8_t *>(out.contents) + level * 1024, w * w * 4), 0)
            << level;
    }
    id<MTLCommandBuffer> none = [[gDevice newCommandQueue] commandBuffer];
    e = [none blitCommandEncoder];
    const size_t recorded = ((SwitchMetalCommandBuffer *)none)->_plan.size();
    [e copyFromTexture:array toTexture:odd];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)none)->_plan.size(), recorded) << "no level of 24 x 24 texels";
    [e endEncoding];
    [none commit];
    [none waitUntilCompleted];
    EXPECT_EQ(none.status, MTLCommandBufferStatusError);
    EXPECT_EQ(none.error.code, MTLCommandBufferErrorInvalidResource);
    EXPECT_EQ(gm20bPath(none), "error:call.blit");
}

// A no-copy block texture off its layout's alignment is a pool image (Texture::callerPages), and a blit into it goes on
// to the caller's memory through the image's kind-0 view: that memory then holds the image's bytes as they are. Read
// through lines the blit's 0xfe writes held, the copy took them in the other GOB order: gpu.astc.layouts blitin_back,
// 21504 blocks wrong with the map g5=w5^w6^1, g6=w5^w7.
TEST_F(FacadeTextureCopies, ABlitIntoAPoolImageReachesTheCallersMemoryAsTheImageHoldsIt)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatASTC_4x4_LDR
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    id layout = ((id (*)(id, SEL, id, BOOL))objc_msgSend)(
        gDevice, sel_registerName("newTextureLayoutWithDescriptor:isHeapOrBufferBacked:"), d, NO);
    ASSERT_TRUE(layout != nil);
    const NSUInteger length =
        std::max<NSUInteger>(((NSUInteger (*)(id, SEL))objc_msgSend)(layout, sel_registerName("size")), 16384);
    constexpr NSUInteger kOffAlignment = 512;
    void *pages = nullptr;
    ASSERT_EQ(posix_memalign(&pages, 16384, (kOffAlignment + length + 16383) / 16384 * 16384), 0);
    auto *at = static_cast<uint8_t *>(pages) + kOffAlignment;
    memset(at, 0, length);
    id<MTLTexture> texture = ((id (*)(id, SEL, void *, NSUInteger, id, id))objc_msgSend)(
        gDevice, sel_registerName("newTextureWithBytesNoCopy:length:descriptor:deallocator:"), at, length, d, nil);
    ASSERT_TRUE(texture != nil);
    const gm20b::Texture &image = ((SwitchMetalTexture *)texture)->_raw;
    ASSERT_TRUE(image.callerPages != nullptr) << "a base off its alignment: a pool image";
    constexpr NSUInteger kRowBytes = 16 * 16;
    id<MTLBuffer> staging = [gDevice newBufferWithLength:kRowBytes * 16 options:MTLResourceStorageModeShared];
    auto *blocks = static_cast<uint8_t *>(staging.contents);
    for (NSUInteger i = 0; i < kRowBytes * 16; ++i)
        blocks[i] = uint8_t(i * 7 + (i >> 8) * 61 + 1);
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> e = [cb blitCommandEncoder];
    [e copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:kRowBytes
        sourceBytesPerImage:0
                 sourceSize:MTLSizeMake(64, 64, 1)
                  toTexture:texture
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e endEncoding];
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run, true));
    const uint8_t *texels = kernel_model::model.host(image.imagePitchVa, image.storage.bytes);
    ASSERT_NE(texels, nullptr);
    size_t wrong = 0, written = 0;
    for (uint64_t i = 0; i < image.storage.bytes; ++i) {
        wrong += at[i] != texels[i];
        written += texels[i] != 0;
    }
    EXPECT_GT(written, 0u) << "the blit wrote the image";
    EXPECT_EQ(wrong, 0u) << "bytes of the caller's memory that are not the image's";
}

// A placement heap texture (kind 0xfe) and an aliased buffer (kind 0) at offset 0: writing the texture fills the L2
// with 0xfe GOBs; a later non-host-written command buffer filling the aliased buffer must invalidate the L2 first, or
// the pitch writes land in FermiColor order and read back scrambled.
TEST_F(FacadeTextureCopies, APlacementHeapAliasedBufferAfterTextureInvalidatesL2AcrossCommandBuffers)
{
    constexpr NSUInteger kSide = 64, kBytes = kSide * kSide * 4, kStripe = 32;
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.storageMode = MTLStorageModePrivate;
    hd.size = 1 << 20;
    id<MTLHeap> heap = [gDevice newHeapWithDescriptor:hd];
    ASSERT_TRUE(heap != nil);

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:kSide
                                                                                 height:kSide
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [heap newTextureWithDescriptor:td offset:0];
    id<MTLBuffer> aliased = [heap newBufferWithLength:kBytes options:MTLResourceStorageModePrivate offset:0];
    ASSERT_TRUE(texture != nil && aliased != nil);

    id<MTLBuffer> staging = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> readback = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    ASSERT_TRUE(staging != nil && readback != nil);
    auto *src = static_cast<uint8_t *>(staging.contents);
    for (NSUInteger i = 0; i < kBytes; ++i)
        src[i] = uint8_t(i * 13 + (i >> 8) * 37 + 1);
    memset(readback.contents, 0, kBytes);

    id<MTLCommandQueue> queue = [gDevice newCommandQueue];
    const size_t first = kernel_model::model.jobs.size();

    id<MTLCommandBuffer> cb1 = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e1 = [cb1 blitCommandEncoder];
    [e1 copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:kSide * 4
        sourceBytesPerImage:kBytes
                 sourceSize:MTLSizeMake(kSide, kSide, 1)
                  toTexture:texture
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e1 endEncoding];
    [cb1 commit];
    [cb1 waitUntilCompleted];
    ASSERT_EQ(cb1.status, MTLCommandBufferStatusCompleted);

    std::vector<uint8_t> expected(kBytes);
    id<MTLCommandBuffer> cb2 = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e2 = [cb2 blitCommandEncoder];
    for (NSUInteger off = 0; off < kBytes; off += kStripe) {
        const uint8_t v = uint8_t(0x11 + ((off / kStripe) * 7 + (off / 256) * 29) % 223);
        memset(expected.data() + off, v, kStripe);
        [e2 fillBuffer:aliased range:NSMakeRange(off, kStripe) value:v];
    }
    [e2 endEncoding];
    [cb2 commit];
    [cb2 waitUntilCompleted];
    ASSERT_EQ(cb2.status, MTLCommandBufferStatusCompleted);

    id<MTLCommandBuffer> cb3 = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e3 = [cb3 blitCommandEncoder];
    [e3 copyFromBuffer:aliased sourceOffset:0 toBuffer:readback destinationOffset:0 size:kBytes];
    [e3 endEncoding];
    [cb3 commit];
    [cb3 waitUntilCompleted];
    ASSERT_EQ(cb3.status, MTLCommandBufferStatusCompleted);

    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::runEngines(first, run, true));
    const auto *got = static_cast<const uint8_t *>(readback.contents);
    size_t wrong = 0;
    for (NSUInteger i = 0; i < kBytes; ++i)
        wrong += got[i] != expected[i];
    EXPECT_EQ(wrong, 0u) << "aliased buffer bytes scrambled across PTE kinds";
}

// Same placement-heap aliasing within a single command buffer, ordered by an MTLFence between encoders: the driver
// emits an in-stream kind switch before the aliased buffer's first command.
TEST_F(FacadeTextureCopies, APlacementHeapAliasedBufferAfterTextureSwitchesKindsWithinOneCommandBuffer)
{
    constexpr NSUInteger kSide = 64, kBytes = kSide * kSide * 4, kStripe = 32;
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.storageMode = MTLStorageModePrivate;
    hd.size = 1 << 20;
    id<MTLHeap> heap = [gDevice newHeapWithDescriptor:hd];
    ASSERT_TRUE(heap != nil);

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:kSide
                                                                                 height:kSide
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [heap newTextureWithDescriptor:td offset:0];
    id<MTLBuffer> aliased = [heap newBufferWithLength:kBytes options:MTLResourceStorageModePrivate offset:0];
    id<MTLFence> fence = [gDevice newFence];
    ASSERT_TRUE(texture != nil && aliased != nil && fence != nil);

    id<MTLBuffer> staging = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> readback = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    ASSERT_TRUE(staging != nil && readback != nil);
    auto *src = static_cast<uint8_t *>(staging.contents);
    for (NSUInteger i = 0; i < kBytes; ++i)
        src[i] = uint8_t(i * 11 + (i >> 8) * 41 + 3);
    memset(readback.contents, 0, kBytes);

    std::vector<uint8_t> expected(kBytes);
    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandQueue> queue = [gDevice newCommandQueue];
    id<MTLCommandBuffer> cb1 = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e1 = [cb1 blitCommandEncoder];
    [e1 copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:kSide * 4
        sourceBytesPerImage:kBytes
                 sourceSize:MTLSizeMake(kSide, kSide, 1)
                  toTexture:texture
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e1 updateFence:fence];
    [e1 endEncoding];

    id<MTLBlitCommandEncoder> e2 = [cb1 blitCommandEncoder];
    [e2 waitForFence:fence];
    for (NSUInteger off = 0; off < kBytes; off += kStripe) {
        const uint8_t v = uint8_t(0x21 + ((off / kStripe) * 5 + (off / 256) * 19) % 211);
        memset(expected.data() + off, v, kStripe);
        [e2 fillBuffer:aliased range:NSMakeRange(off, kStripe) value:v];
    }
    [e2 endEncoding];
    [cb1 commit];
    [cb1 waitUntilCompleted];
    ASSERT_EQ(cb1.status, MTLCommandBufferStatusCompleted);
    const size_t afterCb1 = kernel_model::model.jobs.size();

    id<MTLCommandBuffer> cb2 = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e3 = [cb2 blitCommandEncoder];
    [e3 copyFromBuffer:aliased sourceOffset:0 toBuffer:readback destinationOffset:0 size:kBytes];
    [e3 endEncoding];
    [cb2 commit];
    [cb2 waitUntilCompleted];
    ASSERT_EQ(cb2.status, MTLCommandBufferStatusCompleted);

    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::runEngines(first, run, true));
    const auto *got = static_cast<const uint8_t *>(readback.contents);
    size_t wrong = 0;
    for (NSUInteger i = 0; i < kBytes; ++i)
        wrong += got[i] != expected[i];
    EXPECT_EQ(wrong, 0u) << "same-CB aliased buffer bytes scrambled across PTE kinds";

    uint32_t kindSwitches = 0;
    for (size_t j = first; j < afterCb1; ++j) {
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        ASSERT_GE(count, 0);
        for (long k = 0; k < count; ++k)
            if (methods[k].method == 0x0034 && methods[k].value == (0x0eu << 27))
                ++kindSwitches;
    }
    EXPECT_GE(kindSwitches, 1u) << "same-CB kind switch must emit L2_SYSMEM_INVALIDATE";
}

// Side-by-side non-overlapping texture (0xfe) and buffer (0) in the same placement heap never cross kinds: filling the
// buffer in a separate command buffer stays non-host-written and emits no kind switch.
TEST_F(FacadeTextureCopies, NonOverlappingPlacementHeapKindsIncurNoL2Invalidate)
{
    constexpr NSUInteger kSide = 64, kBytes = kSide * kSide * 4;
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.storageMode = MTLStorageModePrivate;
    hd.size = 1 << 20;
    id<MTLHeap> heap = [gDevice newHeapWithDescriptor:hd];
    ASSERT_TRUE(heap != nil);

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:kSide
                                                                                 height:kSide
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead;
    const MTLSizeAndAlign texNeed = [gDevice heapTextureSizeAndAlignWithDescriptor:td];
    const NSUInteger bufOffset = (texNeed.size + 511) & ~NSUInteger(511);
    id<MTLTexture> texture = [heap newTextureWithDescriptor:td offset:0];
    id<MTLBuffer> sideBySide = [heap newBufferWithLength:kBytes options:MTLResourceStorageModePrivate offset:bufOffset];
    ASSERT_TRUE(texture != nil && sideBySide != nil);

    id<MTLBuffer> staging = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    ASSERT_TRUE(staging != nil);
    id<MTLCommandQueue> queue = [gDevice newCommandQueue];

    id<MTLCommandBuffer> cb1 = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e1 = [cb1 blitCommandEncoder];
    [e1 copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:kSide * 4
        sourceBytesPerImage:kBytes
                 sourceSize:MTLSizeMake(kSide, kSide, 1)
                  toTexture:texture
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e1 endEncoding];
    [cb1 commit];
    [cb1 waitUntilCompleted];
    ASSERT_EQ(cb1.status, MTLCommandBufferStatusCompleted);

    const size_t beforeCb2 = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb2 = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e2 = [cb2 blitCommandEncoder];
    [e2 fillBuffer:sideBySide range:NSMakeRange(0, kBytes) value:0x5a];
    [e2 endEncoding];
    [cb2 commit];
    [cb2 waitUntilCompleted];
    ASSERT_EQ(cb2.status, MTLCommandBufferStatusCompleted);

    ASSERT_GT(kernel_model::model.jobs.size(), beforeCb2);
    for (size_t j = beforeCb2; j < kernel_model::model.jobs.size(); ++j) {
        EXPECT_EQ(kernel_model::model.jobs[j].flags & TegraGM20BJobHostWritten, 0u);
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        ASSERT_GE(count, 0);
        for (long k = 0; k < count; ++k)
            EXPECT_FALSE(methods[k].method == 0x0034 && methods[k].value == (0x0eu << 27));
    }
}

// First CB uses a texture while the arena is still single-kind; later two same-kind aliased buffers are placed on top
// of it and the next CB uses only the second buffer (whose placement did not itself cross). L2 must still invalidate.
TEST_F(FacadeTextureCopies, ASecondSameKindAliasAfterFirstCrossStillInvalidatesL2)
{
    constexpr NSUInteger kSide = 64, kBytes = kSide * kSide * 4, kStripe = 32;
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.storageMode = MTLStorageModePrivate;
    hd.size = 1 << 20;
    id<MTLHeap> heap = [gDevice newHeapWithDescriptor:hd];
    ASSERT_TRUE(heap != nil);

    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:kSide
                                                                                 height:kSide
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> texture = [heap newTextureWithDescriptor:td offset:0];
    ASSERT_TRUE(texture != nil);

    id<MTLBuffer> staging = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    id<MTLBuffer> readback = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    ASSERT_TRUE(staging != nil && readback != nil);
    auto *src = static_cast<uint8_t *>(staging.contents);
    for (NSUInteger i = 0; i < kBytes; ++i)
        src[i] = uint8_t(i * 17 + (i >> 8) * 43 + 5);
    memset(readback.contents, 0, kBytes);

    id<MTLCommandQueue> queue = [gDevice newCommandQueue];
    const size_t first = kernel_model::model.jobs.size();

    id<MTLCommandBuffer> cb1 = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e1 = [cb1 blitCommandEncoder];
    [e1 copyFromBuffer:staging
               sourceOffset:0
          sourceBytesPerRow:kSide * 4
        sourceBytesPerImage:kBytes
                 sourceSize:MTLSizeMake(kSide, kSide, 1)
                  toTexture:texture
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [e1 endEncoding];
    [cb1 commit];
    [cb1 waitUntilCompleted];
    ASSERT_EQ(cb1.status, MTLCommandBufferStatusCompleted);

    // unusedAlias crosses 0xfe -> 0; usedAlias then places kind 0 on top of kind 0.
    id<MTLBuffer> unusedAlias = [heap newBufferWithLength:kBytes options:MTLResourceStorageModePrivate offset:0];
    id<MTLBuffer> usedAlias = [heap newBufferWithLength:kBytes options:MTLResourceStorageModePrivate offset:0];
    ASSERT_TRUE(unusedAlias != nil && usedAlias != nil);

    std::vector<uint8_t> expected(kBytes);
    id<MTLCommandBuffer> cb2 = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e2 = [cb2 blitCommandEncoder];
    for (NSUInteger off = 0; off < kBytes; off += kStripe) {
        const uint8_t v = uint8_t(0x31 + ((off / kStripe) * 9 + (off / 256) * 23) % 217);
        memset(expected.data() + off, v, kStripe);
        [e2 fillBuffer:usedAlias range:NSMakeRange(off, kStripe) value:v];
    }
    [e2 endEncoding];
    [cb2 commit];
    [cb2 waitUntilCompleted];
    ASSERT_EQ(cb2.status, MTLCommandBufferStatusCompleted);

    id<MTLCommandBuffer> cb3 = [queue commandBuffer];
    id<MTLBlitCommandEncoder> e3 = [cb3 blitCommandEncoder];
    [e3 copyFromBuffer:usedAlias sourceOffset:0 toBuffer:readback destinationOffset:0 size:kBytes];
    [e3 endEncoding];
    [cb3 commit];
    [cb3 waitUntilCompleted];
    ASSERT_EQ(cb3.status, MTLCommandBufferStatusCompleted);

    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::runEngines(first, run, true));
    const auto *got = static_cast<const uint8_t *>(readback.contents);
    size_t wrong = 0;
    for (NSUInteger i = 0; i < kBytes; ++i)
        wrong += got[i] != expected[i];
    EXPECT_EQ(wrong, 0u) << "second same-kind alias missed L2 invalidate after arena first crossed";
    (void)unusedAlias;
}

}  // namespace
