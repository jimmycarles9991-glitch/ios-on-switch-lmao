// SPDX-License-Identifier: MIT
// What the facade tests of presenting and held command buffers share: the facade over the pack of the BGRA8 state, a
// drawable that counts its presents, an IOSurface render target as a CAMetalLayer drawable's texture is, and a draw
// into it.
#pragma once
#include "facade_harness.h"

#import <IOSurface/IOSurface.h>
#include <atomic>
#include <string>

// A drawable that counts its presents of every kind
@interface CountingDrawable : NSObject <MTLDrawable>
@property (nonatomic, readonly) int presents;
@end

@implementation CountingDrawable {
    std::atomic<int> _made;
}

- (int)presents
{
    return _made.load();
}

- (void)present
{
    _made++;
}

- (void)presentAtTime:(CFTimeInterval)presentationTime
{
    (void)presentationTime;
    _made++;
}

- (void)presentAfterMinimumDuration:(CFTimeInterval)duration
{
    (void)duration;
    _made++;
}

- (void)addPresentedHandler:(MTLDrawablePresentedHandler)block
{
    (void)block;
}

- (CFTimeInterval)presentedTime
{
    return 0;
}

- (NSUInteger)drawableID
{
    return 0;
}

@end

namespace drawn {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm;
constexpr float kTriangle[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};

inline std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

struct Opened {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> state;
    std::string why;  // which was not made, and its own error
};

// The facade over the pack of the BGRA8 state, and that state; the render server's device when `renderServer`
// (metal_prepare's), else another process's
inline Opened openPacked(bool renderServer = false)
{
    const std::string metallib = writeMetallib();
    const std::string pack = writePack(packKey(kPacked));
    if (renderServer)
        metal_prepare(pack.c_str(), 0);
    NSString *why = nil;
    id<MTLDevice> device = open(pack, &why);
    if (!device)
        return {nil, nil, std::string("no device: ") + (why.UTF8String ?: "?")};
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    if (!library)
        return {device, nil, "no library: " + text(error)};
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked)
                                                                              error:&error];
    if (!state)
        return {device, nil, "no state: " + text(error)};
    return {device, state, ""};
}

// A render target over an IOSurface, as a CAMetalLayer drawable's texture is
inline id<MTLTexture> surfaceTarget(id<MTLDevice> device)
{
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @256,
        (id)kIOSurfaceAllocSize : @(64 * 256),
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    if (!surface)
        return nil;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked
                                                                                 width:64
                                                                                height:64
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> t = [device newTextureWithDescriptor:d iosurface:surface plane:0];
    CFRelease(surface);
    return t;
}

// A clear of `t` and a draw into it
inline void drawInto(id<MTLCommandBuffer> cb, id<MTLTexture> t, id<MTLRenderPipelineState> state)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
    [e setRenderPipelineState:state];
    [e setVertexBytes:kTriangle length:sizeof kTriangle atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

// A command buffer drawing into `t` that presents `drawable` (nil: none) completed: the jobs it took. The first one
// also makes the channel, so a later refusal is the submission's.
inline size_t jobsOfDrawn(id<MTLCommandQueue> queue, id<MTLTexture> t, id<MTLRenderPipelineState> state,
                          CountingDrawable *drawable)
{
    const size_t before = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    drawInto(cb, t, state);
    if (drawable)
        [cb presentDrawable:drawable];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    return kernel_model::model.jobs.size() - before;
}

}  // namespace drawn
