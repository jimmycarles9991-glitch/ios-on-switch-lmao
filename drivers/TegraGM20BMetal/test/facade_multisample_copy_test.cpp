// SPDX-License-Identifier: MIT
#include "facade_harness.h"
#include "../samples.h"
#include "drivers/TegraGM20B/test/gm20b_ce_model.h"
#include "kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"

namespace {

id<MTLDevice> device;

class FacadeMultisampleCopies : public testing::Test {
protected:
    static void SetUpTestSuite()
    {
        device = harness::open(harness::writePack(harness::packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                                  {{gm20b::resolveState(70, 2), MTLPixelFormatRGBA8Unorm},
                                                   {gm20b::resolveState(70, 4), MTLPixelFormatRGBA8Unorm}}));
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

    static uint64_t offset(const gm20b::Texture &t, uint32_t x, uint32_t y, uint32_t sample, uint32_t byte)
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
        NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
        ASSERT_TRUE([path isEqualToString:@"gm20b"]) << path.UTF8String;
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
                if (methods[k].subchannel == gm20b::SUBC_COPY && methods[k].method >= 0x100)
                    engine.method(gm20b::SUBC_COPY, methods[k].method, methods[k].value);
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
        ASSERT_TRUE(((SwitchMetalTexture *)source)->_gm20bOk);
        ASSERT_TRUE(((SwitchMetalTexture *)destination)->_gm20bOk);
        const gm20b::Texture &s = ((SwitchMetalTexture *)source)->_raw, &d = ((SwitchMetalTexture *)destination)->_raw;
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

    // A copy between a multisampled colour texture and a buffer of `samples` samples, both ways, as an M4 Max lays the
    // buffer out: row y at offset + y * bytesPerRow holds texel (x, y)'s samples in order from the copy's origin (2x and
    // 4x, texels of 1 to 16 bytes, a part at a longer row and an offset)
    static void bufferCopy(MTLPixelFormat format, uint32_t samples, bool whole, bool intoTexture)
    {
        SCOPED_TRACE(testing::Message() << format << ", samples " << samples << ", whole " << whole << ", into "
                                        << intoTexture);
        kernel_model::model.unknownUsesRefused = true;
        auto *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:37 height:29 mipmapped:NO];
        d.textureType = MTLTextureType2DMultisample;
        d.sampleCount = samples;
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> texture = [device newTextureWithDescriptor:d];
        ASSERT_NE(texture, nil);
        const gm20b::Texture &t = ((SwitchMetalTexture *)texture)->_raw;
        auto queue = [device newCommandQueue];
        auto cleared = [queue commandBuffer];
        auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        [[cleared renderCommandEncoderWithDescriptor:pass] endEncoding];
        [cleared commit];
        [cleared waitUntilCompleted];
        ASSERT_EQ(cleared.status, MTLCommandBufferStatusCompleted);
        const auto range = t.storageRange();
        uint8_t *grid = kernel_model::model.host(range.va, range.bytes);
        ASSERT_NE(grid, nullptr);
        const uint32_t bpp = t.bpp, x0 = whole ? 0 : 9, y0 = whole ? 0 : 5, w = whole ? 37 : 17, h = whole ? 29 : 11;
        const uint32_t row = w * samples * bpp + (whole ? 0 : 3 * bpp), at = whole ? 0 : 4 * bpp,
                       length = at + row * h + 256;
        std::vector<uint8_t> image(range.bytes), rows(length);
        for (size_t i = 0; i < image.size(); ++i)
            image[i] = intoTexture ? 0xa5 : uint8_t(i * 13 + (i >> 9) * 7 + samples);
        for (size_t i = 0; i < rows.size(); ++i)
            rows[i] = intoTexture ? uint8_t(i * 29 + (i >> 7) * 3) : 0x22;
        memcpy(grid, image.data(), image.size());
        id<MTLBuffer> buffer =
            [device newBufferWithBytes:rows.data() length:length options:MTLResourceStorageModeShared];
        std::vector<uint8_t> wantImage = image, wantRows = rows;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                for (uint32_t sample = 0; sample < samples; ++sample)
                    for (uint32_t byte = 0; byte < bpp; ++byte) {
                        const uint64_t texel = offset(t, x0 + x, y0 + y, sample, byte),
                                       slot = at + uint64_t(y) * row + (uint64_t(x) * samples + sample) * bpp + byte;
                        ASSERT_LT(texel, image.size());
                        if (intoTexture)
                            wantImage[texel] = rows[slot];
                        else
                            wantRows[slot] = image[texel];
                    }
        auto cb = [queue commandBuffer];
        auto e = [cb blitCommandEncoder];
        if (intoTexture)
            [e copyFromBuffer:buffer
                       sourceOffset:at
                  sourceBytesPerRow:row
                sourceBytesPerImage:row * h
                         sourceSize:MTLSizeMake(w, h, 1)
                          toTexture:texture
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(x0, y0, 0)];
        else
            [e copyFromTexture:texture
                             sourceSlice:0
                             sourceLevel:0
                            sourceOrigin:MTLOriginMake(x0, y0, 0)
                              sourceSize:MTLSizeMake(w, h, 1)
                                toBuffer:buffer
                       destinationOffset:at
                  destinationBytesPerRow:row
                destinationBytesPerImage:row * h];
        [e endEncoding];
        ASSERT_NO_FATAL_FAILURE(execute(cb));
        EXPECT_EQ(memcmp(grid, wantImage.data(), wantImage.size()), 0);
        EXPECT_EQ(memcmp(buffer.contents, wantRows.data(), wantRows.size()), 0);
    }
};

TEST_F(FacadeMultisampleCopies, BufferCopiesHoldEachTexelsSamplesInOrder)
{
    struct Case {
        MTLPixelFormat format;
        uint32_t samples;
        bool whole;
    };

    for (const Case &c : {Case{MTLPixelFormatRGBA8Unorm, 4, true}, Case{MTLPixelFormatRGBA8Unorm, 4, false},
                          Case{MTLPixelFormatRGBA8Unorm, 2, false}, Case{MTLPixelFormatR8Unorm, 4, false},
                          Case{MTLPixelFormatRG8Unorm, 4, true}, Case{MTLPixelFormatRGBA16Float, 4, false},
                          Case{MTLPixelFormatRGBA16Float, 2, true}, Case{MTLPixelFormatRGBA32Float, 4, false}})
        for (bool into : {false, true}) {
            @autoreleasepool {
                bufferCopy(c.format, c.samples, c.whole, into);
            }
        }
}

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
                const gm20b::Texture &s = ((SwitchMetalTexture *)source)->_raw,
                                     &d = ((SwitchMetalTexture *)destination)->_raw;
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
                NSDictionary *before = harness::ask(device, "switchMetalFresh");
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
                NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(cb, sel_registerName("switchMetalPath"));
                ASSERT_TRUE([path isEqualToString:@"gm20b"]) << path.UTF8String;
                NSDictionary *after = harness::ask(device, "switchMetalFresh");
                EXPECT_EQ([after[@"filled"] unsignedLongLongValue] - [before[@"filled"] unsignedLongLongValue],
                          whole ? 0u : 1u);
                EXPECT_EQ([after[@"cleared"] unsignedLongLongValue] - [before[@"cleared"] unsignedLongLongValue],
                          whole ? 1u : 0u);
            }
        }
}
}  // namespace
