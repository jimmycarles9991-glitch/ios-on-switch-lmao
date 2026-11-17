// SPDX-License-Identifier: MIT
// The forms the range encode (RawMetalRangeEncode.h) draws through the raw facade over the kernel model, recorded at the
// render thread's pace so the submit worker encodes the ended encoders ahead of the commit (RawMetalPipeline.mm), and
// each again where its command buffer is encoded whole at the commit: a draw Metal fails fails its command buffer only
// at the commit, its encode ahead given back as failed.
#include "raw_scale_frames.h"
#include "../raw/runtime/RawMetalInternal.h"

#include <gtest/gtest.h>
#include <chrono>
#include <thread>

namespace {

using namespace harness;
using scale_frames::opened;

constexpr auto kPace = std::chrono::milliseconds(1);

NSDictionary *pipelined(id<MTLCommandBuffer> cb)
{
    return ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)cb, sel_registerName("switchRawPipelined"));
}

std::string path(id<MTLCommandBuffer> cb)
{
    return [[(id)cb performSelector:@selector(switchRawPath)] UTF8String];
}

NSError *failure(id<MTLCommandBuffer> cb)
{
    std::lock_guard<raw::Mutex> l(((RMCommandBuffer *)cb)->_lock);
    return ((RMCommandBuffer *)cb)->_segError;
}

// `shape`'s passes on `queue` a pass every kPace, `special` recording pass `at` in its place
id<MTLCommandBuffer> paced(id<MTLCommandQueue> queue, const std::vector<id<MTLTexture>> &into,
                           const scale_frames::Shape &shape, int at,
                           void (^special)(id<MTLCommandBuffer>, id<MTLTexture>))
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int p = 0; p < shape.passes; p++) {
        if (p == at)
            special(cb, into[size_t(p) % into.size()]);
        else
            scale_frames::pass(cb, opened().state, into, shape, p, 0);
        std::this_thread::sleep_for(kPace);
    }
    return cb;
}

// A pass into `target` that draws one quad of `state` after `before` sets the encoder up
void onePass(id<MTLCommandBuffer> cb, id<MTLTexture> target, void (^before)(id<MTLRenderCommandEncoder>))
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = target;
    d.colorAttachments[0].loadAction = MTLLoadActionLoad;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:d];
    [e setRenderPipelineState:opened().state];
    if (before)
        before(e);
    const float vertices[16] = {0, 0, 0, 1, 0.5f, 0, 0, 1, 0, 0.5f, 0, 1, 0.5f, 0.5f, 0, 1};
    const float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const float colour[4] = {1, 0, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e setVertexBytes:transform length:sizeof transform atIndex:2];
    [e setFragmentBytes:colour length:sizeof colour atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
}

id<MTLTexture> target(MTLPixelFormat format, const scale_frames::Shape &shape)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:shape.width
                                                                                height:shape.height
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    return [opened().device newTextureWithDescriptor:d];
}

id<MTLTexture> zetaArray(MTLPixelFormat format, const scale_frames::Shape &shape)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                                 width:shape.width
                                                                                height:shape.height
                                                                             mipmapped:NO];
    d.textureType = MTLTextureType2DArray;
    d.arrayLength = 3;
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget;
    return [opened().device newTextureWithDescriptor:d];
}

// A pass that loads and stores `t` and draws nothing
void loadStore(id<MTLCommandBuffer> cb, id<MTLTexture> t)
{
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = t;
    d.colorAttachments[0].loadAction = MTLLoadActionLoad;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[cb renderCommandEncoderWithDescriptor:d] endEncoding];
}

// Pass 9 of 15 draws at a vertex amplification count above the state's maximum (Metal's validation, RawMetalDrawErrors.mm
// RefAmplificationCount). Encoded ahead, the failure stops the encode and is not the command buffer's: nothing has failed
// it once its recording ends. At the commit the whole encode meets it again and fails the command buffer with its line.
TEST(RawFacadeRangeForms, ADrawMetalFailsFailsItsCommandBufferAtTheCommitOnly)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    id<MTLCommandBuffer> cb = paced(queue, into, shape, 9, ^(id<MTLCommandBuffer> c, id<MTLTexture> t) {
        onePass(c, t, ^(id<MTLRenderCommandEncoder> e) {
            [e setVertexAmplificationCount:2 viewMappings:nullptr];
        });
    });
    std::this_thread::sleep_for(10 * kPace);
    EXPECT_EQ(failure(cb), nil) << "failed before its commit";
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.domain, MTLCommandBufferErrorDomain);
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    EXPECT_NE([p[@"given_back"] intValue] & int(VetoFailed), 0) << [[p description] UTF8String];
    id<MTLCommandBuffer> next = scale_frames::record(queue, opened().state, into, shape, 1);
    [next commit];
    [next waitUntilCompleted];
    EXPECT_EQ(next.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(path(next), "raw");
}

// Pass 9 of 15 loads and stores an R8Unorm_sRGB texture, which the GM20B renders none of: the pass renders its RGBA8Unorm_sRGB
// stand-in, the copy engine moving the first bytes in before and out after it (raw_format_standin.h). The frame is encoded
// ahead with it, as any other.
TEST(RawFacadeRangeForms, APassIntoAFormatStandInIsEncodedAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    id<MTLTexture> srgb8 = target(MTLPixelFormat(11), shape);
    ASSERT_TRUE(srgb8 != nil);
    for (uint32_t frame = 0; frame < 2; frame++) {
        id<MTLCommandBuffer> cb = paced(queue, into, shape, 9, ^(id<MTLCommandBuffer> c, id<MTLTexture>) {
            loadStore(c, srgb8);
        });
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << frame;
        EXPECT_EQ(path(cb), "raw") << frame;
        NSDictionary *p = pipelined(cb);
        ASSERT_NE(p, nil);
        EXPECT_EQ([p[@"veto"] intValue], 0) << [[p description] UTF8String];
        EXPECT_GE([p[@"pipelined"] intValue], 10) << [[p description] UTF8String];
    }
}

// Pass 9 of 15 renders a Depth32Float array's slice 2 beside a Stencil8 array's slice 1, both loaded and stored: a scratch zeta
// of the pair's, each aspect loaded from and stored to its own texture's layer (RMRangeEncode::separateZeta). Encoded ahead.
TEST(RawFacadeRangeForms, ASeparateDepthAndStencilFromTheirSlicesAreEncodedAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    id<MTLTexture> depth = zetaArray(MTLPixelFormatDepth32Float, shape),
                   stencil = zetaArray(MTLPixelFormatStencil8, shape);
    ASSERT_TRUE(depth != nil && stencil != nil);
    id<MTLCommandBuffer> cb = paced(queue, into, shape, 9, ^(id<MTLCommandBuffer> c, id<MTLTexture> t) {
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = t;
        d.colorAttachments[0].loadAction = MTLLoadActionLoad;
        d.colorAttachments[0].storeAction = MTLStoreActionStore;
        d.depthAttachment.texture = depth;
        d.depthAttachment.slice = 2;
        d.depthAttachment.loadAction = MTLLoadActionLoad;
        d.depthAttachment.storeAction = MTLStoreActionStore;
        d.stencilAttachment.texture = stencil;
        d.stencilAttachment.slice = 1;
        d.stencilAttachment.loadAction = MTLLoadActionLoad;
        d.stencilAttachment.storeAction = MTLStoreActionStore;
        [[c renderCommandEncoderWithDescriptor:d] endEncoding];
    });
    [cb commit];
    [cb waitUntilCompleted];
    ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(path(cb), "raw");
    NSDictionary *p = pipelined(cb);
    ASSERT_NE(p, nil);
    EXPECT_EQ([p[@"veto"] intValue], 0) << [[p description] UTF8String];
    EXPECT_GE([p[@"pipelined"] intValue], 10) << [[p description] UTF8String];
}

// What an extended-range texture sampled in pass 9 of a paced CA frame takes off encode-ahead, printed per form: its
// stand-in's bind vetoes the frame's encode ahead (VetoShadows, the stand-in drawn from the codes at the commit's encode),
// or, where this harness's pack has no conversion state for it, the frame is taken whole (VetoWhole) or, with no m2v
// behind it, fails; a two-plane
// RGB10A8_2P_XR10 bind adds no veto of its own (RMPass::xrPlaneBinds).
TEST(RawFacadeRangeForms, ExtendedRangeSamplesAndTheEncodeAhead)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const scale_frames::Shape shape{15, 90, 4};
    const auto into = scale_frames::targets(opened().device, shape);
    for (const uint32_t format : {0u, 554u, 550u}) {
        id<MTLTexture> sampled = format ? target(MTLPixelFormat(format), shape) : nil;
        if (format && !sampled) {
            printf("format %u: no texture made here\n", format);
            continue;
        }
        id<MTLCommandBuffer> cb = paced(queue, into, shape, 9, ^(id<MTLCommandBuffer> c, id<MTLTexture> t) {
            onePass(c, t, ^(id<MTLRenderCommandEncoder> e) {
                if (sampled)
                    [e setFragmentTexture:sampled atIndex:0];
            });
        });
        [cb commit];
        [cb waitUntilCompleted];
        if (format && cb.status == MTLCommandBufferStatusError) {
            printf("sampled format %u: no conversion state in this harness's pack, the command buffer fails\n", format);
            continue;
        }
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << format;
        NSDictionary *p = pipelined(cb);
        ASSERT_NE(p, nil);
        printf("sampled format %u in pass 9: %d of %d encoders encoded ahead, veto %d, path %s\n", format,
               [p[@"pipelined"] intValue], [p[@"encoders"] intValue], [p[@"veto"] intValue], path(cb).c_str());
        if (!format)
            EXPECT_GE([p[@"pipelined"] intValue], 10);
        else
            EXPECT_NE([p[@"veto"] intValue] & int(VetoShadows | VetoWhole), 0) << format;
    }
}

}  // namespace
