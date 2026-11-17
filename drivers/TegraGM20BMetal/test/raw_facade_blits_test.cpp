// SPDX-License-Identifier: MIT
#include "raw_facade_harness.h"
#include "drivers/TegraGM20B/test/gm20b_ce_model.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "kits/SwitchGM20B/SwitchGM20B2D.h"
#include "kits/SwitchGM20B/SwitchGM20B3D.h"
#include "../raw/fx/test/twod_sim.h"
#include "../raw/runtime/raw_device_answers.h"

@interface NSObject (SwitchBlitFillTest)
- (void)fillTexture:(id<MTLTexture>)texture
              level:(NSUInteger)level
              slice:(NSUInteger)slice
             region:(MTLRegion)region
              color:(MTLClearColor)color;
@end

namespace {

id<MTLDevice> device;

class FacadeBlits : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        device = harness::open(harness::writePack(harness::packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                                  {{"raw.mip.mip_rgba8_srgb_frag", MTLPixelFormatRGBA8Unorm_sRGB},
                                                   {"raw.mip.mip_3d_rgba8_srgb_frag", MTLPixelFormatRGBA8Unorm_sRGB},
                                                   {"raw.fmt.fmt_mip_srgb8_frag", MTLPixelFormatR8Uint}}));
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
        const size_t first = kernel_model::model.jobs.size();
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
        auto *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchRawPath"));
        ASSERT_TRUE([path isEqualToString:@"raw"]) << path.UTF8String;
        Memory memory;
        for (const auto &[va, bind] : kernel_model::model.binds)
            if (auto *bytes = kernel_model::model.host(va, bind.second))
                memory.map(va, bytes, bind.second, false);
        CopyEngine engine(memory);
        engine.requireNonPipelined = false;
        twodsim::Sim twod;
        twod.host = [](uint64_t va, uint64_t bytes) {
            return kernel_model::model.host(va, bytes);
        };
        std::vector<uint32_t> init(raw::fx::twodInitWords());
        raw::Push push = {init.data(), init.data() + init.size()};
        raw::fx::emitTwodInit(push);
        ASSERT_TRUE(twod.run(init.data(), size_t(push.at - init.data()))) << twod.error();
        for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
            const auto &words = kernel_model::model.jobs[j].words;
            std::vector<switch_gm20b_method> methods(words.size());
            const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
            ASSERT_GE(count, 0);
            for (long k = 0; k < count; ++k) {
                if (methods[k].subchannel == raw::SUBC_COPY && methods[k].method >= 0x100)
                    engine.method(raw::SUBC_COPY, methods[k].method, methods[k].value);
                ASSERT_TRUE(twod.feed(methods[k].subchannel, methods[k].method, methods[k].value)) << twod.error();
            }
        }
        ASSERT_FALSE(engine.fault);
        if (empty)
            EXPECT_EQ(engine.launches, 0u);
        else
            EXPECT_GT(engine.launches, 0u);
        EXPECT_EQ(twod.launches, mipSteps);
    }
};

TEST_F(FacadeBlits, GenerateMipmapsLeavesOneLevelAndDepthStencilTexturesUnchanged)
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
        auto *raw = (RMTexture *)t;
        ASSERT_TRUE(raw->_rawOk);
        const auto range = (raw->_levelsOwner ?: raw)->_raw.storageRange();
        auto *bytes = kernel_model::model.host(range.va, range.bytes);
        ASSERT_NE(bytes, nullptr);
        std::vector<uint8_t> sentinel(range.bytes);
        for (size_t i = 0; i < sentinel.size(); ++i)
            sentinel[i] = uint8_t(i * 13 + i / 64);
        memcpy(bytes, sentinel.data(), sentinel.size());
        const int opens = harness::gOpens;
        auto cb = [[device newCommandQueue] commandBuffer];
        auto e = [cb blitCommandEncoder];
        const size_t recorded = ((RMCommandBuffer *)cb)->_plan.size();
        [e generateMipmapsForTexture:t];
        EXPECT_EQ(((RMCommandBuffer *)cb)->_plan.size(), recorded);
        EXPECT_EQ(((RMCommandBuffer *)cb)->_preds, 0u);
        EXPECT_FALSE(raw->_gpuWrites.load());
        [e endEncoding];
        execute(cb, 0, true);
        EXPECT_EQ(memcmp(bytes, sentinel.data(), sentinel.size()), 0);
        EXPECT_EQ(harness::gOpens, opens);
    }
}

TEST_F(FacadeBlits, GenerateMipmapsRejectsInvalidResourcesBeforeRecordingOrSubmitting)
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
        auto *raw = t == ordinary ? (RMTexture *)ordinary : nil;
        id saved = raw ? raw->_device : nil;
        if (raw)
            raw->_device = nil;
        const size_t jobs = kernel_model::model.jobs.size();
        const int opens = harness::gOpens;
        auto cb = [[device newCommandQueue] commandBuffer];
        auto e = [cb blitCommandEncoder];
        const size_t recorded = ((RMCommandBuffer *)cb)->_plan.size();
        [e generateMipmapsForTexture:t];
        if (raw)
            raw->_device = saved;
        EXPECT_EQ(((RMCommandBuffer *)cb)->_plan.size(), recorded);
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
        EXPECT_TRUE([cb.error.domain isEqualToString:MTLCommandBufferErrorDomain]);
        EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInvalidResource);
        EXPECT_EQ(kernel_model::model.jobs.size(), jobs);
        EXPECT_EQ(harness::gOpens, opens);
    }
}

TEST_F(FacadeBlits, GenerateMipmapsKeepsUpstreamAndVolumeFormatRefusals)
{
    auto t = texture();
    ASSERT_NE(t, nil);
    auto *raw = (RMTexture *)t;
    raw->_rawOk = false;
    raw->_pred = PredTexView;
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    [e generateMipmapsForTexture:t];
    raw->_rawOk = true;
    raw->_pred = 0;
    [e endEncoding];
    EXPECT_EQ(((RMCommandBuffer *)cb)->_preds, 1ull << PredTexView);
    EXPECT_FALSE(((RMCommandBuffer *)cb)->_segError.set());
    auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm_sRGB
                                                                 width:32
                                                                height:32
                                                             mipmapped:YES];
    d.textureType = MTLTextureType3D;
    d.depth = 4;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead;
    auto volume = [device newTextureWithDescriptor:d];
    ASSERT_NE(volume, nil);
    ASSERT_TRUE(rmFormatMips(uint32_t(volume.pixelFormat)));
    cb = [[device newCommandQueue] commandBuffer];
    e = [cb blitCommandEncoder];
    [e generateMipmapsForTexture:volume];
    [e endEncoding];
    EXPECT_EQ(((RMCommandBuffer *)cb)->_preds, 1ull << PredMipmapsFormat);
    EXPECT_FALSE(((RMCommandBuffer *)cb)->_segError.set());
}

TEST_F(FacadeBlits, GenerateMipmapsFailsAnAbsentNativeChainInternally)
{
    auto t = texture();
    ASSERT_NE(t, nil);
    auto *raw = (RMTexture *)t;
    auto *chain = raw->_raw.chain;
    ASSERT_NE(chain, nullptr);
    raw->_raw.chain = nullptr;
    const size_t jobs = kernel_model::model.jobs.size();
    const int opens = harness::gOpens;
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    const size_t recorded = ((RMCommandBuffer *)cb)->_plan.size();
    [e generateMipmapsForTexture:t];
    raw->_raw.chain = chain;
    EXPECT_EQ(((RMCommandBuffer *)cb)->_plan.size(), recorded);
    EXPECT_EQ(((RMCommandBuffer *)cb)->_preds, 0u);
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_TRUE([cb.error.domain isEqualToString:MTLCommandBufferErrorDomain]);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal);
    EXPECT_EQ(kernel_model::model.jobs.size(), jobs);
    EXPECT_EQ(harness::gOpens, opens);
}

TEST_F(FacadeBlits, GenerateMipmapsOfANestedViewKeepsItsParentNeighbors)
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
    EXPECT_EQ(((RMCommandBuffer *)cb)->_preds, 0u);
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

TEST_F(FacadeBlits, GenerateMipmapsUsesTheViewFormatAndParentRangeForArraysCubesAndVolumes)
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
        if (cubeArray && !raw::answers::kThisKind.cubeArrays) {
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
        const auto &chain = *((RMTexture *)parent)->_raw.chain;
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
        EXPECT_EQ(((RMCommandBuffer *)cb)->_preds & (1ull << PredMipmapsFormat), 0u);
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
        auto *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchRawPath"));
        ASSERT_TRUE([path isEqualToString:@"raw"]) << path.UTF8String;
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
                if (m.subchannel != raw::SUBC_3D)
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
    ASSERT_TRUE(((RMTexture *)alias)->_rawOk);
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
        ASSERT_TRUE(((RMTexture *)stencil)->_rawOk);
        const auto &root = ((RMTexture *)parent)->_raw;
        ASSERT_EQ(root.stencilTemp, nullptr);
        const auto storage = root.storageRange();
        auto *bytes = kernel_model::model.host(storage.va, storage.bytes);
        ASSERT_NE(bytes, nullptr);
        std::vector<uint8_t> expected(storage.bytes);
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = uint8_t(i * 17 + i / 31);
        memcpy(bytes, expected.data(), expected.size());
        raw::Texture level;
        ASSERT_TRUE(raw::chainLevelImage(root, 2, level));
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
    raw::Texture unrelated;
    uint64_t temporary = 0;
    __weak RMTexture *held = nil;
    @autoreleasepool {
        id<MTLTexture> parent = texture(MTLPixelFormatDepth32Float_Stencil8);
        ASSERT_NE(parent, nil);
        id<MTLTexture> first = [parent newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        id<MTLTexture> second = [parent newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
        ASSERT_NE(first, nil);
        ASSERT_NE(second, nil);
        __unsafe_unretained RMTexture *root = (RMTexture *)parent;
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
        EXPECT_EQ(((RMTexture *)first)->_raw.stencilTemp, nullptr);
        EXPECT_EQ(((RMTexture *)second)->_raw.stencilTemp, nullptr);
        temporary = root->_raw.stencilTemp->va;
        ASSERT_TRUE(gRM->raw->createTexture(32, 32, 30, unrelated));
        EXPECT_NE(unrelated.va, temporary);
        e = nil;
        cb = nil;
        first = nil;
        parent = nil;
        {
            RMTexture *retained = held;
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
    raw::Texture reused;
    ASSERT_TRUE(gRM->raw->createTexture(32, 32, 30, reused));
    EXPECT_EQ(reused.va, temporary);
    gRM->raw->destroyTexture(reused);
    gRM->raw->destroyTexture(unrelated);
}

TEST_F(FacadeBlits, BlitFillAdmitsArrayMipAndEmitsItsTarget)
{
    auto t = texture(MTLPixelFormatRGBA8Unorm, true);
    raw::Texture level;
    ASSERT_TRUE(raw::chainLevelImage(((RMTexture *)t)->_raw, 1, level));
    const uint64_t expected = level.va + level.layerBytes;
    const size_t first = kernel_model::model.jobs.size();
    auto cb = [[device newCommandQueue] commandBuffer];
    auto e = [cb blitCommandEncoder];
    [(id)e fillTexture:t level:1 slice:1 region:MTLRegionMake2D(3, 5, 7, 9) color:MTLClearColorMake(1, 0, 0, 1)];
    [e endEncoding];
    EXPECT_EQ(((RMCommandBuffer *)cb)->_preds & (1ull << PredBlitTexFill), 0u);
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    auto *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchRawPath"));
    EXPECT_TRUE([path isEqualToString:@"raw"]);
    uint64_t address = 0;
    unsigned fills = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const auto &words = kernel_model::model.jobs[j].words;
        std::vector<switch_gm20b_method> methods(words.size());
        const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
        ASSERT_GE(count, 0);
        for (long k = 0; k < count; ++k) {
            const auto &m = methods[k];
            if (m.subchannel != raw::SUBC_2D)
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

TEST_F(FacadeBlits, FillPartViewUsesTheParentLevel)
{
    auto t = texture();
    auto v = [t newTextureViewWithPixelFormat:t.pixelFormat
                                  textureType:MTLTextureType2D
                                       levels:NSMakeRange(1, 4)
                                       slices:NSMakeRange(0, 1)];
    ASSERT_NE(v, nil);
    ASSERT_TRUE(((RMTexture *)v)->_rawOk);
    raw::Texture level;
    ASSERT_TRUE(raw::chainLevelImage(((RMTexture *)t)->_raw, 2, level));
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
            if (m.subchannel != raw::SUBC_2D)
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
