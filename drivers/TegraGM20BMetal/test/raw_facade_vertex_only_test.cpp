// SPDX-License-Identifier: MIT
// A state of no fragment function (RenderBox's path_edges: depth and stencil from its vertex function, its colours
// masked) is the pack's record and the raw path's draw: its creation and its pass never open the m2v device, its bind
// turns the pixel stage off and the pass's end turns it back on (raw_stage_switches.h). One Metal does not rasterize keys
// rast=0 and turns the rasteriser off the same way. A state of both stages draws with both on, as the channel's context
// turned them (raw_gpu.cpp stageSwitchesOn): no pack baseline or record sets them. Each scenario is a process of its own.
#include "raw_facade_harness.h"
#include "bazel/testing/fresh_process.h"
#include "../../../kits/SwitchGM20BEmit/SwitchGM20BPushDecode.h"
#include "../raw/runtime/raw_stage_switches.h"

#include <gtest/gtest.h>
#include <algorithm>

@interface NSObject (VertexOnlyTestPackHit)
- (NSNumber *)switchPackHit;
@end

namespace {

using namespace harness;

constexpr MTLPixelFormat kColour = MTLPixelFormatRGBA16Float, kDepth = MTLPixelFormatDepth32Float_Stencil8;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

MTLRenderPipelineDescriptor *vertexOnly(id<MTLLibrary> library, bool rasterize)
{
    MTLRenderPipelineDescriptor *d = descriptor(library, kColour);
    d.fragmentFunction = nil;
    d.colorAttachments[0].writeMask = MTLColorWriteMaskNone;
    d.depthAttachmentPixelFormat = kDepth;
    d.stencilAttachmentPixelFormat = kDepth;
    d.rasterizationEnabled = rasterize;
    return d;
}

std::string vertexOnlyKey(bool rasterize)
{
    const raw::StageKey vertex{"v", ""};
    return raw::pipelineKey(vertexOnly(nil, rasterize), &vertex, nullptr, "", "QuartzCore");
}

MTLRenderPipelineDescriptor *twoStage(id<MTLLibrary> library)
{
    MTLRenderPipelineDescriptor *d = vertexOnly(library, true);
    d.fragmentFunction = [library newFunctionWithName:@"f"];
    return d;
}

std::string twoStageKey()
{
    const raw::StageKey vertex{"v", ""}, fragment{"f", ""};
    return raw::pipelineKey(twoStage(nil), &vertex, &fragment, "", "QuartzCore");
}

id<MTLTexture> target(id<MTLDevice> device, MTLPixelFormat format)
{
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget;
    return [device newTextureWithDescriptor:td];
}

// One pass drawing a triangle with `state` into a colour and a depth/stencil target, committed and waited for
id<MTLCommandBuffer> drawDepth(id<MTLDevice> device, id<MTLRenderPipelineState> state)
{
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target(device, kColour);
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLTexture> depth = target(device, kDepth);
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionStore;
    pass.stencilAttachment.texture = depth;
    pass.stencilAttachment.loadAction = MTLLoadActionClear;
    pass.stencilAttachment.storeAction = MTLStoreActionStore;
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

// The values the jobs since `first` wrote to `method`, in order
std::vector<uint32_t> written(size_t first, uint32_t method)
{
    std::vector<uint32_t> out;
    const auto &jobs = kernel_model::model.jobs;
    for (size_t j = first; j < jobs.size(); j++) {
        std::vector<switch_gm20b_method> m(jobs[j].words.size());
        const long n = switch_gm20b_push_decode(jobs[j].words.data(), jobs[j].words.size(), m.data(), m.size());
        for (long i = 0; i < n; i++)
            if (m[i].subchannel == 0 && m[i].method == method)
                out.push_back(m[i].value);
    }
    return out;
}

// The state's bind turned the switch off, and the pass's end on again (a channel's context may have turned it on before)
void expectTurnedOffThenOn(const std::vector<uint32_t> &values, uint32_t off, uint32_t on)
{
    const auto at = std::find(values.begin(), values.end(), off);
    ASSERT_NE(at, values.end());
    EXPECT_NE(std::find(at, values.end(), on), values.end());
    EXPECT_EQ(values.back(), on);
}

void drawnWithItsSwitchOff(bool rasterize)
{
    const std::string pack = writePack(vertexOnlyKey(rasterize), {}, {}, false);
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(pack);
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    MTLRenderPipelineDescriptor *d = vertexOnly(library, rasterize);
    EXPECT_EQ(std::string(raw_metal_pipeline_key(d).UTF8String), vertexOnlyKey(rasterize));
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    EXPECT_TRUE([[(id)state switchPackHit] boolValue]);

    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = drawDepth(device, state);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);

    NSDictionary *m2v = m2vRecord(device);
    EXPECT_EQ(gOpens, 0);
    EXPECT_FALSE([m2v[@"opened"] boolValue]);
    EXPECT_EQ([m2v[@"needs"] count], 0u) << text(m2v);

    expectTurnedOffThenOn(written(first, raw::kPixelStageSwitch), raw::kPixelStageOff, raw::kPixelStageOn);
    if (rasterize)
        for (const uint32_t v : written(first, raw::kRasterSwitch))
            EXPECT_EQ(v, 1u);
    else
        expectTurnedOffThenOn(written(first, raw::kRasterSwitch), 0, 1);
    EXPECT_EQ(written(first, 0x2144), std::vector<uint32_t>{}) << "a pixel program set for a state of none";
}

void vertexOnlyDraw()
{
    drawnWithItsSwitchOff(true);
}

void unrasterizedDraw()
{
    drawnWithItsSwitchOff(false);
}

void twoStageDraw()
{
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(writePack(twoStageKey()));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    MTLRenderPipelineDescriptor *d = twoStage(library);
    EXPECT_EQ(std::string(raw_metal_pipeline_key(d).UTF8String), twoStageKey());
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    EXPECT_TRUE([[(id)state switchPackHit] boolValue]);
    id<MTLCommandBuffer> cb = drawDepth(device, state);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);

    const std::vector<uint32_t> pixel = written(0, raw::kPixelStageSwitch), raster = written(0, raw::kRasterSwitch);
    EXPECT_FALSE(pixel.empty()) << "no channel context turned the pixel stage on";
    EXPECT_FALSE(raster.empty()) << "no channel context turned the rasteriser on";
    for (const uint32_t v : pixel)
        EXPECT_EQ(v, raw::kPixelStageOn);
    for (const uint32_t v : raster)
        EXPECT_EQ(v, 1u);
}

}  // namespace

TEST(RawFacadeVertexOnly, AStateOfNoFragmentFunctionIsDrawnByTheRawPathWithItsPixelStageOff)
{
    switch_ios::test::ExpectPassesInFreshProcess(vertexOnlyDraw);
}

TEST(RawFacadeVertexOnly, AStateMetalDoesNotRasterizeIsDrawnWithTheRasteriserOff)
{
    switch_ios::test::ExpectPassesInFreshProcess(unrasterizedDraw);
}

TEST(RawFacadeVertexOnly, AStateOfBothStagesDrawsWithThePixelStageAndRasteriserTheContextTurnedOn)
{
    switch_ios::test::ExpectPassesInFreshProcess(twoStageDraw);
}
