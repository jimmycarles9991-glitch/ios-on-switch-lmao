// SPDX-License-Identifier: MIT
#include "raw_facade_harness.h"
#include "../raw/runtime/raw_samples.h"
#include "drivers/TegraGM20B/test/gm20b_ce_model.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace {

id<MTLDevice> device;

class FacadeMultisampleCopies : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        device = harness::open(harness::writePack(harness::packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                                  {{raw::resolveState(70, 2), MTLPixelFormatRGBA8Unorm},
                                                   {raw::resolveState(70, 4), MTLPixelFormatRGBA8Unorm}}));
        ASSERT_NE(device, nil);
    }

    static id<MTLTexture> texture(MTLPixelFormat format, NSUInteger samples, NSUInteger width, NSUInteger height,
                                  MTLStorageMode storage = MTLStorageModePrivate)
    {
        auto *d =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
        d.textureType = samples > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
        d.sampleCount = samples;
        d.storageMode = storage;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
        return [device newTextureWithDescriptor:d];
    }

    static uint64_t offset(const raw::Texture &t, uint32_t x, uint32_t y, uint32_t sample, uint32_t byte)
    {
        const uint32_t px = (t.samples > 1 ? 2 : 1) * x + (sample & 1),
                       py = (t.samples == 4 ? 2 : 1) * y + (sample >> 1);
        return tgm_layout_offset_of(t.pitch, t.blockHeight, px * t.bpp + byte, py);
    }

    static void execute(id<MTLCommandBuffer> cb)
    {
        const size_t first = kernel_model::model.jobs.size();
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
        NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchRawPath"));
        ASSERT_TRUE([path isEqualToString:@"raw"]) << path.UTF8String;
        Memory memory;
        for (const auto &[va, bind] : kernel_model::model.binds)
            if (auto *bytes = kernel_model::model.host(va, bind.second))
                memory.map(va, bytes, bind.second, false);
        CopyEngine engine(memory);
        engine.requireNonPipelined = false;
        for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
            const auto &words = kernel_model::model.jobs[j].words;
            std::vector<switch_gm20b_method> methods(words.size());
            const long count = switch_gm20b_push_decode(words.data(), words.size(), methods.data(), methods.size());
            ASSERT_GE(count, 0);
            for (long k = 0; k < count; ++k)
                if (methods[k].subchannel == raw::SUBC_COPY && methods[k].method >= 0x100)
                    engine.method(raw::SUBC_COPY, methods[k].method, methods[k].value);
        }
        ASSERT_FALSE(engine.fault);
        EXPECT_GT(engine.launches, 0u);
    }

    static void copy(MTLPixelFormat format, uint32_t samples, bool whole, bool stencil = false)
    {
        SCOPED_TRACE(testing::Message() << format << ", samples " << samples << ", whole " << whole << ", stencil "
                                        << stencil);
        kernel_model::model.unknownUsesRefused = true;
        auto source = texture(format, samples, 37, 29);
        auto destination = texture(format, samples, whole ? 37 : 43, whole ? 29 : 37);
        ASSERT_NE(source, nil);
        ASSERT_NE(destination, nil);
        ASSERT_TRUE(((RMTexture *)source)->_rawOk);
        ASSERT_TRUE(((RMTexture *)destination)->_rawOk);
        const raw::Texture &s = ((RMTexture *)source)->_raw, &d = ((RMTexture *)destination)->_raw;
        ASSERT_TRUE(s.blockLinear && d.blockLinear);
        ASSERT_EQ(s.samples, samples);
        ASSERT_EQ(d.samples, samples);
        auto queue = [device newCommandQueue];
        if (!s.zeta) {
            auto initialized = [queue commandBuffer];
            for (id<MTLTexture> t : {source, destination}) {
                auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = t;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                auto e = [initialized renderCommandEncoderWithDescriptor:pass];
                ASSERT_NE(e, nil);
                [e endEncoding];
            }
            [initialized commit];
            [initialized waitUntilCompleted];
            ASSERT_EQ(initialized.status, MTLCommandBufferStatusCompleted);
        }
        const auto sr = s.storageRange(), dr = d.storageRange();
        uint8_t *from = kernel_model::model.host(sr.va, sr.bytes), *to = kernel_model::model.host(dr.va, dr.bytes);
        ASSERT_NE(from, nullptr);
        ASSERT_NE(to, nullptr);
        std::vector<uint8_t> sourceBytes(sr.bytes), expected(dr.bytes, 0xa5);
        for (size_t i = 0; i < sourceBytes.size(); ++i)
            sourceBytes[i] = uint8_t(i * 17 + (i >> 8) * 31 + samples * 7);
        memcpy(from, sourceBytes.data(), sourceBytes.size());
        memset(to, 0xa5, dr.bytes);
        const uint32_t sx = whole ? 0 : 17, sy = whole ? 0 : 7, dx = whole ? 0 : 14, dy = whole ? 0 : 11,
                       width = whole ? 37 : 19, height = whole ? 29 : 13;
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x)
                for (uint32_t sample = 0; sample < samples; ++sample)
                    for (uint32_t byte = stencil ? 4 : 0; byte < (stencil ? 5 : s.bpp); ++byte) {
                        const uint64_t a = offset(s, sx + x, sy + y, sample, byte),
                                       b = offset(d, dx + x, dy + y, sample, byte);
                        ASSERT_LT(a, sr.bytes);
                        ASSERT_LT(b, dr.bytes);
                        expected[b] = sourceBytes[a];
                    }
        id<MTLTexture> input = source, target = destination;
        if (stencil) {
            input = [source newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
            target = [destination newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8];
            ASSERT_NE(input, nil);
            ASSERT_NE(target, nil);
        } else if (whole) {
            target = [destination newTextureViewWithPixelFormat:format];
            ASSERT_NE(target, nil);
        }
        auto cb = [queue commandBuffer];
        auto e = [cb blitCommandEncoder];
        if (whole)
            [e copyFromTexture:input toTexture:target];
        else
            [e copyFromTexture:input
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(sx, sy, 0)
                       sourceSize:MTLSizeMake(width, height, 1)
                        toTexture:target
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(dx, dy, 0)];
        [e endEncoding];
        execute(cb);
        // D32S8 exposes bytes 0–4; bytes 5–7 are padding.
        if (stencil)
            for (uint32_t y = 0; y < height; ++y)
                for (uint32_t x = 0; x < width; ++x)
                    for (uint32_t sample = 0; sample < samples; ++sample)
                        for (uint32_t byte = 5; byte < 8; ++byte) {
                            const uint64_t at = offset(d, dx + x, dy + y, sample, byte);
                            expected[at] = to[at];
                        }
        EXPECT_EQ(memcmp(from, sourceBytes.data(), sourceBytes.size()), 0);
        EXPECT_EQ(memcmp(to, expected.data(), expected.size()), 0);
    }
};

TEST_F(FacadeMultisampleCopies, PartialCopiesPreserveEverySampleAndNeighbor)
{
    for (uint32_t samples : {2u, 4u})
        for (MTLPixelFormat format :
             {MTLPixelFormatR8Unorm, MTLPixelFormatRG8Unorm, MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA16Float,
              MTLPixelFormatRGBA32Float, MTLPixelFormatDepth16Unorm, MTLPixelFormatDepth32Float, MTLPixelFormatStencil8,
              MTLPixelFormatDepth32Float_Stencil8}) {
            @autoreleasepool {
                copy(format, samples, false);
            }
        }
}

TEST_F(FacadeMultisampleCopies, WholeCopyThroughAViewKeepsSampleOrder)
{
    for (uint32_t samples : {2u, 4u}) {
        @autoreleasepool {
            copy(MTLPixelFormatRGBA16Float, samples, true);
        }
    }
}

TEST_F(FacadeMultisampleCopies, StencilViewsPreserveDestinationDepth)
{
    for (uint32_t samples : {1u, 2u, 4u}) {
        @autoreleasepool {
            copy(MTLPixelFormatDepth32Float_Stencil8, samples, false, true);
        }
    }
}

TEST_F(FacadeMultisampleCopies, FirstUseInitializesEverySampleAndPreservesPadding)
{
    for (uint32_t samples : {1u, 2u, 4u})
        for (MTLPixelFormat format : {MTLPixelFormatR8Unorm, MTLPixelFormatRG8Unorm, MTLPixelFormatRGBA8Unorm,
                                      MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA32Float}) {
            @autoreleasepool {
                SCOPED_TRACE(testing::Message() << format << ", samples " << samples);
                auto source = texture(format, samples, 37, 29), destination = texture(format, samples, 37, 29);
                ASSERT_NE(source, nil);
                ASSERT_NE(destination, nil);
                const raw::Texture &s = ((RMTexture *)source)->_raw, &d = ((RMTexture *)destination)->_raw;
                const auto sr = s.storageRange(), dr = d.storageRange();
                uint8_t *from = kernel_model::model.host(sr.va, sr.bytes),
                        *to = kernel_model::model.host(dr.va, dr.bytes);
                ASSERT_NE(from, nullptr);
                ASSERT_NE(to, nullptr);
                memset(from, 0xa5, sr.bytes);
                memset(to, 0xa5, dr.bytes);
                std::vector<uint8_t> expectedSource(sr.bytes, 0xa5), expectedDestination(dr.bytes, 0xa5);
                for (uint32_t y = 0; y < 29; ++y)
                    for (uint32_t x = 0; x < 37; ++x)
                        for (uint32_t sample = 0; sample < samples; ++sample)
                            for (uint32_t byte = 0; byte < s.bpp; ++byte) {
                                expectedSource.at(offset(s, x, y, sample, byte)) = 0;
                                expectedDestination.at(offset(d, x, y, sample, byte)) = 0;
                            }
                auto cb = [[device newCommandQueue] commandBuffer];
                auto e = [cb blitCommandEncoder];
                auto view = [source newTextureViewWithPixelFormat:format];
                auto nested = [view newTextureViewWithPixelFormat:format];
                ASSERT_NE(nested, nil);
                [e copyFromTexture:nested toTexture:destination];
                [e endEncoding];
                execute(cb);
                EXPECT_EQ(memcmp(from, expectedSource.data(), expectedSource.size()), 0);
                EXPECT_EQ(memcmp(to, expectedDestination.data(), expectedDestination.size()), 0);
            }
        }
}

TEST_F(FacadeMultisampleCopies, AWholeClearAndStoredResolveTakesNoFill)
{
    for (uint32_t samples : {2u, 4u})
        for (bool whole : {false, true}) {
            @autoreleasepool {
                SCOPED_TRACE(testing::Message() << "samples " << samples << ", whole " << whole);
                auto source = texture(MTLPixelFormatRGBA8Unorm, samples, 37, 29);
                auto resolved = texture(MTLPixelFormatRGBA8Unorm, 1, 37, 29, MTLStorageModeShared);
                ASSERT_NE(source, nil);
                ASSERT_NE(resolved, nil);
                NSDictionary *before = harness::ask(device, "switchRawFresh");
                ASSERT_NE(before[@"filled"], nil);
                auto cb = [[device newCommandQueue] commandBuffer];
                auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = source;
                pass.colorAttachments[0].resolveTexture = resolved;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStoreAndMultisampleResolve;
                if (!whole) {
                    pass.renderTargetWidth = 8;
                    pass.renderTargetHeight = 8;
                }
                auto e = [cb renderCommandEncoderWithDescriptor:pass];
                ASSERT_NE(e, nil);
                [e endEncoding];
                [cb commit];
                [cb waitUntilCompleted];
                ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
                NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchRawPath"));
                ASSERT_TRUE([path isEqualToString:@"raw"]) << path.UTF8String;
                NSDictionary *after = harness::ask(device, "switchRawFresh");
                EXPECT_EQ([after[@"filled"] unsignedLongLongValue] - [before[@"filled"] unsignedLongLongValue],
                          whole ? 0u : 1u);
                EXPECT_EQ([after[@"cleared"] unsignedLongLongValue] - [before[@"cleared"] unsignedLongLongValue],
                          whole ? 1u : 0u);
            }
        }
}
}  // namespace
