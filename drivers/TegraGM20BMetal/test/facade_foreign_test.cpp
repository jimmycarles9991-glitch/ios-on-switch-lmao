// SPDX-License-Identifier: MIT
// A call Metal's validation stops for an object another device made, or nil where Metal takes none, fails its command
// buffer InvalidResource as it is recorded (TegraGM20BMetalObjects.h gm20bCallInvalid), its predicate in its path and its failed
// counter. A command on an encoder that had ended: facade_ended_encoder_test.cpp.
#include "scale_frames.h"

#include <gtest/gtest.h>
#include <string>

// A buffer this device did not make
@interface ForeignBuffer : NSObject
@end

@implementation ForeignBuffer

- (NSUInteger)length
{
    return 64;
}

@end

// A render or depth/stencil state this device did not make
@interface ForeignState : NSObject
@end

@implementation ForeignState
@end

// A texture this device did not make
@interface ForeignTexture : NSObject
@end

@implementation ForeignTexture

- (MTLPixelFormat)pixelFormat
{
    return MTLPixelFormatBGRA8Unorm;
}

@end

namespace {

using scale_frames::opened;

const scale_frames::Shape kOnePass{1, 1, 1};

id<MTLBuffer> foreignBuffer()
{
    return (id<MTLBuffer>)[ForeignBuffer new];
}

std::string path(id<MTLCommandBuffer> cb)
{
    return [[(id)cb performSelector:@selector(switchMetalPath)] UTF8String];
}

// `cb` committed failed InvalidResource for predicate `pred`, counted once under it
void expectInvalid(id<MTLCommandBuffer> cb, const char *pred, const char *call)
{
    NSDictionary *before = harness::counters(opened().device);
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError) << call;
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInvalidResource) << call << ": " << cb.error.description.UTF8String;
    EXPECT_EQ(path(cb), std::string("error:") + pred) << call;
    EXPECT_EQ(harness::failedMoved(before, harness::counters(opened().device)), harness::failedOnce(pred)) << call;
}

// One pass of the BGRA8 state into a new target, `begun` run on its encoder before its draw
id<MTLCommandBuffer> passWith(void (^begun)(id<MTLRenderCommandEncoder>))
{
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    scale_frames::pass(cb, opened().state, scale_frames::targets(opened().device, kOnePass), kOnePass, 0, 0, nil,
                       begun);
    return cb;
}

TEST(GM20BFacadeForeign, ABufferOfAnotherDeviceFails)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                      [e setVertexBuffer:foreignBuffer() offset:0 atIndex:5];
                  }),
                  "call.buf", "setVertexBuffer");
    expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                      [e setFragmentBuffer:foreignBuffer() offset:0 atIndex:5];
                  }),
                  "call.buf", "setFragmentBuffer");
    expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                      [e setVertexBuffer:foreignBuffer() offset:0 attributeStride:16 atIndex:5];
                  }),
                  "call.buf", "setVertexBuffer:attributeStride:");
}

TEST(GM20BFacadeForeign, AnIndexBufferOfAnotherDeviceOrNoneFails)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLBuffer> buffers[] = {foreignBuffer(), nil};
    for (id<MTLBuffer> indices : buffers)
        expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                          [e setRenderPipelineState:opened().state];
                          [e drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                        indexCount:3
                                         indexType:MTLIndexTypeUInt16
                                       indexBuffer:indices
                                 indexBufferOffset:0];
                      }),
                      "call.buf", indices ? "another device's index buffer" : "no index buffer");
}

TEST(GM20BFacadeForeign, ARenderStateOfAnotherDeviceOrNoneFails)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLRenderPipelineState> states[] = {(id<MTLRenderPipelineState>)[ForeignState new], nil};
    for (id<MTLRenderPipelineState> state : states)
        expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                          [e setRenderPipelineState:state];
                      }),
                      "state.foreign", state ? "another device's render state" : "no render state");
}

TEST(GM20BFacadeForeign, ADepthStencilStateOfAnotherDeviceFails)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                      [e setDepthStencilState:(id<MTLDepthStencilState>)[ForeignState new]];
                  }),
                  "state.foreign", "setDepthStencilState");
}

TEST(GM20BFacadeForeign, APatchDrawsBufferOfAnotherDeviceFails)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                      [e setTessellationFactorBuffer:foreignBuffer() offset:0 instanceStride:0];
                  }),
                  "call.buf", "setTessellationFactorBuffer");
    expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                      [e drawPatches:4
                                      patchStart:0
                                      patchCount:1
                                patchIndexBuffer:foreignBuffer()
                          patchIndexBufferOffset:0
                                   instanceCount:1
                                    baseInstance:0];
                  }),
                  "call.buf", "drawPatches");
}

TEST(GM20BFacadeForeign, ABlitOfATextureOfAnotherDeviceFails)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLTexture> foreign = (id<MTLTexture>)[ForeignTexture new];
    id<MTLTexture> own = scale_frames::targets(opened().device, kOnePass)[0];
    id<MTLBuffer> rows = [opened().device newBufferWithLength:64 * 4 * 64 options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const MTLSize size = MTLSizeMake(64, 64, 1);
    const auto blit = [&](void (^copy)(id<MTLBlitCommandEncoder>)) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        copy(b);
        [b endEncoding];
        return cb;
    };
    expectInvalid(blit(^(id<MTLBlitCommandEncoder> b) {
                      [b copyFromTexture:foreign
                                sourceSlice:0
                                sourceLevel:0
                               sourceOrigin:MTLOriginMake(0, 0, 0)
                                 sourceSize:size
                                  toTexture:own
                           destinationSlice:0
                           destinationLevel:0
                          destinationOrigin:MTLOriginMake(0, 0, 0)];
                  }),
                  "tex.foreign", "copyFromTexture:toTexture:");
    expectInvalid(blit(^(id<MTLBlitCommandEncoder> b) {
                      [b copyFromBuffer:rows
                                 sourceOffset:0
                            sourceBytesPerRow:256
                          sourceBytesPerImage:256 * 64
                                   sourceSize:size
                                    toTexture:foreign
                             destinationSlice:0
                             destinationLevel:0
                            destinationOrigin:MTLOriginMake(0, 0, 0)];
                  }),
                  "tex.foreign", "copyFromBuffer:toTexture:");
    expectInvalid(blit(^(id<MTLBlitCommandEncoder> b) {
                      [b copyFromTexture:foreign
                                       sourceSlice:0
                                       sourceLevel:0
                                      sourceOrigin:MTLOriginMake(0, 0, 0)
                                        sourceSize:size
                                          toBuffer:rows
                                 destinationOffset:0
                            destinationBytesPerRow:256
                          destinationBytesPerImage:256 * 64];
                  }),
                  "tex.foreign", "copyFromTexture:toBuffer:");
}

// A blit's buffer of another device, filled, copied or copied into a texture, and a counter sample buffer of another
// device resolved: each fails its command buffer as it is recorded, as every object of another device does
TEST(GM20BFacadeForeign, ABlitOfABufferOrCounterSampleBufferOfAnotherDeviceFails)
{
    ASSERT_TRUE(opened().device != nil);
    id<MTLBuffer> own = [opened().device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    id<MTLTexture> texture = scale_frames::targets(opened().device, kOnePass)[0];
    id<MTLCommandQueue> queue = [opened().device newCommandQueue];
    const auto blit = [&](void (^copy)(id<MTLBlitCommandEncoder>)) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        copy(b);
        [b endEncoding];
        return cb;
    };
    expectInvalid(blit(^(id<MTLBlitCommandEncoder> b) {
                      [b fillBuffer:foreignBuffer() range:NSMakeRange(0, 64) value:7];
                  }),
                  "call.buf", "fillBuffer:");
    expectInvalid(blit(^(id<MTLBlitCommandEncoder> b) {
                      [b copyFromBuffer:foreignBuffer() sourceOffset:0 toBuffer:own destinationOffset:0 size:64];
                  }),
                  "call.buf", "copyFromBuffer:toBuffer:");
    expectInvalid(blit(^(id<MTLBlitCommandEncoder> b) {
                      [b copyFromBuffer:foreignBuffer()
                                 sourceOffset:0
                            sourceBytesPerRow:256
                          sourceBytesPerImage:256 * 64
                                   sourceSize:MTLSizeMake(16, 16, 1)
                                    toTexture:texture
                             destinationSlice:0
                             destinationLevel:0
                            destinationOrigin:MTLOriginMake(0, 0, 0)];
                  }),
                  "call.buf", "copyFromBuffer:toTexture:");
    expectInvalid(blit(^(id<MTLBlitCommandEncoder> b) {
                      [b resolveCounters:(id<MTLCounterSampleBuffer>)[ForeignState new]
                                    inRange:NSMakeRange(0, 1)
                          destinationBuffer:own
                          destinationOffset:0];
                  }),
                  "call.counter", "resolveCounters:");
}

// A resource or heap used, or a fence waited for, of another device: the command buffer fails as it is recorded
TEST(GM20BFacadeForeign, AUseOrFenceOfAnotherDevicesObjectFails)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                      [e useResource:(id<MTLResource>)foreignBuffer() usage:MTLResourceUsageRead];
                  }),
                  "call.buf", "useResource:");
    expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                      [e useHeap:(id<MTLHeap>)[ForeignState new]];
                  }),
                  "call.buf", "useHeap:");
    expectInvalid(passWith(^(id<MTLRenderCommandEncoder> e) {
                      [e waitForFence:(id<MTLFence>)[ForeignState new] beforeStages:MTLRenderStageVertex];
                  }),
                  "event", "waitForFence:beforeStages:");
}

// One sub-encoder's call fails the parallel pass's command buffer, the other sub-encoders' passes run nowhere
TEST(GM20BFacadeForeign, AParallelSubEncodersBufferOfAnotherDeviceFailsItsCommandBuffer)
{
    ASSERT_TRUE(opened().device != nil && opened().state != nil);
    id<MTLCommandBuffer> cb = [[opened().device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
    d.colorAttachments[0].texture = scale_frames::targets(opened().device, kOnePass)[0];
    d.colorAttachments[0].loadAction = MTLLoadActionClear;
    d.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLParallelRenderCommandEncoder> parallel = [cb parallelRenderCommandEncoderWithDescriptor:d];
    const float vertices[16] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1, 1, 1, 0, 1};
    for (int sub = 0; sub < 2; sub++) {
        id<MTLRenderCommandEncoder> e = [parallel renderCommandEncoder];
        [e setRenderPipelineState:opened().state];
        [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
        if (sub)
            [e setVertexBuffer:foreignBuffer() offset:0 atIndex:5];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
        [e endEncoding];
    }
    [parallel endEncoding];
    expectInvalid(cb, "call.buf", "a parallel sub-encoder's setVertexBuffer");
}

}  // namespace
