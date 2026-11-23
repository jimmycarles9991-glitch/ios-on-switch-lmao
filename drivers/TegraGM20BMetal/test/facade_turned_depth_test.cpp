// SPDX-License-Identifier: MIT
// Depth attachments the driver renders through attachments of its own, on the facade: a Depth16Unorm a draw tests in
// its shader beside a turned plane (converted into a Depth32Float_Stencil8 the turned pass moves into its plane and
// back), a slice of a depth array beside a turned plane (its turn copies reading the texture's own header of the
// slice), and a slice of a Depth16Unorm array a draw tests in its shader (converted through the texture's header of the
// slice). Each completes on the driver, the m2v device never opened.
#include "facade_pass_scene.h"

namespace {

using namespace pass_scene;

struct Served {
    RenderCompiler compiler;
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = nil;
};

// The device of the BGRA8 pack and its compiler's shader-testing state, with the own states a Depth16Unorm's conversion
// and the turn copy of its Depth32Float_Stencil8 draw
bool serveShaderTests(Served &s, const char *name)
{
    s.compiler.shapes = testsDepthInItsShader;
    id<MTLLibrary> library = served(s.compiler, name, writePack(packKey(MTLPixelFormatBGRA8Unorm)), s.device);
    if (!library || !depthConversionIn() || !depthConversionBack() || !zetaColourRoute())
        return false;
    s.state = shaderTestingState(s.device, library);
    return s.state != nil;
}

// One draw of `state` under a depth test that writes
void (^drawTested(id<MTLDevice> device, id<MTLRenderPipelineState> state))(id<MTLRenderCommandEncoder>)
{
    id<MTLDepthStencilState> tests = depthWrite(device);
    return ^(id<MTLRenderCommandEncoder> e) {
        [e setRenderPipelineState:state];
        [e setDepthStencilState:tests];
        [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    };
}

void aTurnedDepth16ShaderTestedPass()
{
    Served s;
    ASSERT_TRUE(serveShaderTests(s, "turned-depth16"));
    id<MTLTexture> plane = turnedPlane(s.device),
                   depth = attachmentTexture(s.device, MTLPixelFormatDepth16Unorm, 64, 32);
    ASSERT_TRUE(plane != nil && depth != nil);
    expectRaw(s.device, run(s.device, pass(plane, depth, MTLLoadActionLoad), drawTested(s.device, s.state)));
}

// The same turned pass three times: the slice's header is the texture's, made at the first and taken again since, so no
// command buffer after the first holds a texture header more
void aTurnedPassLoadingADepthSlice()
{
    const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm));
    id<MTLDevice> device = open(pack);
    ASSERT_TRUE(device != nil);
    ASSERT_TRUE(depth32Turned());
    id<MTLTexture> plane = turnedPlane(device),
                   depth = attachmentTexture(device, MTLPixelFormatDepth32Float, 64, 32, 2);
    ASSERT_TRUE(plane != nil && depth != nil);
    uint32_t left[3] = {};
    for (uint32_t &after : left) {
        expectRaw(device, run(device, pass(plane, depth, MTLLoadActionLoad, 1)));
        after = headersLeft(device);
    }
    EXPECT_EQ(left[2], left[1]);
}

void aShaderTestedDepth16Slice()
{
    Served s;
    ASSERT_TRUE(serveShaderTests(s, "depth16-slice"));
    id<MTLTexture> colour = attachmentTexture(s.device, MTLPixelFormatBGRA8Unorm, 64, 64),
                   depth = attachmentTexture(s.device, MTLPixelFormatDepth16Unorm, 64, 64, 2);
    ASSERT_TRUE(colour != nil && depth != nil);
    expectRaw(s.device, run(s.device, pass(colour, depth, MTLLoadActionLoad, 1), drawTested(s.device, s.state)));
}

}  // namespace

TEST(GM20BFacadeTurnedDepth, ATurnedDepth16ShaderTestedPassIsRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(aTurnedDepth16ShaderTestedPass);
}

TEST(GM20BFacadeTurnedDepth, ATurnedPassLoadingADepthSliceReadsTheTexturesOwnHeaderOfIt)
{
    switch_ios::test::ExpectPassesInFreshProcess(aTurnedPassLoadingADepthSlice);
}

TEST(GM20BFacadeTurnedDepth, AShaderTestedDepth16SliceConvertsThroughItsTexturesHeaderOfIt)
{
    switch_ios::test::ExpectPassesInFreshProcess(aShaderTestedDepth16Slice);
}
