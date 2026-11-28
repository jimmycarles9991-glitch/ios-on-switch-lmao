// SPDX-License-Identifier: MIT
// A private target made plain for want of compressed memory moves onto it on the submit worker (private_comp.h
// compressedFromPlain) while another thread records passes into it, binds it for sampling and copies out of it, through
// the facade over the kernel model. The worker alone writes the texture's memory fields; the recording reads none of
// them, which the thread-sanitized build (facade_compressed_move_tsan_test) holds to: any such read with no order to
// the move is its failure.
#include "scale_frames.h"

#include <gtest/gtest.h>
#include <objc/message.h>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

using scale_frames::opened;

constexpr int kRounds = 16;

uint64_t counted(id<MTLDevice> device, const char *selector, NSString *key)
{
    NSDictionary *d = ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)device, sel_registerName(selector));
    return [d[key] unsignedLongLongValue];
}

NSString *facts(id<MTLDevice> device, id<MTLTexture> texture)
{
    return ((NSString * (*)(id, SEL, id)) objc_msgSend)((id)device, sel_registerName("switchMetalProbeFacts:"),
                                                        texture);
}

id<MTLTexture> sharedTexture(id<MTLDevice> device, const scale_frames::Shape &shape)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:shape.width
                                                                                 height:shape.height
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:td];
}

TEST(GM20BFacadeCompressedMove, ARecordingThreadUsesATargetWhileTheWorkerMovesIt)
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoCompression;
    }
    ASSERT_TRUE(opened().device != nil);
    id<MTLDevice> device = opened().device;
    const scale_frames::Shape shape{4, 24, 1, 704, 392};
    const uint64_t made = counted(device, "switchMetalCompression", @"made");
    const auto target = scale_frames::targets(device, shape);
    ASSERT_TRUE([facts(device, target[0]) containsString:@"comptags=0"]) << "none of its size made: asked for";
    ASSERT_EQ(harness::compressionSettled(device), 1) << "the compression thread did not settle the target's ask once";
    ASSERT_GT(counted(device, "switchMetalCompression", @"made"), made) << "no compressed memory was made";
    const uint64_t moves = counted(device, "switchMetalCounters", @"private_compressed_moves");
    const std::vector<id<MTLTexture>> beside = {sharedTexture(device, shape)};
    id<MTLTexture> copy = sharedTexture(device, shape);
    id<MTLTexture> moved = target[0];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLRenderPipelineState> state = opened().state;
    id<MTLCommandBuffer> last = nil;
    // each command buffer committed with no wait, so the next one records while the worker submits it and moves the
    // target: nothing orders this thread's reads of the texture after the move but what the facade itself does
    std::thread recorder([&] {
        for (int round = 0; round < kRounds; round++) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            scale_frames::pass(cb, state, target, shape, 0, uint32_t(round));
            scale_frames::pass(cb, state, beside, shape, 0, uint32_t(round), nil, ^(id<MTLRenderCommandEncoder> e) {
                [e setFragmentTexture:moved atIndex:0];
            });
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:moved
                      sourceSlice:0
                      sourceLevel:0
                     sourceOrigin:MTLOriginMake(0, 0, 0)
                       sourceSize:MTLSizeMake(shape.width, shape.height, 1)
                        toTexture:copy
                 destinationSlice:0
                 destinationLevel:0
                destinationOrigin:MTLOriginMake(0, 0, 0)];
            [blit endEncoding];
            [cb commit];
            last = cb;
        }
    });
    recorder.join();
    [last waitUntilCompleted];
    EXPECT_EQ(last.status, MTLCommandBufferStatusCompleted) << last.error.description.UTF8String;
    EXPECT_TRUE([facts(device, moved) containsString:@"comptags=1"]) << facts(device, moved).UTF8String;
    EXPECT_EQ(counted(device, "switchMetalCounters", @"private_compressed_moves"), moves + 1);
}

// A private colour target binds the compressible kind of its texel size whatever its channels (NIL's nvc0_choose_pte_kind
// picks colour kinds by element size alone): the single-channel 32-bit formats C32_2CRA as BGRA8Unorm does
TEST(GM20BFacadeCompressedMove, ASingleChannelWordTargetTakesItsTexelSizesKind)
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoCompression;
    }
    ASSERT_TRUE(opened().device != nil);
    for (MTLPixelFormat format :
         {MTLPixelFormatR32Uint, MTLPixelFormatR32Sint, MTLPixelFormatR32Float, MTLPixelFormatBGRA8Unorm}) {
        MTLTextureDescriptor *td =
            [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:200 height:120 mipmapped:NO];
        td.storageMode = MTLStorageModePrivate;
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        EXPECT_EQ(privateCompressedKind(td), gm20b::kCompKindC32) << format;
    }
}

// A private Depth32Float texture made plain for want of ZF32_2CZ memory moves onto it on the submit worker while
// another thread records passes that clear, load and store it as their depth attachment: the move writes its memory
// fields under the device lock and the recording reads none of them (the thread-sanitized build's check)
TEST(GM20BFacadeCompressedMove, ARecordingThreadAttachesADepthTextureWhileTheWorkerMovesIt)
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoCompression;
    }
    ASSERT_TRUE(opened().device != nil);
    id<MTLDevice> device = opened().device;
    const uint64_t made = counted(device, "switchMetalCompression", @"made");
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                  width:200
                                                                                 height:120
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> depth = [device newTextureWithDescriptor:td];
    ASSERT_TRUE([facts(device, depth) containsString:@"comptags=0"]) << "none of its size made: asked for";
    ASSERT_EQ(harness::compressionSettled(device), 1);
    ASSERT_GT(counted(device, "switchMetalCompression", @"made"), made) << "no compressed memory was made";
    const auto target = scale_frames::targets(device, scale_frames::Shape{1, 4, 1, 200, 120});
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> last = nil;
    std::thread recorder([&] {
        for (int round = 0; round < kRounds; round++) {
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = target[0];
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            rp.depthAttachment.texture = depth;
            rp.depthAttachment.loadAction = round ? MTLLoadActionLoad : MTLLoadActionClear;
            rp.depthAttachment.storeAction = MTLStoreActionStore;
            rp.depthAttachment.clearDepth = 1.0;
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            [cb commit];
            last = cb;
        }
    });
    recorder.join();
    [last waitUntilCompleted];
    EXPECT_EQ(last.status, MTLCommandBufferStatusCompleted) << last.error.description.UTF8String;
    EXPECT_TRUE([facts(device, depth) containsString:@"comptags=1"]) << facts(device, depth).UTF8String;
}

size_t jobs()
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    return kernel_model::model.jobs.size();
}

id<MTLTexture> privateDepth(id<MTLDevice> device, MTLPixelFormat format, NSUInteger width, NSUInteger height)
{
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [device newTextureWithDescriptor:td];
}

// One command buffer through the submit worker, whose raw range submission first moves a texture that still wants memory
// made for it
void submitOne(id<MTLDevice> device)
{
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = sharedTexture(device, scale_frames::Shape{1, 1, 1, 64, 64});
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
}

// A whole-texture replaceRegion then getBytes, each the copy engine's job of its own (nothing else submits meanwhile)
void copiesMove(id<MTLTexture> depth, uint32_t bpp)
{
    const NSUInteger w = depth.width, h = depth.height;
    std::vector<uint8_t> written(w * h * bpp), read(written.size(), 0xab);
    for (size_t i = 0; i < written.size(); i++)
        written[i] = uint8_t(i * 37 + 11);
    const size_t before = jobs();
    [depth replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:written.data() bytesPerRow:w * bpp];
    const size_t replaced = jobs();
    EXPECT_GT(replaced, before) << "replaceRegion moved nothing";
    [depth getBytes:read.data() bytesPerRow:w * bpp fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    EXPECT_GT(jobs(), replaced) << "getBytes moved nothing";
}

// A private depth texture made plain for want of compressed memory takes CPU copies through the copy engine, as Apple's
// driver does with API validation off (HW Test metal.native.depth_cpu), and stays plain from then on: the memory made
// for it and a raw range after it move it no more
TEST(GM20BFacadeCompressedMove, ACpuCopyOfADepthTextureMadePlainMovesItsTexelsAndKeepsItPlain)
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoCompression;
    }
    ASSERT_TRUE(opened().device != nil);
    id<MTLDevice> device = opened().device;
    const std::pair<MTLPixelFormat, uint32_t> kFormats[] = {{MTLPixelFormatDepth32Float, 4},
                                                            {MTLPixelFormatDepth16Unorm, 2}};
    for (const auto &[format, bpp] : kFormats) {
        SCOPED_TRACE(testing::Message() << "format " << (unsigned long)format);
        id<MTLTexture> depth = privateDepth(device, format, 72, 40);
        ASSERT_TRUE([facts(device, depth) containsString:@"comptags=0"]) << "none of its size made: asked for";
        copiesMove(depth, bpp);
        ASSERT_EQ(harness::compressionSettled(device), 1);
        submitOne(device);
        EXPECT_TRUE([facts(device, depth) containsString:@"comptags=0"]) << facts(device, depth).UTF8String;
    }
}

// One that moved onto its compressed memory first takes them through the copy engine there (of a size past the 128 KiB
// granule, which the memory made for the plain ones above, and left ready, does not serve)
TEST(GM20BFacadeCompressedMove, ACpuCopyOfADepthTextureMovedOntoCompressedMemoryReachesIt)
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoCompression;
    }
    ASSERT_TRUE(opened().device != nil);
    id<MTLDevice> device = opened().device;
    id<MTLTexture> depth = privateDepth(device, MTLPixelFormatDepth32Float, 256, 160);
    ASSERT_TRUE([facts(device, depth) containsString:@"comptags=0"]) << "none of its size made: asked for";
    ASSERT_EQ(harness::compressionSettled(device), 1);
    submitOne(device);
    ASSERT_TRUE([facts(device, depth) containsString:@"comptags=1"]) << facts(device, depth).UTF8String;
    copiesMove(depth, 4);
}

// Nothing asked of the compression thread: the block runs at once, on the caller's thread, before the call returns
TEST(GM20BFacadeCompressedMove, NothingAskedSettlesAtOnceOnTheCallersThread)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLDevice> device = opened().device;
    ASSERT_EQ(harness::compressionSettled(device), 1);
    __block bool ran = false;
    NSString *refused = ((NSString * (*)(id, SEL, id))
                             objc_msgSend)((id)device, sel_registerName("switchMetalProbeCompressionSettled:"), ^{
        ran = true;
    });
    EXPECT_EQ(refused, nil);
    EXPECT_TRUE(ran) << "nothing was asked, yet the block did not run before the call returned";
}

}  // namespace
