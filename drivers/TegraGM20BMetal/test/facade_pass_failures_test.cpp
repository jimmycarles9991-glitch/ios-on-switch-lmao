// SPDX-License-Identifier: MIT
// The driver's own work around a pass failing as it is encoded (TegraGM20BMetalPassErrors.h gm20bPassWorkFails): a turn copy or
// a Depth16Unorm's conversion the pack holds no state of fails Internal, a texture header or a backing the device has
// none left for fails OutOfMemory, and more two-plane RGB10A8_2P_XR10 textures written in a pass than its images hold
// fails Internal. Each ends its command buffer with the line of what failed, its predicate named in its path, nothing
// of it on m2v. The device's texture headers are taken by views (takeEveryHeader); the missing states are the own
// states the test leaves out.
#include "facade_pass_scene.h"

namespace {

using namespace pass_scene;

constexpr const char *kNoTurnCopyState = "no turn copy state for the attachment's format";
constexpr const char *kNoConversionState = "no depth conversion state for the formats";

// The device of the BGRA8 pack and its compiler's shader-testing state, no own state added
id<MTLRenderPipelineState> serveShaderTests(RenderCompiler &compiler, const char *name, id<MTLDevice> __strong &device)
{
    compiler.shapes = testsDepthInItsShader;
    id<MTLLibrary> library = served(compiler, name, writePack(packKey(MTLPixelFormatBGRA8Unorm)), device);
    return library ? shaderTestingState(device, library) : nil;
}

void (^bound(id<MTLRenderPipelineState> state))(id<MTLRenderCommandEncoder>)
{
    return ^(id<MTLRenderCommandEncoder> e) {
        [e setRenderPipelineState:state];
    };
}

// A turned pass whose depth no turn copy state moves: in before it where it loads, back after it where it only stores
void aTurnedDepthWithNoTurnCopyState(MTLLoadAction load)
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    id<MTLTexture> plane = turnedPlane(device), depth = attachmentTexture(device, MTLPixelFormatDepth32Float, 64, 32);
    ASSERT_TRUE(plane != nil && depth != nil);
    expectFailed(device, run(device, pass(plane, depth, load)), "pass.rotated", MTLCommandBufferErrorInternal,
                 kNoTurnCopyState);
}

void aTurnedDepthLoadedWithNoTurnCopyState()
{
    aTurnedDepthWithNoTurnCopyState(MTLLoadActionLoad);
}

void aTurnedDepthStoredWithNoTurnCopyState()
{
    aTurnedDepthWithNoTurnCopyState(MTLLoadActionClear);
}

// A shader-tested Depth16Unorm the pack converts one way only: loaded with no conversion in, stored with none back
void aDepth16WithOneConversion(bool in)
{
    RenderCompiler compiler;
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = serveShaderTests(compiler, in ? "depth16-in" : "depth16-back", device);
    ASSERT_TRUE(state != nil);
    ASSERT_TRUE(in ? depthConversionBack() : depthConversionIn());
    id<MTLTexture> colour = attachmentTexture(device, MTLPixelFormatBGRA8Unorm, 64, 64),
                   depth = attachmentTexture(device, MTLPixelFormatDepth16Unorm, 64, 64);
    ASSERT_TRUE(colour != nil && depth != nil);
    id<MTLCommandBuffer> cb =
        run(device, pass(colour, depth, in ? MTLLoadActionLoad : MTLLoadActionClear), bound(state));
    expectFailed(device, cb, "enc.pass", MTLCommandBufferErrorInternal, kNoConversionState);
}

void aLoadedDepth16WithNoConversionIn()
{
    aDepth16WithOneConversion(true);
}

void aStoredDepth16WithNoConversionBack()
{
    aDepth16WithOneConversion(false);
}

void aTurnedPassesDepthSliceWithNoHeaderLeft()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    ASSERT_TRUE(depth32Turned());
    id<MTLTexture> plane = turnedPlane(device),
                   depth = attachmentTexture(device, MTLPixelFormatDepth32Float, 64, 32, 2),
                   viewed = attachmentTexture(device, MTLPixelFormatRGBA8Unorm, 16, 16);
    ASSERT_TRUE(plane != nil && depth != nil && viewed != nil);
    NSMutableArray *held = takeEveryHeader(device, viewed);
    ASSERT_EQ(headersLeft(device), 0u);
    expectFailed(device, run(device, pass(plane, depth, MTLLoadActionLoad, 1)), "pass.rotated",
                 MTLCommandBufferErrorOutOfMemory,
                 "no texture header for a turned pass's depth/stencil level or slice");
    EXPECT_GT(held.count, 0u);
}

// A shader-tested Depth16Unorm with no texture header left: a loaded slice's conversion reads one of its own, a cleared
// whole one's Depth32Float_Stencil8 scratch takes one
void aShaderTestedDepth16WithNoHeaderLeft(bool slice)
{
    RenderCompiler compiler;
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = serveShaderTests(compiler, slice ? "slice-headers" : "scratch-headers", device);
    ASSERT_TRUE(state != nil);
    ASSERT_TRUE(depthConversionIn() && depthConversionBack());
    id<MTLTexture> colour = attachmentTexture(device, MTLPixelFormatBGRA8Unorm, 64, 64),
                   depth = attachmentTexture(device, MTLPixelFormatDepth16Unorm, 64, 64, slice ? 2 : 1),
                   viewed = attachmentTexture(device, MTLPixelFormatRGBA8Unorm, 16, 16);
    ASSERT_TRUE(colour != nil && depth != nil && viewed != nil);
    NSMutableArray *held = takeEveryHeader(device, viewed);
    ASSERT_EQ(headersLeft(device), 0u);
    MTLRenderPassDescriptor *p =
        slice ? pass(colour, depth, MTLLoadActionLoad, 1) : pass(colour, depth, MTLLoadActionClear);
    id<MTLCommandBuffer> cb = run(device, p, bound(state));
    if (slice)
        expectFailed(device, cb, "pass.ds", MTLCommandBufferErrorOutOfMemory,
                     "no texture header for a Depth16Unorm's conversion");
    else
        expectFailed(device, cb, "enc.pass", MTLCommandBufferErrorOutOfMemory,
                     "no Depth32Float_Stencil8 scratch for a Depth16Unorm");
    EXPECT_GT(held.count, 0u);
}

void aShaderTestedDepth16SliceWithNoHeaderLeft()
{
    aShaderTestedDepth16WithNoHeaderLeft(true);
}

void aShaderTestedDepth16ScratchWithNoHeaderLeft()
{
    aShaderTestedDepth16WithNoHeaderLeft(false);
}

// A memoryless colour beside a stored one: its backing takes a header the device has none of
void aMemorylessColoursBackingWithNoHeaderLeft()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    id<MTLTexture> memoryless =
                       attachmentTexture(device, MTLPixelFormatBGRA8Unorm, 64, 64, 1, MTLStorageModeMemoryless),
                   stored = attachmentTexture(device, MTLPixelFormatBGRA8Unorm, 64, 64),
                   viewed = attachmentTexture(device, MTLPixelFormatRGBA8Unorm, 16, 16);
    ASSERT_TRUE(memoryless != nil && stored != nil && viewed != nil);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = memoryless;
    p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].storeAction = MTLStoreActionDontCare;
    p.colorAttachments[1].texture = stored;
    p.colorAttachments[1].loadAction = MTLLoadActionClear;
    p.colorAttachments[1].storeAction = MTLStoreActionStore;
    NSMutableArray *held = takeEveryHeader(device, viewed);
    ASSERT_EQ(headersLeft(device), 0u);
    expectFailed(device, run(device, p), "enc.pass", MTLCommandBufferErrorOutOfMemory,
                 "no memory for a pass's attachment backing: MTLCommandBufferErrorOutOfMemory (MTLCommandBuffer.h)");
    EXPECT_GT(held.count, 0u);
}

}  // namespace

TEST(GM20BFacadePassFailures, ATurnedDepthLoadedWithNoTurnCopyStateFailsInternal)
{
    switch_ios::test::ExpectPassesInFreshProcess(aTurnedDepthLoadedWithNoTurnCopyState);
}

TEST(GM20BFacadePassFailures, ATurnedDepthStoredWithNoTurnCopyStateFailsInternal)
{
    switch_ios::test::ExpectPassesInFreshProcess(aTurnedDepthStoredWithNoTurnCopyState);
}

TEST(GM20BFacadePassFailures, ALoadedDepth16WithNoConversionInFailsInternal)
{
    switch_ios::test::ExpectPassesInFreshProcess(aLoadedDepth16WithNoConversionIn);
}

TEST(GM20BFacadePassFailures, AStoredDepth16WithNoConversionBackFailsInternal)
{
    switch_ios::test::ExpectPassesInFreshProcess(aStoredDepth16WithNoConversionBack);
}

TEST(GM20BFacadePassFailures, ATurnedPassesDepthSliceWithNoHeaderLeftFailsOutOfMemory)
{
    switch_ios::test::ExpectPassesInFreshProcess(aTurnedPassesDepthSliceWithNoHeaderLeft);
}

TEST(GM20BFacadePassFailures, AShaderTestedDepth16SliceWithNoHeaderLeftFailsOutOfMemory)
{
    switch_ios::test::ExpectPassesInFreshProcess(aShaderTestedDepth16SliceWithNoHeaderLeft);
}

TEST(GM20BFacadePassFailures, AShaderTestedDepth16ScratchWithNoHeaderLeftFailsOutOfMemory)
{
    switch_ios::test::ExpectPassesInFreshProcess(aShaderTestedDepth16ScratchWithNoHeaderLeft);
}

TEST(GM20BFacadePassFailures, AMemorylessColoursBackingWithNoHeaderLeftFailsOutOfMemory)
{
    switch_ios::test::ExpectPassesInFreshProcess(aMemorylessColoursBackingWithNoHeaderLeft);
}
