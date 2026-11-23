// SPDX-License-Identifier: MIT
// generateMipmapsForTexture: on the facade: Metal's leaves and stops, each format's engine (the 2D engine, the pack's
// mip states, the word mip states of formats the GM20B cannot filter), views' levels and layers.
#include "facade_blit_engines.h"
#include "kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../device_answers.h"

namespace {

id<MTLDevice> device;

class FacadeMipmaps : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        device = harness::open(harness::writePack(harness::packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                                  {{"raw.mip.mip_rgba8_srgb_frag", MTLPixelFormatRGBA8Unorm_sRGB},
                                                   {"raw.mip.mip_3d_rgba8_srgb_frag", MTLPixelFormatRGBA8Unorm_sRGB},
                                                   {"raw.fmt.fmt_mip_srgb8_frag", MTLPixelFormatR8Uint},
                                                   {"raw.fmt.fmt_mip_3d_srgb8_frag", MTLPixelFormatR8Uint},
                                                   {"raw.fmt.fmt_mip_bgr10_xr_frag", MTLPixelFormatR32Uint}}));
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

    // The colour targets (address, format) the 3D engine's passes from job `first` on bound
    static std::vector<std::pair<uint64_t, uint32_t>> targets(size_t first)
    {
        std::vector<std::pair<uint64_t, uint32_t>> out;
        uint64_t address = 0;
        for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
            const auto &words = kernel_model::model.jobs[j].words;
            std::vector<switch_gm20b_method> methods(words.size());
            const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
            for (long k = 0; k < count; ++k) {
                const auto &m = methods[k];
                if (m.subchannel != gm20b::SUBC_3D)
                    continue;
                if (m.method == NVB197_SET_COLOR_TARGET_A(0))
                    address = uint64_t(m.value) << 32;
                else if (m.method == NVB197_SET_COLOR_TARGET_B(0))
                    address |= m.value;
                else if (m.method == NVB197_SET_COLOR_TARGET_FORMAT(0) && m.value)
                    out.emplace_back(address, m.value);
            }
        }
        return out;
    }
};

TEST_F(FacadeMipmaps, GenerateMipmapsLeavesOneLevelAndDepthStencilTexturesUnchanged)
{
    std::vector<id<MTLTexture>> textures;
    for (MTLTextureType type : {MTLTextureType1D, MTLTextureType1DArray, MTLTextureType2D, MTLTextureType2DArray,
                                MTLTextureTypeCube, MTLTextureType3D, MTLTextureType2DMultisample}) {
        auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                     width:32
                                                                    height:32
                                                                 mipmapped:NO];
        d.textureType = type;
        d.height = type == MTLTextureType1D || type == MTLTextureType1DArray ? 1 : 32;
        d.depth = type == MTLTextureType3D ? 3 : 1;
        d.arrayLength = type == MTLTextureType1DArray || type == MTLTextureType2DArray ? 2 : 1;
        d.sampleCount = type == MTLTextureType2DMultisample ? 4 : 1;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead;
        textures.push_back([device newTextureWithDescriptor:d]);
    }
    for (MTLPixelFormat format : {MTLPixelFormatR8Uint, MTLPixelFormatDepth16Unorm, MTLPixelFormatDepth32Float,
                                  MTLPixelFormatStencil8, MTLPixelFormatDepth32Float_Stencil8}) {
        auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                     width:32
                                                                    height:32
                                                                 mipmapped:format != MTLPixelFormatR8Uint];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        auto t = [device newTextureWithDescriptor:d];
        textures.push_back(t);
        if (format == MTLPixelFormatDepth32Float_Stencil8)
            textures.push_back([t newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8]);
    }
    auto parent = texture();
    textures.push_back([parent newTextureViewWithPixelFormat:parent.pixelFormat
                                                 textureType:MTLTextureType2D
                                                      levels:NSMakeRange(2, 1)
                                                      slices:NSMakeRange(0, 1)]);
    auto b = buffer();
    auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                 width:32
                                                                height:1
                                                             mipmapped:NO];
    d.textureType = MTLTextureTypeTextureBuffer;
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageShaderRead;
    textures.push_back([b newTextureWithDescriptor:d offset:0 bytesPerRow:0]);
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @32,
        (id)kIOSurfaceHeight : @32,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfacePixelFormat : @(uint32_t('RGBA'))
    });
    ASSERT_NE(surface, nullptr);
    d.textureType = MTLTextureType2D;
    d.height = 32;
    textures.push_back([device newTextureWithDescriptor:d iosurface:surface plane:0]);
    CFRelease(surface);
    for (auto t : textures) {
        ASSERT_NE(t, nil);
        SCOPED_TRACE(t.description.UTF8String);
        auto *raw = (SwitchMetalTexture *)t;
        ASSERT_TRUE(raw->_gm20bOk);
        const auto range = (raw->_levelsOwner ?: raw)->_raw.storageRange();
        auto *bytes = kernel_model::model.host(range.va, range.bytes);
        ASSERT_NE(bytes, nullptr);
        std::vector<uint8_t> sentinel(range.bytes);
        for (size_t i = 0; i < sentinel.size(); ++i)
            sentinel[i] = uint8_t(i * 13 + i / 64);
        memcpy(bytes, sentinel.data(), sentinel.size());
        auto cb = [[device newCommandQueue] commandBuffer];
        auto e = [cb blitCommandEncoder];
        const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
        [e generateMipmapsForTexture:t];
        EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded);
        EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
        EXPECT_FALSE(raw->_gpuWrites.load());
        [e endEncoding];
        execute(cb, 0, true);
        EXPECT_EQ(memcmp(bytes, sentinel.data(), sentinel.size()), 0);
    }
}

TEST_F(FacadeMipmaps, GenerateMipmapsRejectsInvalidResourcesBeforeRecordingOrSubmitting)
{
    auto ordinary = texture();
    ASSERT_NE(ordinary, nil);
    std::vector<id<MTLTexture>> invalid = {nil, (id<MTLTexture>)[NSObject new]};
    for (BOOL mips : {NO, YES}) {
        auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                     width:32
                                                                    height:32
                                                                 mipmapped:mips];
        d.storageMode = MTLStorageModeMemoryless;
        d.usage = MTLTextureUsageRenderTarget;
        invalid.push_back([device newTextureWithDescriptor:d]);
        ASSERT_NE(invalid.back(), nil);
    }
    invalid.push_back(ordinary);
    for (auto t : invalid) {
        auto *raw = t == ordinary ? (SwitchMetalTexture *)ordinary : nil;
        id saved = raw ? raw->_device : nil;
        if (raw)
            raw->_device = nil;
        const size_t jobs = kernel_model::model.jobs.size();
        auto cb = [[device newCommandQueue] commandBuffer];
        auto e = [cb blitCommandEncoder];
        const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
        [e generateMipmapsForTexture:t];
        if (raw)
            raw->_device = saved;
        EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded);
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
        EXPECT_TRUE([cb.error.domain isEqualToString:MTLCommandBufferErrorDomain]);
        EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInvalidResource);
        EXPECT_EQ(kernel_model::model.jobs.size(), jobs);
    }
}

// A 3D chain of a format the GM20B renders none of, and a 2D chain of an extended-range format, draw each level's words
// from the level above's by their word mip states (mip.h): a pass a plane into the words' integer view, on raw
TEST_F(FacadeMipmaps, GenerateMipmapsOfWordFormatsDrawsTheirWords)
{
    struct Case {
        MTLPixelFormat format;
        MTLTextureType type;
        NSUInteger side, depth;
        uint32_t view;
    };

    for (const Case &c : {Case{MTLPixelFormatR8Unorm_sRGB, MTLTextureType3D, 32, 4, 13},
                          Case{MTLPixelFormatBGR10_XR, MTLTextureType2D, 16, 1, 53}}) {
        SCOPED_TRACE(c.format);
        auto *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.format width:c.side height:c.side mipmapped:YES];
        d.textureType = c.type;
        d.depth = c.depth;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead;
        auto t = [device newTextureWithDescriptor:d];
        ASSERT_NE(t, nil);
        ASSERT_TRUE(((SwitchMetalTexture *)t)->_gm20bOk);
        EXPECT_EQ(((SwitchMetalTexture *)t)->_raw.blockDepth, 0u)
            << "a mipmapped volume of words is laid out one plane a block";
        const auto &chain = *((SwitchMetalTexture *)t)->_raw.chain;
        std::vector<std::pair<uint64_t, uint32_t>> want;
        for (uint32_t level = 1; level < chain.layout.levels; ++level)
            for (uint32_t z = 0; z < chain.layout.level[level].depth; ++z)
                want.emplace_back(chain.va + chain.layout.level[level].offset +
                                      uint64_t(z) * chain.layout.level[level].rowBytes * chain.layout.level[level].rows,
                                  gm20b::formatInfo(c.view)->rt);
        const size_t first = kernel_model::model.jobs.size();
        auto cb = [[device newCommandQueue] commandBuffer];
        auto e = [cb blitCommandEncoder];
        [e generateMipmapsForTexture:t];
        [e endEncoding];
        EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
        auto *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
        ASSERT_TRUE([path isEqualToString:@"gm20b"]) << path.UTF8String;
        EXPECT_EQ(targets(first), want);
    }
}

// An integer format's levels as an M4 Max makes them (validation off, every integer format, 2D, arrays and cubes): texel
// (x, y) of each level the level above's (2x + 1, 2y + 1), clamped to its last, every bit kept (words that are float
// denormals and NaNs among them), by the 2D engine's point steps: texels of 1 to 16 bytes, an axis of one texel, a
// layer of an array
TEST_F(FacadeMipmaps, GenerateMipmapsOfAnIntegerFormatTakesTheOddTexelOfTheLevelAbove)
{
    struct Case {
        MTLPixelFormat format;
        uint32_t bpp, width, height, layers;
    };

    for (const Case &c : {Case{MTLPixelFormatRGBA8Uint, 4, 63, 61, 1}, Case{MTLPixelFormatR8Uint, 1, 37, 5, 1},
                          Case{MTLPixelFormatRG8Sint, 2, 64, 1, 1}, Case{MTLPixelFormatRGBA16Uint, 8, 45, 30, 1},
                          Case{MTLPixelFormatRGBA32Uint, 16, 33, 17, 1}, Case{MTLPixelFormatRGBA8Uint, 4, 33, 20, 2}}) {
        SCOPED_TRACE(testing::Message() << c.format << " " << c.width << "x" << c.height << " layers " << c.layers);
        auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:c.format
                                                                     width:c.width
                                                                    height:c.height
                                                                 mipmapped:YES];
        d.textureType = c.layers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
        d.arrayLength = c.layers;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        auto t = [device newTextureWithDescriptor:d];
        ASSERT_NE(t, nil);
        ASSERT_TRUE(((SwitchMetalTexture *)t)->_gm20bOk);
        const uint32_t levels = uint32_t(t.mipmapLevelCount);
        std::vector<uint64_t> at;
        uint64_t bytes = 0;
        for (uint32_t layer = 0; layer < c.layers; ++layer)
            for (uint32_t level = 0; level < levels; ++level) {
                at.push_back(bytes);
                bytes += uint64_t(std::max(1u, c.width >> level)) * std::max(1u, c.height >> level) * c.bpp;
            }
        auto in = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        auto out = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
        auto *source = static_cast<uint8_t *>(in.contents);
        memset(source, 0x22, bytes);
        for (uint32_t layer = 0; layer < c.layers; ++layer)
            for (uint64_t i = 0; i < uint64_t(c.width) * c.height * c.bpp; i += 4) {
                const uint32_t k = uint32_t(i / 4) + 977 * layer;
                const uint32_t word = k % 3 == 0   ? k + 1
                                      : k % 3 == 1 ? 0x7f800001u + k
                                                   : 0x80000000u | (k * 2654435761u >> 9);
                for (uint32_t b = 0; b < 4 && i + b < uint64_t(c.width) * c.height * c.bpp; ++b)
                    source[at[layer * levels] + i + b] = uint8_t(word >> (8 * b));
            }
        auto cb = [[device newCommandQueue] commandBuffer];
        auto e = [cb blitCommandEncoder];
        const auto each = [&](auto &&copy) {
            for (uint32_t layer = 0; layer < c.layers; ++layer)
                for (uint32_t level = 0; level < levels; ++level) {
                    const uint32_t w = std::max(1u, c.width >> level), h = std::max(1u, c.height >> level);
                    copy(layer, level, w, h, at[layer * levels + level]);
                }
        };
        each([&](uint32_t layer, uint32_t level, uint32_t w, uint32_t h, uint64_t offset) {
            [e copyFromBuffer:in
                       sourceOffset:offset
                  sourceBytesPerRow:w * c.bpp
                sourceBytesPerImage:w * h * c.bpp
                         sourceSize:MTLSizeMake(w, h, 1)
                          toTexture:t
                   destinationSlice:layer
                   destinationLevel:level
                  destinationOrigin:MTLOriginMake(0, 0, 0)];
        });
        [e generateMipmapsForTexture:t];
        each([&](uint32_t layer, uint32_t level, uint32_t w, uint32_t h, uint64_t offset) {
            [e copyFromTexture:t
                             sourceSlice:layer
                             sourceLevel:level
                            sourceOrigin:MTLOriginMake(0, 0, 0)
                              sourceSize:MTLSizeMake(w, h, 1)
                                toBuffer:out
                       destinationOffset:offset
                  destinationBytesPerRow:w * c.bpp
                destinationBytesPerImage:w * h * c.bpp];
        });
        [e endEncoding];
        EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
        execute(cb, (levels - 1) * c.layers);
        const auto *got = static_cast<const uint8_t *>(out.contents);
        for (uint32_t layer = 0; layer < c.layers; ++layer) {
            std::vector<uint8_t> above(source + at[layer * levels],
                                       source + at[layer * levels] + c.width * c.height * c.bpp);
            uint32_t aw = c.width, ah = c.height;
            for (uint32_t level = 1; level < levels; ++level) {
                const uint32_t w = std::max(1u, c.width >> level), h = std::max(1u, c.height >> level);
                std::vector<uint8_t> want(uint64_t(w) * h * c.bpp);
                for (uint32_t y = 0; y < h; ++y)
                    for (uint32_t x = 0; x < w; ++x)
                        memcpy(
                            &want[(uint64_t(y) * w + x) * c.bpp],
                            &above[(uint64_t(std::min(2 * y + 1, ah - 1)) * aw + std::min(2 * x + 1, aw - 1)) * c.bpp],
                            c.bpp);
                EXPECT_EQ(memcmp(got + at[layer * levels + level], want.data(), want.size()), 0)
                    << "layer " << layer << " level " << level;
                above = want;
                aw = w, ah = h;
            }
        }
    }
}

// A 3D texture of an integer format: the 2D engine moves one plane, so no engine of the device takes a level's planes
// from the level above's: the command buffer fails Internal, nothing is submitted
TEST_F(FacadeMipmaps, GenerateMipmapsOfAThreeDIntegerTextureFailsInternally)
{
    auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Uint
                                                                 width:9
                                                                height:6
                                                             mipmapped:YES];
    d.textureType = MTLTextureType3D;
    d.depth = 5;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead;
    auto t = [device newTextureWithDescriptor:d];
    ASSERT_NE(t, nil);
    const size_t jobs = kernel_model::model.jobs.size();
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    [e generateMipmapsForTexture:t];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal);
    EXPECT_EQ(kernel_model::model.jobs.size(), jobs);
}

// A chain whose word mip state the device's pack lacks (a stale pack) fails its command buffer, no m2v asked
TEST_F(FacadeMipmaps, GenerateMipmapsWithoutItsPackStateFailsInternally)
{
    auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA10_XR
                                                                 width:16
                                                                height:16
                                                             mipmapped:YES];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead;
    auto t = [device newTextureWithDescriptor:d];
    ASSERT_NE(t, nil);
    ASSERT_TRUE(((SwitchMetalTexture *)t)->_gm20bOk);
    const size_t jobs = kernel_model::model.jobs.size();
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    [e generateMipmapsForTexture:t];
    [e endEncoding];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal);
    EXPECT_EQ(kernel_model::model.jobs.size(), jobs);
}

TEST_F(FacadeMipmaps, GenerateMipmapsFailsAnAbsentNativeChainInternally)
{
    auto t = texture();
    ASSERT_NE(t, nil);
    auto *raw = (SwitchMetalTexture *)t;
    auto *chain = raw->_raw.chain;
    ASSERT_NE(chain, nullptr);
    raw->_raw.chain = nullptr;
    const size_t jobs = kernel_model::model.jobs.size();
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    const size_t recorded = ((SwitchMetalCommandBuffer *)cb)->_plan.size();
    [e generateMipmapsForTexture:t];
    raw->_raw.chain = chain;
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_plan.size(), recorded);
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_TRUE([cb.error.domain isEqualToString:MTLCommandBufferErrorDomain]);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal);
    EXPECT_EQ(kernel_model::model.jobs.size(), jobs);
}

TEST_F(FacadeMipmaps, GenerateMipmapsOfANestedViewKeepsItsParentNeighbors)
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
    auto in = [device newBufferWithLength:12 * 4096 options:MTLResourceStorageModeShared];
    auto out = [device newBufferWithLength:12 * 4096 options:MTLResourceStorageModeShared];
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    for (NSUInteger slice = 0; slice < 2; ++slice)
        for (NSUInteger level = 0; level < 6; ++level) {
            const NSUInteger w = 32 >> level, at = (slice * 6 + level) * 4096;
            memset(static_cast<uint8_t *>(in.contents) + at, int(0x20 + slice * 32 + level), w * w * 4);
            upload(e, in, parent, level, slice, at);
        }
    [e generateMipmapsForTexture:nested];
    for (NSUInteger slice = 0; slice < 2; ++slice)
        for (NSUInteger level = 0; level < 6; ++level) {
            const NSUInteger at = (slice * 6 + level) * 4096;
            read(e, parent, out, level, slice, at);
        }
    [e endEncoding];
    EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
    execute(cb, 1);
    for (NSUInteger slice = 0; slice < 2; ++slice)
        for (NSUInteger level = 0; level < 6; ++level) {
            const NSUInteger w = 32 >> level, at = (slice * 6 + level) * 4096;
            const uint8_t want = uint8_t(0x20 + slice * 32 + (slice == 1 && level == 3 ? 2 : level));
            const auto *got = static_cast<const uint8_t *>(out.contents) + at;
            for (NSUInteger b = 0; b < w * w * 4; ++b)
                ASSERT_EQ(got[b], want) << slice << ',' << level << ',' << b;
        }
}

TEST_F(FacadeMipmaps, GenerateMipmapsUsesTheViewFormatAndParentRangeForArraysCubesAndVolumes)
{
    for (MTLTextureType type : {MTLTextureType2DArray, MTLTextureTypeCube, MTLTextureTypeCubeArray, MTLTextureType3D}) {
        SCOPED_TRACE(type);
        const bool volume = type == MTLTextureType3D, cubeArray = type == MTLTextureTypeCubeArray,
                   cube = type == MTLTextureTypeCube || cubeArray;
        auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                     width:volume ? 9 : 32
                                                                    height:volume ? 6 : 32
                                                                 mipmapped:YES];
        d.textureType = type;
        d.depth = volume ? 9 : 1;
        d.arrayLength = volume || type == MTLTextureTypeCube ? 1 : 2;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
        auto parent = [device newTextureWithDescriptor:d];
        if (cubeArray && !gm20b::answers::kThisKind.cubeArrays) {
            EXPECT_EQ(parent, nil);
            continue;
        }
        ASSERT_NE(parent, nil);
        auto format = [parent newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB];
        ASSERT_NE(format, nil);
        const NSUInteger firstSlice = volume || type == MTLTextureTypeCube ? 0
                                      : cubeArray                          ? 6
                                                                           : 1,
                         slices = cube ? 6 : 1;
        auto selected = [format newTextureViewWithPixelFormat:format.pixelFormat
                                                  textureType:volume ? MTLTextureType3D
                                                              : cube ? MTLTextureTypeCube
                                                                     : MTLTextureType2D
                                                       levels:NSMakeRange(1, volume ? 2 : 3)
                                                       slices:NSMakeRange(firstSlice, slices)];
        ASSERT_NE(selected, nil);
        const auto &chain = *((SwitchMetalTexture *)parent)->_raw.chain;
        const auto &c = chain.layout;
        std::vector<uint64_t> want;
        std::vector<uint32_t> widths, heights;
        for (NSUInteger layer = firstSlice; layer < firstSlice + slices; ++layer)
            for (uint32_t level = 2; level <= (volume ? 2u : 3u); ++level)
                for (uint32_t z = 0; z < (volume ? c.level[level].depth : 1u); ++z) {
                    want.push_back(
                        chain.va + c.level[level].offset +
                        (volume ? uint64_t(z) * c.level[level].rowBytes * c.level[level].rows : layer * c.arrayStride));
                    widths.push_back(c.level[level].rowBytes / c.bpp);
                    heights.push_back(c.level[level].height);
                }
        const size_t first = kernel_model::model.jobs.size();
        auto cb = [[device newCommandQueue] commandBuffer];
        auto e = [cb blitCommandEncoder];
        [e generateMipmapsForTexture:selected];
        [e endEncoding];
        EXPECT_EQ(((SwitchMetalCommandBuffer *)cb)->_pred0, 0u);
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
        auto *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
        ASSERT_TRUE([path isEqualToString:@"gm20b"]) << path.UTF8String;
        std::vector<uint64_t> addresses;
        std::vector<uint32_t> gotWidths, gotHeights, formats;
        uint64_t address = 0;
        for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
            const auto &words = kernel_model::model.jobs[j].words;
            std::vector<switch_gm20b_method> methods(words.size());
            const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
            ASSERT_GE(count, 0);
            for (long k = 0; k < count; ++k) {
                const auto &m = methods[k];
                if (m.subchannel != gm20b::SUBC_3D)
                    continue;
                if (m.method == NVB197_SET_COLOR_TARGET_A(0))
                    address = uint64_t(m.value) << 32;
                else if (m.method == NVB197_SET_COLOR_TARGET_B(0))
                    addresses.push_back(address | m.value);
                else if (m.method == NVB197_SET_COLOR_TARGET_WIDTH(0))
                    gotWidths.push_back(m.value);
                else if (m.method == NVB197_SET_COLOR_TARGET_HEIGHT(0))
                    gotHeights.push_back(m.value);
                else if (m.method == NVB197_SET_COLOR_TARGET_FORMAT(0) && m.value)
                    formats.push_back(m.value);
            }
        }
        EXPECT_EQ(addresses, want);
        EXPECT_EQ(gotWidths, widths);
        EXPECT_EQ(gotHeights, heights);
        EXPECT_EQ(formats, std::vector<uint32_t>(want.size(), 0xd6));
    }
}

}  // namespace
