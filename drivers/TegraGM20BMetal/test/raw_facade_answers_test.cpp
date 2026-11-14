// SPDX-License-Identifier: MIT
// The answers above the identity (raw_device_answers.h) through the facade device's own selectors, and the paths behind
// them staying raw on this build's kind: a layered pass that draws, a pass of eight targets of the widest format, the
// creations the answers promise (eight amplifications, patches in an indirect command buffer, a dynamic stride).
#include "raw_facade_harness.h"
#include "MetalABI.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BMacroSlots.h"
#include "../raw/runtime/raw_device_answers.h"
#include "bazel/testing/fresh_process.h"

#include <gtest/gtest.h>
#include <string_view>

namespace {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm, kWide = MTLPixelFormatRGBA32Float;
constexpr bool kA13 = std::string_view(SWITCH_METAL_KIND) == "24B119";

uint64_t count(id<MTLDevice> device, const char *selector)
{
    return ((NSUInteger (*)(id, SEL))objc_msgSend)(device, sel_registerName(selector));
}

uint64_t flag(id<MTLDevice> device, const char *selector)
{
    return ((BOOL (*)(id, SEL))objc_msgSend)(device, sel_registerName(selector));
}

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

void expectRaw(id<MTLDevice> device, id<MTLCommandBuffer> cb)
{
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    NSDictionary *drivers = counters(device);
    EXPECT_EQ([drivers[@"raw"] intValue], 1) << text(drivers);
    EXPECT_EQ([drivers[@"m2v"] intValue], 0) << text(drivers);
    EXPECT_EQ(gOpens, 0);
}

void answers()
{
    id<MTLDevice> device = facade::device();
    ASSERT_TRUE(device != nil);
    EXPECT_EQ(count(device, "maxViewportCount"), uint64_t(SWITCH_GM20B_LIMIT_VIEWPORTS));
    EXPECT_EQ(count(device, "maxTessellationFactor"), uint64_t(SWITCH_GM20B_LIMIT_TESSELLATION_LEVEL));
    EXPECT_EQ(flag(device, "supportsIndirectTessellation"), 1u);
    EXPECT_EQ(flag(device, "supportsDynamicAttributeStride"), 1u);
    EXPECT_EQ(count(device, "maxThreadgroupMemoryLength"), uint64_t(SWITCH_GM20B_LIMIT_THREADGROUP_MEMORY_BYTES));
    EXPECT_EQ(count(device, "maxComputeThreadgroupMemory"), uint64_t(SWITCH_GM20B_LIMIT_THREADGROUP_MEMORY_BYTES));
    EXPECT_EQ(count(device, "maxFramebufferStorageBits"), SWITCH_GM20B_LIMIT_COLOR_TARGETS * 128u);
    EXPECT_EQ(count(device, "maxInterpolatedComponents"),
              kA13 ? 124u : uint64_t(SWITCH_GM20B_LIMIT_INTERPOLATED_COMPONENTS));
    EXPECT_EQ(count(device, "argumentBuffersSupport"), uint64_t(MTLArgumentBuffersTier2));
    EXPECT_EQ(count(device, "maxIndirectTextures"), 500000u);
    EXPECT_EQ(count(device, "maxIndirectSamplers"), 500000u);
    EXPECT_EQ(count(device, "maxVertexAmplificationFactor"), uint64_t(SWITCH_GM20B_AMP_MAX));
    for (NSUInteger n = 1; n <= SWITCH_GM20B_AMP_MAX; n++)
        EXPECT_TRUE([device supportsVertexAmplificationCount:n]) << n;
    EXPECT_FALSE([device supportsVertexAmplificationCount:SWITCH_GM20B_AMP_MAX + 1]);
    if (!kA13)
        EXPECT_EQ(flag(device, "supportsLayeredRendering"), 1u);
}

// A state of eight amplifications is made; one past the amplify macro's mapping words is refused at creation
void creations()
{
    id<MTLDevice> device = facade::device();
    ASSERT_TRUE(device != nil);
    MTLRenderPipelineDescriptor *d = descriptor(nil, kPacked);
    d.maxVertexAmplificationCount = SWITCH_GM20B_AMP_MAX;
    EXPECT_EQ(rmUnsupportedRenderState(d), nullptr);
    d.maxVertexAmplificationCount = SWITCH_GM20B_AMP_MAX + 1;
    EXPECT_NE(rmUnsupportedRenderState(d), nullptr);

    MTLIndirectCommandBufferDescriptor *patches = [MTLIndirectCommandBufferDescriptor new];
    patches.commandTypes = MTLIndirectCommandTypeDrawPatches | MTLIndirectCommandTypeDrawIndexedPatches;
    patches.inheritPipelineState = YES;
    patches.inheritBuffers = YES;
    EXPECT_TRUE([device newIndirectCommandBufferWithDescriptor:patches maxCommandCount:4 options:0] != nil);
}

// A pass of four layers from slice 1 of a six-layer array, drawn with the pack's state, as a one-layer pass draws
void layeredDraw()
{
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2DArray;
    td.pixelFormat = kPacked;
    td.width = td.height = 32;
    td.arrayLength = 6;
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    ASSERT_TRUE(target != nil);
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.renderTargetArrayLength = 4;
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].slice = 1;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:state];
    const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:4];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    expectRaw(device, cb);
}

// Every colour target of RGBA32Float: the pixel's bits maxFramebufferStorageBits answers
MTLRenderPipelineDescriptor *wideDescriptor(id<MTLLibrary> library)
{
    MTLRenderPipelineDescriptor *d = descriptor(library, kWide);
    for (NSUInteger c = 1; c < SWITCH_GM20B_LIMIT_COLOR_TARGETS; c++)
        d.colorAttachments[c].pixelFormat = kWide;
    return d;
}

void wideTargets()
{
    const raw::StageKey vertex{"v", ""}, fragment{"f", ""};
    const std::string key = raw::pipelineKey(wideDescriptor(nil), &vertex, &fragment, "", "QuartzCore");
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(writePack(key));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    MTLRenderPipelineDescriptor *d = wideDescriptor(library);
    EXPECT_EQ(std::string(raw_metal_pipeline_key(d).UTF8String), key);
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kWide width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    for (NSUInteger c = 0; c < SWITCH_GM20B_LIMIT_COLOR_TARGETS; c++) {
        pass.colorAttachments[c].texture = [device newTextureWithDescriptor:td];
        ASSERT_TRUE(pass.colorAttachments[c].texture != nil) << c;
        pass.colorAttachments[c].loadAction = MTLLoadActionClear;
        pass.colorAttachments[c].storeAction = MTLStoreActionStore;
    }
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:state];
    const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    expectRaw(device, cb);
}

}  // namespace

TEST(RawFacadeAnswers, AnswersAboveTheIdentityAreGm20bs)
{
    switch_ios::test::ExpectPassesInFreshProcess(answers);
}

TEST(RawFacadeAnswers, TheirCreationsAreMade)
{
    switch_ios::test::ExpectPassesInFreshProcess(creations);
}

TEST(RawFacadeAnswers, ALayeredPassThatDrawsStaysRaw)
{
    switch_ios::test::ExpectPassesInFreshProcess(layeredDraw);
}

TEST(RawFacadeAnswers, EightTargetsOfTheWidestFormatStayRaw)
{
    switch_ios::test::ExpectPassesInFreshProcess(wideTargets);
}
