// SPDX-License-Identifier: MIT
// fillTexture:level:slice:region:color:[pixelFormat:] (Apple SPI) on the facade, run on the copy engine's and the 2D
// engine's models: every fill a raw texture takes stays raw and leaves the bytes plain Metal leaves (m2v blit_fill.rs,
// fill_value.rs, measured on an Apple M4): the colour packed by the fill's rules, repeated or cut at the texel, placed
// and cut as Metal places it.
#include "facade_blit_engines.h"
#include "kits/SwitchGM20B/SwitchGM20B2D.h"
#include "../fill_texel.h"

#import <IOSurface/IOSurface.h>

@interface NSObject (SwitchBlitFillTest)
- (void)fillTexture:(id<MTLTexture>)texture
              level:(NSUInteger)level
              slice:(NSUInteger)slice
             region:(MTLRegion)region
              color:(MTLClearColor)color;
- (void)fillTexture:(id<MTLTexture>)texture
              level:(NSUInteger)level
              slice:(NSUInteger)slice
             region:(MTLRegion)region
              color:(MTLClearColor)color
        pixelFormat:(MTLPixelFormat)format;
@end

namespace {

id<MTLDevice> device;

class FacadeFills : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        device = harness::open(harness::writePack(harness::packKey(MTLPixelFormatBGRA8Unorm)));
        ASSERT_NE(device, nil);
    }

    static id<MTLTexture> texture(MTLPixelFormat format, NSUInteger side, MTLTextureType type = MTLTextureType2D,
                                  NSUInteger depth = 1, NSUInteger samples = 1, bool mipmapped = false)
    {
        auto *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:side height:side mipmapped:mipmapped];
        d.textureType = type;
        d.depth = type == MTLTextureType3D ? depth : 1;
        d.arrayLength = type == MTLTextureType2DArray ? depth : 1;
        d.sampleCount = samples;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        return [device newTextureWithDescriptor:d];
    }

    // The host bytes of `t`'s memory, every one set to `fill` first
    static uint8_t *storage(id<MTLTexture> t, uint64_t &bytes, uint8_t fill)
    {
        const auto *x = (SwitchMetalTexture *)t;
        const auto range = (x->_levelsOwner ?: x)->_raw.storageRange();
        bytes = range.bytes;
        auto *host = kernel_model::model.host(range.va, range.bytes);
        if (host)
            memset(host, fill, range.bytes);
        return host;
    }

    // Each texel of a w x h plane read out of `t` (level 0, slice `slice`, plane z) into a buffer
    static void plane(id<MTLBlitCommandEncoder> e, id<MTLTexture> t, id<MTLBuffer> b, NSUInteger side, NSUInteger bpp,
                      NSUInteger slice, NSUInteger z, NSUInteger at)
    {
        [e copyFromTexture:t
                         sourceSlice:slice
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, z)
                          sourceSize:MTLSizeMake(side, side, 1)
                            toBuffer:b
                   destinationOffset:at
              destinationBytesPerRow:side * bpp
            destinationBytesPerImage:side * side * bpp];
    }

    static void fill(id<MTLBlitCommandEncoder> e, id<MTLTexture> t, NSUInteger slice, MTLRegion region,
                     MTLClearColor color, MTLPixelFormat format = MTLPixelFormatInvalid)
    {
        if (format == MTLPixelFormatInvalid)
            [(id)e fillTexture:t level:0 slice:slice region:region color:color];
        else
            [(id)e fillTexture:t level:0 slice:slice region:region color:color pixelFormat:format];
    }

    static std::vector<uint8_t> packed(MTLPixelFormat format, MTLClearColor c)
    {
        const double rgba[4] = {c.red, c.green, c.blue, c.alpha};
        uint8_t out[16];
        const uint32_t n = gm20b::fillTexel(uint32_t(format), rgba, out);
        return std::vector<uint8_t>(out, out + n);
    }
};

// RGB9E5Float, which the GM20B renders none of, fills its region with the fill's packed texel on the copy engine
TEST_F(FacadeFills, AFormatTheGM20BRendersNoneOfFillsItsPackedTexel)
{
    constexpr NSUInteger side = 16;
    auto t = texture(MTLPixelFormatRGB9E5Float, side);
    ASSERT_NE(t, nil);
    ASSERT_TRUE(((SwitchMetalTexture *)t)->_gm20bOk);
    auto in = [device newBufferWithLength:side * side * 4 options:MTLResourceStorageModeShared];
    auto out = [device newBufferWithLength:side * side * 4 options:MTLResourceStorageModeShared];
    memset(in.contents, 0x5c, in.length);
    const MTLClearColor colour = MTLClearColorMake(0.25, 0.5, 0.75, 1);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    [e copyFromBuffer:in
               sourceOffset:0
          sourceBytesPerRow:side * 4
        sourceBytesPerImage:side * side * 4
                 sourceSize:MTLSizeMake(side, side, 1)
                  toTexture:t
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    fill(e, t, 0, MTLRegionMake2D(3, 5, 7, 9), colour);
    plane(e, t, out, side, 4, 0, 0, 0);
    [e endEncoding];
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    const std::vector<uint8_t> want = {0x80, 0x00, 0x02, 0x7e};  // Metal's texel (m2v fill_value.rs, EV FRT-fil1-m4)
    ASSERT_EQ(packed(MTLPixelFormatRGB9E5Float, colour), want);
    const auto *got = static_cast<const uint8_t *>(out.contents);
    for (NSUInteger y = 0; y < side; ++y)
        for (NSUInteger x = 0; x < side; ++x)
            for (NSUInteger c = 0; c < 4; ++c)
                ASSERT_EQ(got[(y * side + x) * 4 + c], x >= 3 && x < 10 && y >= 5 && y < 14 ? want[c] : 0x5c)
                    << x << ',' << y << ',' << c;
}

// A 3D texture's fill writes the region's planes, cut at the level's depth
TEST_F(FacadeFills, A3DFillWritesTheRegionsPlanes)
{
    constexpr NSUInteger side = 8, depth = 4;
    auto t = texture(MTLPixelFormatRGBA8Unorm, side, MTLTextureType3D, depth);
    ASSERT_NE(t, nil);
    auto in = [device newBufferWithLength:side * side * 4 * depth options:MTLResourceStorageModeShared];
    auto out = [device newBufferWithLength:side * side * 4 * depth options:MTLResourceStorageModeShared];
    memset(in.contents, 0x5c, in.length);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    [e copyFromBuffer:in
               sourceOffset:0
          sourceBytesPerRow:side * 4
        sourceBytesPerImage:side * side * 4
                 sourceSize:MTLSizeMake(side, side, depth)
                  toTexture:t
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    fill(e, t, 0, MTLRegionMake3D(1, 2, 2, 4, 3, 9), MTLClearColorMake(1, 0, 0, 1));
    for (NSUInteger z = 0; z < depth; ++z)
        plane(e, t, out, side, 4, 0, z, z * side * side * 4);
    [e endEncoding];
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    const uint8_t red[4] = {0xff, 0, 0, 0xff};
    const auto *got = static_cast<const uint8_t *>(out.contents);
    for (NSUInteger z = 0; z < depth; ++z)
        for (NSUInteger y = 0; y < side; ++y)
            for (NSUInteger x = 0; x < side; ++x)
                for (NSUInteger c = 0; c < 4; ++c)
                    ASSERT_EQ(got[((z * side + y) * side + x) * 4 + c],
                              z >= 2 && x >= 1 && x < 5 && y >= 2 && y < 5 ? red[c] : 0x5c)
                        << x << ',' << y << ',' << z << ',' << c;
}

// A multisampled texture's fill writes every sample of its region's pixels: the whole level, each sample once
TEST_F(FacadeFills, AMultisampledFillWritesEverySample)
{
    constexpr NSUInteger side = 8;
    auto t = texture(MTLPixelFormatRGBA8Unorm, side, MTLTextureType2DMultisample, 1, 4);
    ASSERT_NE(t, nil);
    ASSERT_TRUE(((SwitchMetalTexture *)t)->_gm20bOk);
    uint64_t bytes = 0;
    uint8_t *memory = storage(t, bytes, 0xee);
    ASSERT_NE(memory, nullptr);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    fill(e, t, 0, MTLRegionMake2D(0, 0, side, side), MTLClearColorMake(0, 0, 1, 1));
    [e endEncoding];
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    const uint8_t blue[4] = {0, 0, 0xff, 0xff};
    uint64_t written = 0;
    for (uint64_t i = 0; i + 4 <= bytes; i += 4)
        if (memcmp(memory + i, blue, 4) == 0)
            written++;
    EXPECT_EQ(written, 4u * side * side);
}

// A colour packed in another format than the texture's is that format's texel, repeated or cut at the texture's (EV
// FRT-fil4-m4): R8Unorm's byte across an RGBA32Float texel, RGBA16Float's first four bytes in an RGBA8Unorm one
TEST_F(FacadeFills, AColourPackedInAnotherFormatRepeatsOrCutsItsTexel)
{
    constexpr NSUInteger side = 4;
    auto wide = texture(MTLPixelFormatRGBA32Float, side), narrow = texture(MTLPixelFormatRGBA8Unorm, side);
    auto out = [device newBufferWithLength:side * side * 16 * 2 options:MTLResourceStorageModeShared];
    const MTLClearColor colour = MTLClearColorMake(0.25, 0.5, 0.75, 1);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
    fill(e, wide, 0, MTLRegionMake2D(0, 0, side, side), colour, MTLPixelFormatR8Unorm);
    fill(e, narrow, 0, MTLRegionMake2D(0, 0, side, side), colour, MTLPixelFormatRGBA16Float);
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded + 2);
    plane(e, wide, out, side, 16, 0, 0, 0);
    plane(e, narrow, out, side, 4, 0, 0, side * side * 16);
    [e endEncoding];
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    EXPECT_EQ(run.solids, 1u) << "RGBA8Unorm's on the 2D engine, RGBA32Float's on the copy engine";
    const std::vector<uint8_t> half = packed(MTLPixelFormatRGBA16Float, colour);
    ASSERT_EQ(half.size(), 8u);
    const auto *got = static_cast<const uint8_t *>(out.contents);
    for (NSUInteger i = 0; i < side * side * 16; ++i)
        ASSERT_EQ(got[i], 0x40) << i;
    for (NSUInteger i = 0; i < side * side * 4; ++i)
        ASSERT_EQ(got[side * side * 16 + i], half[i % 4]) << i;
}

// Depth32Float_Stencil8's fill writes each texel's depth word and keeps its stencil; its stencil view's writes the stencil
// byte (the word after the depth: its other bytes are padding) and keeps the depth
TEST_F(FacadeFills, ADepthStencilFillWritesOneAspect)
{
    constexpr NSUInteger side = 4;
    auto t = texture(MTLPixelFormatDepth32Float_Stencil8, side);
    ASSERT_NE(t, nil);
    auto stencil = [t newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
    ASSERT_NE(stencil, nil);
    for (bool depth : {true, false}) {
        SCOPED_TRACE(depth);
        uint64_t bytes = 0;
        uint8_t *memory = storage(t, bytes, 0xee);
        ASSERT_NE(memory, nullptr);
        auto cb = [[device newCommandQueue] commandBuffer];
        auto e = [cb blitCommandEncoder];
        if (depth)
            fill(e, t, 0, MTLRegionMake2D(0, 0, side, side), MTLClearColorMake(0.5, 0, 0, 0));
        else
            fill(e, stencil, 0, MTLRegionMake2D(0, 0, side, side), MTLClearColorMake(7, 0, 0, 0), MTLPixelFormatR8Uint);
        [e endEncoding];
        harness::EngineRun run;
        ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
        uint64_t changed = 0;
        for (uint64_t i = 0; i < bytes; ++i) {
            if (memory[i] == 0xee)
                continue;
            changed++;
            ASSERT_EQ(i % 8 < 4, depth) << i;
            if (!depth && i % 8 == 4)
                ASSERT_EQ(memory[i], 7) << i;
        }
        EXPECT_EQ(changed, 4u * side * side);
    }
}

// Plain Metal's placement: a cube takes the slice modulo 6, a 2D texture ignores it, an array writes nothing past its
// slices, and a region past the level is cut there
TEST_F(FacadeFills, AFillIsPlacedAsPlainMetalPlacesIt)
{
    constexpr NSUInteger side = 4;
    auto cube = texture(MTLPixelFormatRGBA8Unorm, side, MTLTextureTypeCube);
    auto flat = texture(MTLPixelFormatRGBA8Unorm, side);
    auto array = texture(MTLPixelFormatRGBA8Unorm, side, MTLTextureType2DArray, 2);
    ASSERT_TRUE(cube && flat && array);
    auto out = [device newBufferWithLength:8 * side * side * 4 options:MTLResourceStorageModeShared];
    memset(out.contents, 0x5c, out.length);
    const MTLClearColor white = MTLClearColorMake(1, 1, 1, 1);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    for (id<MTLTexture> t in @[ cube, flat, array ])
        for (NSUInteger slice = 0; slice < (t == cube ? 6u : t == array ? 2u : 1u); ++slice)
            fill(e, t, slice, MTLRegionMake2D(0, 0, side, side), MTLClearColorMake(0, 0, 0, 0));
    const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
    fill(e, array, 2, MTLRegionMake2D(0, 0, side, side), white);
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded) << "a slice past the array's writes nothing";
    fill(e, cube, 7, MTLRegionMake2D(0, 0, side, side), white);
    fill(e, flat, 5, MTLRegionMake2D(2, 3, 10, 10), white);
    for (NSUInteger face = 0; face < 6; ++face)
        plane(e, cube, out, side, 4, face, 0, face * side * side * 4);
    plane(e, flat, out, side, 4, 0, 0, 6 * side * side * 4);
    plane(e, array, out, side, 4, 1, 0, 7 * side * side * 4);
    [e endEncoding];
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    EXPECT_EQ(run.solids, 11u);
    const auto *got = static_cast<const uint8_t *>(out.contents);
    for (NSUInteger image = 0; image < 8; ++image)
        for (NSUInteger y = 0; y < side; ++y)
            for (NSUInteger x = 0; x < side; ++x) {
                const bool lit = image == 1 || (image == 6 && x >= 2 && y >= 3);
                for (NSUInteger c = 0; c < 4; ++c)
                    ASSERT_EQ(got[((image * side + y) * side + x) * 4 + c], lit ? 0xff : 0)
                        << image << ':' << x << ',' << y;
            }
}

// A rotation-4 drawable's fill lands where Metal stores its frame (rotation.h: rotation 1's map), the plane's other
// texels as they were: no pass loads the plane, so its transposed load never applies
TEST_F(FacadeFills, ARotation4FillLandsTurnedInItsPlane)
{
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @32,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @256,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_NE(surface, nullptr);
    auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                 width:64
                                                                height:32
                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    [d setValue:@YES forKey:@"framebufferOnly"];
    [d setValue:@4 forKey:@"rotation"];
    id<MTLTexture> frame = [device newTextureWithDescriptor:d iosurface:surface plane:0];
    ASSERT_NE(frame, nil);
    ASSERT_TRUE(((SwitchMetalTexture *)frame)->_gm20bOk);
    auto *base = static_cast<uint8_t *>(IOSurfaceGetBaseAddress(surface));
    memset(base, 0x5c, 256 * 64);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    fill(e, frame, 0, MTLRegionMake2D(0, 0, 8, 4), MTLClearColorMake(1, 0, 0, 1));
    [e endEncoding];
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    EXPECT_EQ(run.solids, 1u);
    const uint8_t red[4] = {0, 0, 0xff, 0xff};  // BGRA
    for (NSUInteger y = 0; y < 64; ++y)
        for (NSUInteger x = 0; x < 32; ++x)
            for (NSUInteger c = 0; c < 4; ++c)
                ASSERT_EQ(base[y * 256 + x * 4 + c], x >= 28 && y < 8 ? red[c] : 0x5c) << x << ',' << y;
    CFRelease(surface);
}

// A texture buffer's fill, as an M4 Max writes it: along x alone, into row 0 whatever its y, up to its buffer's end past
// the texture's own texels (macOS 27.2.1)
TEST_F(FacadeFills, ATextureBuffersFillRunsAlongItsBuffer)
{
    id<MTLBuffer> b = [device newBufferWithLength:1024 options:MTLResourceStorageModeShared];
    auto *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                     width:64
                                                           resourceOptions:MTLResourceStorageModeShared
                                                                     usage:MTLTextureUsageShaderRead];
    id<MTLTexture> t = [b newTextureWithDescriptor:d offset:64 bytesPerRow:256];
    ASSERT_NE(t, nil);
    ASSERT_TRUE(((SwitchMetalTexture *)t)->_gm20bOk);
    memset(b.contents, 0xee, b.length);
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    fill(e, t, 0, MTLRegionMake2D(60, 1, 10, 2), MTLClearColorMake(0, 1, 0, 1));
    [e endEncoding];
    harness::EngineRun run;
    ASSERT_NO_FATAL_FAILURE(harness::commitOnRaw(cb, run));
    const auto *got = static_cast<const uint8_t *>(b.contents);
    const uint8_t green[4] = {0, 0xff, 0, 0xff};
    for (NSUInteger i = 0; i < b.length; ++i)
        ASSERT_EQ(got[i], i >= 64 + 60 * 4 && i < 64 + 70 * 4 ? green[i % 4] : 0xee) << i;
}

TEST_F(FacadeFills, AnArrayLevelFillIsTheTwoDEnginesRectangleAtItsLayer)
{
    auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                 width:32
                                                                height:32
                                                             mipmapped:YES];
    d.textureType = MTLTextureType2DArray;
    d.arrayLength = 2;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    auto t = [device newTextureWithDescriptor:d];
    gm20b::Texture level;
    ASSERT_TRUE(gm20b::chainLevelImage(((SwitchMetalTexture *)t)->_raw, 1, level));
    const uint64_t expected = level.va + level.layerBytes;
    const size_t first = kernel_model::model.jobs.size();
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    [(id)e fillTexture:t level:1 slice:1 region:MTLRegionMake2D(3, 5, 7, 9) color:MTLClearColorMake(1, 0, 0, 1)];
    [e endEncoding];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    auto *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
    EXPECT_TRUE([path isEqualToString:@"gm20b"]);
    uint64_t address = 0;
    unsigned fills = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        ASSERT_GE(count, 0);
        for (long k = 0; k < count; ++k) {
            const auto &m = methods[k];
            if (m.subchannel != gm20b::SUBC_2D)
                continue;
            if (m.method == NV902D_SET_DST_OFFSET_UPPER)
                address = uint64_t(m.value) << 32;
            if (m.method == NV902D_SET_DST_OFFSET_LOWER)
                address |= m.value;
            if (m.method == NV902D_SET_RENDER_SOLID_PRIM_COLOR) {
                EXPECT_EQ(address, expected);
                EXPECT_EQ(m.value, 0xff0000ffu);
                ++fills;
            }
        }
    }
    EXPECT_EQ(fills, 1u);
}

TEST_F(FacadeFills, APartViewsFillWritesItsParentsLevel)
{
    auto t = texture(MTLPixelFormatRGBA8Unorm, 32, MTLTextureType2D, 1, 1, true);
    auto v = [t newTextureViewWithPixelFormat:t.pixelFormat
                                  textureType:MTLTextureType2D
                                       levels:NSMakeRange(1, 4)
                                       slices:NSMakeRange(0, 1)];
    ASSERT_NE(v, nil);
    ASSERT_TRUE(((SwitchMetalTexture *)v)->_gm20bOk);
    gm20b::Texture level;
    ASSERT_TRUE(gm20b::chainLevelImage(((SwitchMetalTexture *)t)->_raw, 2, level));
    const uint64_t expected = level.va;
    const size_t first = kernel_model::model.jobs.size();
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    [(id)e fillTexture:v level:1 slice:0 region:MTLRegionMake2D(1, 2, 3, 4) color:MTLClearColorMake(1, 0, 0, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    uint64_t address = 0;
    unsigned fills = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        ASSERT_GE(count, 0);
        for (long k = 0; k < count; ++k) {
            const auto &m = methods[k];
            if (m.subchannel != gm20b::SUBC_2D)
                continue;
            if (m.method == NV902D_SET_DST_OFFSET_UPPER)
                address = uint64_t(m.value) << 32;
            if (m.method == NV902D_SET_DST_OFFSET_LOWER)
                address |= m.value;
            if (m.method == NV902D_SET_RENDER_SOLID_PRIM_COLOR) {
                EXPECT_EQ(address, expected);
                EXPECT_EQ(m.value, 0xff0000ffu);
                ++fills;
            }
        }
    }
    EXPECT_EQ(fills, 1u);
}

}  // namespace
