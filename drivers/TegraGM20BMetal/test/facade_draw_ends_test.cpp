// SPDX-License-Identifier: MIT
// How the draws gm20b::RenderEncoder refuses at encode end on the facade (TegraGM20BMetalDrawErrors.mm drawFailed), m2v
// never opened: patches of a state that fetches its colour through the texture unit and a shader-side stencil in a pass
// with no stencil each fail their command buffer with their line, and every refusal id fails its command buffer with
// the error drawFailed names, none left for m2v. Each scenario is a process of its own.
#include "facade_variant_compiler.h"
#include "bazel/testing/fresh_process.h"
#include "../icb.h"
#include "../rotated_pass.h"
#include "../shader_tests.h"

#include <unistd.h>

namespace {

using namespace harness;

constexpr uint8_t kControlWord = 5, kShadowWord = 5;
constexpr uint32_t kStencilView = 0x6014921d;  // Stencil8's header word 0 (zeta.h)

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

bool logged(const char *reason)
{
    return !switch_refusal_first(switch_refusal_key("gm20b", reason));
}

// A tessellation state reading the colour it writes through the texture unit, with no storage form
// (device_fetch_patches_test.cpp fetchingPatches)
void fetchingPatches(const gm20b::CompileRequest &, gm20b::RuntimeRecord &record)
{
    for (gm20b::CapturedProgram &p : record.tessPrograms) {
        const auto stage = &p - record.tessPrograms;
        p.image.assign(80 + 64, 0);
        p.image[80] = uint8_t(1 + stage);
        p.id = "fetchtess" + std::to_string(stage);
        p.gprs = 12;
        p.cbufTypes = {gm20b::CbufRoot, gm20b::CbufSet};
    }
    gm20b::PackPipeline &pp = record.pipeline;
    pp.controlWord = kControlWord;
    pp.controlPoints = 4;
    pp.setBytes = 0;
    pp.memoryEntries = 0;
    gm20b::setEntryCount(pp, gm20b::TessBufferCount + 1);
    for (uint8_t k = 0; k < gm20b::TessBufferCount; k++)
        pp.entry[k] = {uint16_t(16 * (k + 1)),
                       gm20b::EntryTessBuffer,
                       1,
                       k,
                       uint8_t(1 | (kControlWord + gm20b::kTessResidualWord[k]) << 2),
                       0,
                       0};
    pp.entry[gm20b::TessBufferCount] = {0, gm20b::EntryFetch, 2, 0, 0, 0, 0};
    pp.pushMask = 0x3fu << (kControlWord - 1);
    pp.flags |= gm20b::PipelineFetches;
    record.extras = {{0x0320, 0x202}, {0x0dcc, 1}, {0x2080, 0x21}, {0x2090, 1}, {0x20c0, 0x31}, {0x20d0, 2}};
    record.restore = {{0x2080, 0x20}, {0x20c0, 0x30}};
}

// A shader-side stencil's record of a state with a stencil attachment (storage.h usableShadowStencil)
void shadowStencil(const gm20b::CompileRequest &, gm20b::RuntimeRecord &record)
{
    gm20b::PackPipeline &pp = record.pipeline;
    gm20b::setEntryCount(pp, 1);
    pp.entry[0] = {0, gm20b::EntryStencilTest, 2, 0, 0, 0, 0};
    pp.setBytes = 16;
    pp.memoryEntries = 1;
    pp.flags |= gm20b::PipelinePushMetadata;
    pp.pushMask = 0x30;
    pp.shaderTests = {0, 0, gm20b::kShaderTestStencil, kShadowWord, 0, kStencilView, 0};
    record.blob = {0x1224, 2, 0x1a00, 0x1111};
}

// A device over a pack of the BGRA8 state, m2v the stand-in, `compiler` making the RGBA8 state: that state
id<MTLRenderPipelineState> served(RenderCompiler &compiler, const char *name, id<MTLDevice> __strong &device)
{
    const std::string directory = dir() + "/" + name + "-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    if (!compiler.serve(directory))
        return nil;
    device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    if (!device)
        return nil;
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
    return library
               ? [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatRGBA8Unorm) error:nil]
               : nil;
}

// One pass into a 64x64 RGBA8 target (beside `depth` where given) drawing with `state` as `draw` does, committed and
// waited for
id<MTLCommandBuffer> drawn(id<MTLDevice> device, id<MTLRenderPipelineState> state, id<MTLTexture> depth,
                           void (^draw)(id<MTLRenderCommandEncoder>))
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:64
                                                                                 height:64
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = [device newTextureWithDescriptor:td];
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (depth) {
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    }
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:state];
    draw(e);
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return cb;
}

void patchesOfAFetchingStateWithNoStorageForm()
{
    RenderCompiler compiler;
    compiler.shapes = fetchingPatches;
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = served(compiler, "fetch-patches", device);
    ASSERT_TRUE(state != nil);
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    ASSERT_TRUE(pipeline->_raw);
    ASSERT_EQ(pipeline->_raw->fetchBits, 1);
    ASSERT_EQ(pipeline->_raw->storageBits, 0);
    id<MTLCommandBuffer> cb = drawn(device, state, nil, ^(id<MTLRenderCommandEncoder> e) {
        [e drawPatches:4
                        patchStart:0
                        patchCount:1
                  patchIndexBuffer:nil
            patchIndexBufferOffset:0
                     instanceCount:1
                      baseInstance:0];
    });
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal) << text(cb.error);
    EXPECT_NE(text(cb.error).find("framebuffer-fetch ordering"), std::string::npos) << text(cb.error);
    EXPECT_TRUE(logged("draw.fetch_patches"));
}

void aShadowStencilInAPassWithNoStencil()
{
    RenderCompiler compiler;
    compiler.shapes = shadowStencil;
    id<MTLDevice> device = nil;
    id<MTLRenderPipelineState> state = served(compiler, "shadow-stencil", device);
    ASSERT_TRUE(state != nil);
    ASSERT_TRUE(((SwitchMetalPipeline *)state)->_raw);
    id<MTLCommandBuffer> cb = drawn(device, state, nil, ^(id<MTLRenderCommandEncoder> e) {
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    });
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal) << text(cb.error);
    EXPECT_NE(text(cb.error).find("an aspect or sample count the pass's attachment does not have"), std::string::npos)
        << text(cb.error);
    EXPECT_TRUE(logged("draw.depth_stencil"));
}

struct Ending {
    gm20b::Refusal refusal;
    const char *line;
    MTLCommandBufferError code;
};

void everyRefusalFailsItsCommandBuffer()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    constexpr MTLCommandBufferError kInternal = MTLCommandBufferErrorInternal;
    const Ending endings[] = {
        {{gm20b::RefNone}, "draw.unnamed", kInternal},
        {{gm20b::RefGeometry, gm20b::icb::kPatchPrimitive, 2, 1}, "draw.fetch_patches", kInternal},
        {{gm20b::RefDepthStencil, 252, gm20b::kShaderTestStencil, 1}, "draw.depth_stencil", kInternal},
        {{gm20b::RefStorageSamples, 0, 70, 4}, "draw.storage_samples", kInternal},
        {{gm20b::RefStorageView, 0, 115, 64}, "draw.storage_view", kInternal},
        {{gm20b::RefSamplerHeader, 0, 1, 0}, "draw.sampler_header", kInternal},
        {{gm20b::RefMatrixUniform, 3, 0, 16}, "draw.matrix_uniform", kInternal},
        // a turned draw whose turned fragment program found no room in the shader heap (device_room_test.cpp)
        {{gm20b::RefRotated, uint32_t(gm20b::RotatedRefusal::Variant), 1},
         "draw.turned_memory",
         MTLCommandBufferErrorOutOfMemory},
    };
    for (const Ending &x : endings) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        EXPECT_TRUE(drawFailed((SwitchMetalCommandBuffer *)cb, x.refusal, x.line)) << x.line;
        [cb commit];
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusError) << x.line;
        EXPECT_EQ(cb.error.code, x.code) << x.line << ": " << text(cb.error);
        EXPECT_TRUE(logged(x.line)) << x.line;
    }
}

}  // namespace

TEST(GM20BFacadeDrawEnds, PatchesOfAStateFetchingThroughTheTextureUnitFailInternalNeverM2v)
{
    switch_ios::test::ExpectPassesInFreshProcess(patchesOfAFetchingStateWithNoStorageForm);
}

TEST(GM20BFacadeDrawEnds, AShaderSideStencilInAPassWithNoStencilFailsInternalNeverM2v)
{
    switch_ios::test::ExpectPassesInFreshProcess(aShadowStencilInAPassWithNoStencil);
}

TEST(GM20BFacadeDrawEnds, EveryRefusalFailsItsCommandBufferWithItsErrorAndLine)
{
    switch_ios::test::ExpectPassesInFreshProcess(everyRefusalFailsItsCommandBuffer);
}
