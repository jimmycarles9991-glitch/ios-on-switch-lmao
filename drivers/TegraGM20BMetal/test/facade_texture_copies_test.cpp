// SPDX-License-Identifier: MIT
// Metal's copies between textures as the facade records them (facade_harness.h), run on the copy engine's model:
// Metal's copy units (a stencil view's stencil with a texture of bytes, a combined depth/stencil format into its own
// alone), plain Metal's blit options, and the whole-texture copy's levels
#include "facade_blit_engines.h"
#include "../TegraGM20BMetalInternal.h"

#include <gtest/gtest.h>

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

}  // namespace
