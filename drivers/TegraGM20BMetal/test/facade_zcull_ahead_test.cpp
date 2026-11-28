// SPDX-License-Identifier: MIT
// A depth's zcull region kept from the pass that stores it to a later pass that loads it (zcull_planes.h), through the
// facade over the kernel model with the GPU's zcull layout: the submit worker encodes the storing pass ahead of the
// commit (TegraGM20BMetalPipeline.mm), before the loading pass is recorded, as HW Test gpu.gm20b.zcull's `loaded` arm
// records them, and the loading pass loads the region all the same, in the storing pass's job or in the next range's.
#include "scale_frames.h"
#include "TegraGM20BZcullABI.h"
#include "../TegraGM20BMetalInternal.h"

#include <gtest/gtest.h>
#include <chrono>
#include <cstdio>
#include <thread>

namespace {

using scale_frames::opened;

constexpr TegraGM20BAbiZcullInfo kX1Zcull = {32, 32, 1024, 2048, 32, 32, 192, 16, 32, 64, 0x1500, 0x1000};

const scale_frames::Shape kShape{4, 24, 4};

const scale_frames::Opened &openedWithZcull()
{
    static const bool answered = [] {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.otherMethod = [](uint32_t method, void *output) -> kern_return_t {
            if (method != TegraGM20BMethodZcullInfo)
                return kIOReturnUnsupported;
            *static_cast<TegraGM20BAbiZcullInfo *>(output) = kX1Zcull;
            return KERN_SUCCESS;
        };
        return true;
    }();
    (void)answered;
    return opened();
}

id<MTLTexture> depthTexture()
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                 width:kShape.width
                                                                                height:kShape.height
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    return [openedWithZcull().device newTextureWithDescriptor:d];
}

void depthPass(id<MTLCommandBuffer> cb, id<MTLTexture> colour, id<MTLTexture> depth, bool first)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = colour;
    d.colorAttachments[0].loadAction = MTLLoadActionLoad;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    d.depthAttachment.texture = depth;
    d.depthAttachment.loadAction = first ? MTLLoadActionClear : MTLLoadActionLoad;
    d.depthAttachment.clearDepth = 1;
    d.depthAttachment.storeAction = MTLStoreActionStore;
    [[cb renderCommandEncoderWithDescriptor:d] endEncoding];
}

long long counter(NSString *name)
{
    return [harness::counters(openedWithZcull().device)[name] longLongValue];
}

struct Moved {
    long long passes, loads, fused, carried;
    int pipelined;
    uint32_t stopped, givenBack;
    MTLCommandBufferStatus status;
};

// Core Animation shaped passes enough for the worker to be handed the command buffer, a pass that clears and stores a
// depth, what `between` records, more of those passes once the depth pass is not the last recorded, and once the worker
// has encoded all that ahead, a pass that loads the depth: the zcull counters it moved
Moved aheadPair(void (^between)(id<MTLCommandBuffer>, id<MTLTexture>), bool apart)
{
    id<MTLCommandQueue> queue = [openedWithZcull().device newCommandQueue];
    const auto into = scale_frames::targets(openedWithZcull().device, kShape);
    id<MTLTexture> depth = depthTexture();
    EXPECT_TRUE(depth != nil);
    const long long passes = counter(@"zcull_passes"), loads = counter(@"zcull_loads"),
                    fused = counter(@"fused_passes"), carried = counter(@"zcull_carried");
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < kShape.passes; p++) {
        scale_frames::pass(cb, openedWithZcull().state, into, kShape, p, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    depthPass(cb, into[0], depth, true);
    if (between)
        between(cb, depth);
    if (apart)
        for (int p = 0; p < kShape.passes; p++)
            scale_frames::pass(cb, openedWithZcull().state, into, kShape, p, 1);
    else
        depthPass(cb, into[0], depth, false);
    EXPECT_EQ(scale_frames::waitAhead(cb), 0u);
    if (apart)
        depthPass(cb, into[0], depth, false);
    else
        for (int p = 0; p < kShape.passes; p++)
            scale_frames::pass(cb, openedWithZcull().state, into, kShape, p, 1);
    [cb commit];
    [cb waitUntilCompleted];
    NSDictionary *p = ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)cb, sel_registerName("switchMetalPipelined"));
    std::printf("zcull_ahead %s: passes=%lld loads=%lld carried=%lld pipelined=%d stopped=0x%x given_back=0x%x\n",
                testing::UnitTest::GetInstance()->current_test_info()->name(), counter(@"zcull_passes") - passes,
                counter(@"zcull_loads") - loads, counter(@"zcull_carried") - carried, [p[@"pipelined"] intValue],
                [p[@"stopped"] unsignedIntValue], [p[@"given_back"] unsignedIntValue]);
    return {counter(@"zcull_passes") - passes,   counter(@"zcull_loads") - loads,
            counter(@"fused_passes") - fused,    counter(@"zcull_carried") - carried,
            [p[@"pipelined"] intValue],          [p[@"stopped"] unsignedIntValue],
            [p[@"given_back"] unsignedIntValue], cb.status};
}

// The pass after a blit that writes another buffer loads the region the pass encoded ahead before it stored, in the
// storing pass's job or, where the commit cut the range at the encode ahead's end, in the next range's
TEST(GM20BFacadeZcullAhead, APassAfterABlitLoadsTheRegionAPassEncodedAheadStored)
{
    ASSERT_TRUE(openedWithZcull().device != nil);
    id<MTLBuffer> other = [openedWithZcull().device newBufferWithLength:256 options:MTLResourceStorageModeShared];
    const Moved m = aheadPair(
        ^(id<MTLCommandBuffer> cb, id<MTLTexture>) {
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit fillBuffer:other range:NSMakeRange(0, 256) value:0];
            [blit endEncoding];
        },
        true);
    EXPECT_EQ(m.status, MTLCommandBufferStatusCompleted);
    EXPECT_GE(m.pipelined, kShape.passes + 2);
    EXPECT_EQ(m.passes, 2);
    EXPECT_EQ(m.loads, 1);
}

// A blit that copies into the depth between the store and the load leaves the stored region stale: the loading pass takes
// none, whatever was encoded ahead
TEST(GM20BFacadeZcullAhead, ACopyIntoTheDepthBetweenLeavesTheLoadingPassNoRegion)
{
    ASSERT_TRUE(openedWithZcull().device != nil);
    id<MTLTexture> source = depthTexture();
    ASSERT_TRUE(source != nil);
    const Moved m = aheadPair(
        ^(id<MTLCommandBuffer> cb, id<MTLTexture> depth) {
            id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
            [blit copyFromTexture:source toTexture:depth];
            [blit endEncoding];
        },
        true);
    EXPECT_EQ(m.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(m.passes, 1);
    EXPECT_EQ(m.loads, 0);
}

// A pass between the store and the load that binds a texture of rows 48 bytes apart, sampled through a pool image the
// commit's encode refreshes (VetoShadows), stops the encode ahead there: the commit cuts the range at that stop, keeps
// what was encoded ahead, and the next range's job loads the region the storing pass's job kept for it
TEST(GM20BFacadeZcullAhead, AStopBetweenTheStoreAndTheLoadCarriesTheRegionIntoTheNextRange)
{
    ASSERT_TRUE(openedWithZcull().device != nil);
    id<MTLBuffer> rows = [openedWithZcull().device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:12
                                                                                height:8
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> sampled = [rows newTextureWithDescriptor:d offset:48 bytesPerRow:48];
    ASSERT_TRUE(sampled != nil && ((SwitchMetalTexture *)sampled)->_raw.sampled != nullptr);
    id<MTLTexture> colour = scale_frames::targets(openedWithZcull().device, kShape)[0];
    const Moved m = aheadPair(
        ^(id<MTLCommandBuffer> cb, id<MTLTexture>) {
            MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
            pass.colorAttachments[0].texture = colour;
            pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
            pass.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
            [e setFragmentTexture:sampled atIndex:0];
            [e endEncoding];
        },
        true);
    EXPECT_EQ(m.status, MTLCommandBufferStatusCompleted);
    EXPECT_NE(m.stopped & uint32_t(VetoShadows), 0u);
    EXPECT_EQ(m.givenBack, 0u) << "the encode ahead is kept";
    EXPECT_GE(m.pipelined, kShape.passes + 1);
    EXPECT_EQ(m.passes, 2);
    EXPECT_EQ(m.loads, 1);
    EXPECT_EQ(m.carried, 1);
}

// A compute encoder between the store and the load halts the encode ahead before it (VetoOps): the same carry, past a
// depth prepass, a compute pass and the main pass that loads the depth
TEST(GM20BFacadeZcullAhead, AComputeEncoderBetweenTheStoreAndTheLoadCarriesTheRegion)
{
    ASSERT_TRUE(openedWithZcull().device != nil);
    const Moved m = aheadPair(
        ^(id<MTLCommandBuffer> cb, id<MTLTexture>) {
            [[cb computeCommandEncoder] endEncoding];
        },
        true);
    EXPECT_EQ(m.status, MTLCommandBufferStatusCompleted);
    EXPECT_NE(m.stopped & uint32_t(VetoOps), 0u);
    EXPECT_EQ(m.givenBack, 0u);
    EXPECT_GE(m.pipelined, kShape.passes + 1);
    EXPECT_EQ(m.passes, 2);
    EXPECT_EQ(m.loads, 1);
    EXPECT_EQ(m.carried, 1);
}

// Encoded at its commit and cut into chunks (splitCommittedRange), a range whose depth is stored in the first chunk and
// loaded in the second carries the region across the cut
TEST(GM20BFacadeZcullAhead, AChunkCutBetweenTheStoreAndTheLoadCarriesTheRegion)
{
    ASSERT_TRUE(openedWithZcull().device != nil);
    id<MTLCommandQueue> queue = [openedWithZcull().device newCommandQueue];
    const scale_frames::Shape shape{12, 24, 4};
    const auto into = scale_frames::targets(openedWithZcull().device, shape);
    id<MTLTexture> depth = depthTexture();
    ASSERT_TRUE(depth != nil);
    const long long loads = counter(@"zcull_loads"), carried = counter(@"zcull_carried");
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    // first, so the encode ahead stops where it starts and is given back: the range is encoded at its commit
    [[cb computeCommandEncoder] endEncoding];
    depthPass(cb, into[0], depth, true);
    for (int p = 0; p < shape.passes; p++)
        scale_frames::pass(cb, openedWithZcull().state, into, shape, p, 0);
    depthPass(cb, into[0], depth, false);
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(counter(@"zcull_loads") - loads, 1);
    EXPECT_EQ(counter(@"zcull_carried") - carried, 1) << "the load in a job of its own";
}

// HW Test gpu.gm20b.zcull's `carried` arm as it records: a compute encoder first, the depth stored, ten clear-only passes
// of one 8x8 target, the depth loaded; encoded at the commit, the range is cut between them and the region carried
TEST(GM20BFacadeZcullAhead, TheCarriedArmsShapeCarriesTheRegion)
{
    ASSERT_TRUE(openedWithZcull().device != nil);
    id<MTLCommandQueue> queue = [openedWithZcull().device newCommandQueue];
    id<MTLTexture> colour = scale_frames::targets(openedWithZcull().device, kShape)[0];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:8
                                                                                height:8
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget;
    d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> filler = [openedWithZcull().device newTextureWithDescriptor:d];
    id<MTLTexture> depth = depthTexture();
    ASSERT_TRUE(filler != nil && depth != nil);
    const long long loads = counter(@"zcull_loads"), carried = counter(@"zcull_carried");
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    [[cb computeCommandEncoder] endEncoding];
    depthPass(cb, colour, depth, true);
    for (int k = 0; k < 10; k++) {
        MTLRenderPassDescriptor *clear = [MTLRenderPassDescriptor renderPassDescriptor];
        clear.colorAttachments[0].texture = filler;
        clear.colorAttachments[0].loadAction = MTLLoadActionClear;
        clear.colorAttachments[0].storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:clear] endEncoding];
    }
    depthPass(cb, colour, depth, false);
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(counter(@"zcull_loads") - loads, 1);
    EXPECT_EQ(counter(@"zcull_carried") - carried, 1) << "the load in a job of its own";
}

// The loading pass right after the storing one, both encoded ahead: fused into one pass that keeps its region (HW Test
// gpu.gm20b.zcull's `fused` arm); a change to storing and loading it shows here
TEST(GM20BFacadeZcullAhead, AnAdjacentLoadingPassEncodedAheadKeepsTheRegion)
{
    ASSERT_TRUE(openedWithZcull().device != nil);
    const Moved m = aheadPair(nil, false);
    EXPECT_EQ(m.status, MTLCommandBufferStatusCompleted);
    EXPECT_GE(m.pipelined, kShape.passes + 2);
    EXPECT_GE(m.fused, 1) << "the facade fuses the pair";
    EXPECT_EQ(m.passes, 1) << "one pass, its region kept across the fused draws";
    EXPECT_EQ(m.loads, 0);
}

}  // namespace
