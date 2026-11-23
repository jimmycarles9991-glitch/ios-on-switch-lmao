// SPDX-License-Identifier: MIT
// A command buffer that binds a state variant still being made (a cubic sampler's, its compile held back) and whose ended
// encoders the submit worker would encode ahead of its commit (TegraGM20BMetalPipeline.mm): the binding vetoes the encode ahead
// (VetoVariant), as an encode ahead would drop every draw of a state it has no record of yet. At the commit, once the
// variant is made, every draw is drawn, the cubic pass's with the variant's programs.
#include "facade_variant_compiler.h"
#include "bazel/testing/fresh_process.h"

#include <objc/message.h>
#include <algorithm>
#include <thread>
#include <unistd.h>

namespace {

using namespace harness;

const std::vector<gm20b::PackEntry> kSampled = {{0, gm20b::EntryTexture, 1, 0, gm20b::SamplerSlot, 0, 0},
                                                {16, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0}};

// Draws a pass: the cubic pass alone is past the worker's hand-off (TegraGM20BMetalPipeline.mm kHandOff)
constexpr unsigned kDraws = 32, kPasses = 4;

// The compile of a cubic variant, held until it is opened. Shared with the compiler's thread, which may outlive a failed
// test's frame.
struct HeldCompile {
    std::mutex lock;
    std::condition_variable opened;
    bool held = true;

    void open()
    {
        {
            std::lock_guard<std::mutex> l(lock);
            held = false;
        }
        opened.notify_all();
    }
};

id<MTLSamplerState> cubicSampler(id<MTLDevice> device)
{
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = d.magFilter = static_cast<MTLSamplerMinMagFilter>(gm20b::kFilterCubic);
    return [device newSamplerStateWithDescriptor:d];
}

MTLRenderPassDescriptor *colourPass(id<MTLTexture> target, MTLLoadAction load)
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = load;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    return pass;
}

NSDictionary *pipelined(id<MTLCommandBuffer> cb)
{
    return ((NSDictionary * (*)(id, SEL)) objc_msgSend)((id)cb, sel_registerName("switchMetalPipelined"));
}

void aPendingVariantIsNotEncodedAhead()
{
    RenderCompiler compiler;
    const std::string directory = dir() + "/pending-variant-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    ASSERT_TRUE(compiler.serve(directory));
    const auto held = std::make_shared<HeldCompile>();
    kernel_model::model.compiler = [held, compile = kernel_model::model.compiler](const std::string &bytes) {
        gm20b::CompileRequest request;
        if (gm20b::decodeRequest(bytes, request) && !request.variant.cubic.empty()) {
            std::unique_lock<std::mutex> l(held->lock);
            held->opened.wait(l, [&] {
                return !held->held;
            });
        }
        return compile(bytes);
    };
    NSString *why = nil;
    id<MTLDevice> device = metal_device_open(
        writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, kSampled).c_str(), 0, 0, &why);
    ASSERT_TRUE(device != nil) << (why ? why.UTF8String : "");
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    id<MTLTexture> sampled = image(device, MTLTextureUsageShaderRead);
    id<MTLTexture> target = image(device, MTLTextureUsageRenderTarget);
    id<MTLSamplerState> cubic = cubicSampler(device);
    ASSERT_TRUE(state != nil && sampled != nil && target != nil && cubic != nil);
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    const uint32_t base = program(pipeline->_raw->methods, 2);
    const size_t firstJob = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    for (unsigned pass = 0; pass < kPasses; pass++) {
        id<MTLRenderCommandEncoder> encoder = [commands
            renderCommandEncoderWithDescriptor:colourPass(target, pass ? MTLLoadActionLoad : MTLLoadActionClear)];
        [encoder setRenderPipelineState:state];
        texture(encoder, 2, sampled);
        if (!pass)
            sampler(encoder, 2, cubic);
        for (unsigned d = 0; d < kDraws; d++)
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // the worker has had the ended encoders to encode ahead while the variant is still being made
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    ASSERT_EQ(pipeline->_variants.size(), 1u);
    gm20b::Variant<gm20b::Pipeline> &variant = *pipeline->_variants.begin()->second;
    EXPECT_EQ(variant.state(), gm20b::Variant<gm20b::Pipeline>::State::Pending);
    held->open();
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    NSDictionary *p = pipelined(commands);
    ASSERT_TRUE(p != nil);
    EXPECT_NE([p[@"veto"] unsignedIntValue] & VetoVariant, 0u) << [[p description] UTF8String];
    EXPECT_EQ([p[@"pipelined"] intValue], 0) << [[p description] UTF8String];
    ASSERT_TRUE(ready(variant));
    const uint32_t made = program(variant.value()->methods, 2);
    ASSERT_NE(made, base);
    const std::vector<uint32_t> drawn = drawnPrograms(firstJob, 2);
    const auto madeDraws = std::count(drawn.begin(), drawn.end(), made),
               baseDraws = std::count(drawn.begin(), drawn.end(), base);
    EXPECT_GE(madeDraws, long(kDraws)) << "the cubic pass's draws, each with the variant's programs";
    EXPECT_GE(baseDraws, long((kPasses - 1) * kDraws));
    EXPECT_EQ(size_t(madeDraws + baseDraws), drawn.size());
}

}  // namespace

TEST(GM20BFacadePendingVariant, ABindingOfAVariantStillBeingMadeIsNotEncodedAhead)
{
    switch_ios::test::ExpectPassesInFreshProcess(aPendingVariantIsNotEncodedAhead);
}
