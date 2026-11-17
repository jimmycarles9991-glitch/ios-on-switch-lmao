// SPDX-License-Identifier: MIT
// Textures of Metal's rotation SPI on the raw facade. Over no surface Metal makes none (an M4 Max: its descriptor
// validation wants framebufferOnly, then its driver asserts "Buffer texture has invalid rotation"), so the facade
// answers nil and asks m2v for no twin. Over a surface plane one is framebuffer-only, and one bound as a texture
// argument anyway is read through its plane as it lies, as an M4 Max reads it: its draw stays raw. A state whose record
// holds no turned fragment program fails its turned command buffer, never reaching m2v. Each scenario is a process of
// its own.
#include "raw_facade_harness.h"
#include "MetalABI.h"
#include "bazel/testing/fresh_process.h"

#import <IOSurface/IOSurface.h>
#include <cstddef>
#include <gtest/gtest.h>

namespace {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

MTLTextureDescriptor *turned(NSUInteger width, NSUInteger height, NSUInteger rotation)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked width:width height:height mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    [d setValue:@YES forKey:@"framebufferOnly"];
    [d setValue:@(rotation) forKey:@"rotation"];
    return d;
}

// The 32x64 plane a 64x32 frame of rotation 1 turns into
IOSurfaceRef planeSurface()
{
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @32,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @256,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
}

void turnedOverNoSurface()
{
    const std::string pack = writePack(packKey(kPacked));
    writeMetallib();
    id<MTLDevice> device = open(pack);
    ASSERT_TRUE(device != nil);
    for (NSUInteger rotation = 1; rotation <= 4; rotation++) {
        MTLTextureDescriptor *d = turned(64, 32, rotation);
        d.storageMode = MTLStorageModePrivate;
        EXPECT_EQ([device newTextureWithDescriptor:d], nil) << rotation;
        d.storageMode = MTLStorageModeShared;
        id<MTLBuffer> buffer = [device newBufferWithLength:64 * 32 * 4 options:MTLResourceStorageModeShared];
        EXPECT_EQ([buffer newTextureWithDescriptor:d offset:0 bytesPerRow:64 * 4], nil) << rotation;
    }
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
}

void turnedPlaneBound()
{
    const std::string pack = writePack(packKey(kPacked));
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(pack);
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    IOSurfaceRef surface = planeSurface();
    ASSERT_TRUE(surface != nullptr);
    id<MTLTexture> plane = [device newTextureWithDescriptor:turned(64, 32, 1) iosurface:surface plane:0];
    ASSERT_TRUE(plane != nil);

    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:state];
    [e setFragmentTexture:plane atIndex:0];
    const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    NSDictionary *drivers = counters(device);
    EXPECT_EQ([drivers[@"raw"] intValue], 1) << text(drivers);
    EXPECT_EQ([drivers[@"m2v"] intValue], 0) << text(drivers);
    EXPECT_EQ(gOpens, 0);
    CFRelease(surface);
}

// The pack's one pipeline marked as raw_direct.cpp marks a state with no turned fragment program
void markTurnRefused(const std::string &pack)
{
    std::fstream f(pack, std::ios::in | std::ios::out | std::ios::binary);
    raw::PackHeader h = {};
    f.read(reinterpret_cast<char *>(&h), sizeof h);
    const uint32_t flags = raw::PipelineTurnRefused;
    f.seekp(std::streamoff(h.section[raw::PackPipelines].offset + offsetof(raw::PackPipeline, flags)));
    f.write(reinterpret_cast<const char *>(&flags), sizeof flags);
}

id<MTLCommandBuffer> drawInto(id<MTLDevice> device, id<MTLRenderPipelineState> state, id<MTLTexture> target)
{
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:state];
    const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb;
}

// The pack's state marked PipelineTurnRefused (its flag read by the pack reader) draws upright on the raw path; drawn
// into a turned plane its command buffer fails with MTLCommandBufferErrorInternal and the state's line
void turnRefusedState()
{
    const std::string key = packKey(kPacked);
    const std::string pack = writePack(key);
    markTurnRefused(pack);
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(pack);
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    id<MTLCommandBuffer> upright = draw(device, state, kPacked);
    EXPECT_EQ(upright.status, MTLCommandBufferStatusCompleted) << text(upright.error);

    IOSurfaceRef surface = planeSurface();
    ASSERT_TRUE(surface != nullptr);
    id<MTLTexture> plane = [device newTextureWithDescriptor:turned(64, 32, 1) iosurface:surface plane:0];
    ASSERT_TRUE(plane != nil);
    const uint64_t hash = raw::Pack::fnv(key.data(), key.size());
    const uint64_t line = switch_refusal_key_with(switch_refusal_key("raw", "pass.turn_refused"), hash);
    id<MTLCommandBuffer> cb = drawInto(device, state, plane);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal) << text(cb.error);
    char named[32];
    snprintf(named, sizeof named, "state %016llx", (unsigned long long)hash);
    EXPECT_NE(text(cb.error).find(named), std::string::npos) << text(cb.error);
    EXPECT_NE(text(cb.error).find(key), std::string::npos) << text(cb.error);
    EXPECT_FALSE(switch_refusal_first(line)) << "the state's raw:pass.turn_refused line was logged";
    NSDictionary *drivers = counters(device);
    EXPECT_EQ([drivers[@"m2v"] intValue], 0) << text(drivers);
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
    CFRelease(surface);
}

}  // namespace

TEST(RawFacadeTurned, ARotationOverNoSurfaceIsNilAndAsksNoTwin)
{
    switch_ios::test::ExpectPassesInFreshProcess(turnedOverNoSurface);
}

TEST(RawFacadeTurned, ATurnedPlaneBoundAsATextureDrawsRaw)
{
    switch_ios::test::ExpectPassesInFreshProcess(turnedPlaneBound);
}

TEST(RawFacadeTurned, AStateWithNoTurnedProgramFailsItsTurnedCommandBufferNeverM2v)
{
    switch_ios::test::ExpectPassesInFreshProcess(turnRefusedState);
}
