// SPDX-License-Identifier: MIT
// A render state's variants (TegraGM20BMetalCubic.mm): each key asked once in the process and named once where none is made, a
// packed state's test class the pack lacks asked as any other form, the test classes of a state this process made made
// with it, and a pass switching among its states finding each variant again without the state's map
#include "facade_variant_compiler.h"
#include "../fragment_tests.h"
#include "bazel/testing/fresh_process.h"

#include <set>
#include <unistd.h>

namespace {

using namespace harness;
using State = gm20b::Variant<gm20b::Pipeline>::State;

const std::vector<gm20b::PackEntry> kSampled = {{0, gm20b::EntryTexture, 1, 0, gm20b::SamplerSlot, 0, 0},
                                                {16, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0}};

// The device of `pack`, served by `compiler` from a directory of `name`'s, and the test's library on it. Without
// `serving` the kernel says no compiler is registered or expected until the test sets its flags.
id<MTLLibrary> served(RenderCompiler &compiler, const std::string &name, const std::string &pack,
                      id<MTLDevice> __strong &device, bool serving = true)
{
    const std::string directory = dir() + "/" + name + "-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    if (!compiler.serve(directory))
        return nil;
    if (!serving)
        kernel_model::model.compilerFlags = TegraGM20BCompilerCapable;
    NSString *why = nil;
    device = metal_device_open(pack.c_str(), 0, 0, &why);
    if (!device)
        return nil;
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    return [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
}

id<MTLSamplerState> cubicSampler(id<MTLDevice> device)
{
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = d.magFilter = static_cast<MTLSamplerMinMagFilter>(gm20b::kFilterCubic);
    return [device newSamplerStateWithDescriptor:d];
}

MTLRenderPassDescriptor *colourPass(id<MTLDevice> device, bool depth)
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = image(device, MTLTextureUsageRenderTarget);
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    if (depth) {
        pass.depthAttachment.texture = image(device, MTLTextureUsageRenderTarget, MTLPixelFormatDepth32Float);
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.clearDepth = 1;
    }
    return pass;
}

id<MTLDepthStencilState> depthWrite(id<MTLDevice> device)
{
    MTLDepthStencilDescriptor *ds = [MTLDepthStencilDescriptor new];
    ds.depthCompareFunction = MTLCompareFunctionLess;
    ds.depthWriteEnabled = YES;
    return [device newDepthStencilStateWithDescriptor:ds];
}

gm20b::Variant<gm20b::Pipeline> *variantOf(__unsafe_unretained SwitchMetalPipeline *state,
                                           const gm20b::RenderVariant &forms)
{
    const auto found = state->_variants.find(forms.name());
    return found == state->_variants.end() ? nullptr : found->second.get();
}

// A cubic variant the compiler could not compile fails the command buffer that draws it. The next command buffer that
// draws it meets the same variant and fails without asking again, and the variant's key has one refusal line. Each
// failure names no predicate (error:none, failed.none), and the process's first of its description is logged.
void failedVariant()
{
    RenderCompiler compiler;
    compiler.fails = [](const gm20b::CompileRequest &request) {
        return !request.variant.cubic.empty();
    };
    id<MTLDevice> device = nil;
    id<MTLLibrary> library =
        served(compiler, "variant-failed", writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, kSampled),
               device);
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    id<MTLTexture> sampled = image(device, MTLTextureUsageShaderRead);
    id<MTLSamplerState> cubic = cubicSampler(device);
    ASSERT_TRUE(state != nil && sampled != nil && cubic != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLRenderPassDescriptor *pass = colourPass(device, false);
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    gm20b::Variant<gm20b::Pipeline> *first = nullptr;
    for (unsigned round = 0; round < 2; round++) {
        @autoreleasepool {
            NSDictionary *before = counters(device);
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:state];
            texture(encoder, 2, sampled);
            sampler(encoder, 2, cubic);
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            EXPECT_EQ(commands.status, MTLCommandBufferStatusError) << "round " << round;
            EXPECT_EQ(commands.error.code, MTLCommandBufferErrorInternal) << "round " << round;
            EXPECT_EQ(gm20bPath(commands), "error:none") << "round " << round;
            EXPECT_EQ(failedMoved(before, counters(device)), failedOnce("none")) << "round " << round;
            const auto asked = compiler.captured();
            ASSERT_EQ(asked.size(), 1u) << "round " << round << ": a variant no compiler made is not asked again";
            gm20b::Variant<gm20b::Pipeline> *variant = variantOf(pipeline, asked[0].variant);
            ASSERT_NE(variant, nullptr);
            EXPECT_EQ(variant->state(), State::Failed);
            if (!round)
                first = variant;
            EXPECT_EQ(variant, first) << "the failed variant stays the state's";
        }
    }
    const std::string key = pipeline->_key + compiler.captured()[0].variant.name();
    const uint64_t line =
        switch_refusal_key_with(SWITCH_REFUSAL_KEY("gm20b", "state.variant"), gm20b::Pack::fnv(key.data(), key.size()));
    EXPECT_FALSE(switch_refusal_first(line)) << "no refusal line for " << key;
    const uint64_t failure = switch_refusal_key_with(SWITCH_REFUSAL_KEY("gm20b.failed", "none"),
                                                     switch_refusal_key("", "A render pipeline could not be compiled"));
    EXPECT_FALSE(switch_refusal_first(failure)) << "no failure line";
}

// A cubic variant first drawn while the kernel says no compiler serves fails that command buffer with no answer about
// its key. Once a compiler serves, the next pass that draws it asks once and draws it, and nothing names it refused.
void unservedVariant()
{
    RenderCompiler compiler;
    const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, kSampled);
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "variant-unserved", pack, device, false);
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:nil];
    id<MTLTexture> sampled = image(device, MTLTextureUsageShaderRead);
    id<MTLSamplerState> cubic = cubicSampler(device);
    ASSERT_TRUE(state != nil && sampled != nil && cubic != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLRenderPassDescriptor *pass = colourPass(device, false);
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    const MTLCommandBufferStatus expected[3] = {MTLCommandBufferStatusError, MTLCommandBufferStatusCompleted,
                                                MTLCommandBufferStatusCompleted};
    gm20b::Variant<gm20b::Pipeline> *first = nullptr;
    for (unsigned round = 0; round < 3; round++) {
        @autoreleasepool {
            if (round == 1)
                kernel_model::model.compilerFlags = TegraGM20BCompilerCapable | TegraGM20BCompilerExpected |
                                                    TegraGM20BCompilerRegistered | TegraGM20BCompilerStore;
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            [encoder setRenderPipelineState:state];
            texture(encoder, 2, sampled);
            sampler(encoder, 2, cubic);
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            EXPECT_EQ(commands.status, expected[round]) << "round " << round;
            EXPECT_EQ(compiler.captured().size(), round ? 1u : 0u) << "round " << round;
            ASSERT_EQ(pipeline->_variants.size(), 1u);
            gm20b::Variant<gm20b::Pipeline> *variant = pipeline->_variants.begin()->second.get();
            EXPECT_EQ(variant->state(), round ? State::Ready : State::Failed) << "round " << round;
            if (!round)
                first = variant;
            EXPECT_EQ(variant, first) << "the state keeps the one variant plans name";
        }
    }
    const std::string key = pipeline->_key + pipeline->_variants.begin()->first;
    const uint64_t line =
        switch_refusal_key_with(SWITCH_REFUSAL_KEY("gm20b", "state.variant"), gm20b::Pack::fnv(key.data(), key.size()));
    EXPECT_TRUE(switch_refusal_first(line)) << "a refusal line for " << key;
}

// Two states of one descriptor, drawn in turn with a cubic sampler: once each has its variant, switching between them
// in a pass takes no lock and allocates nothing
void switchedStates()
{
    RenderCompiler compiler;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library =
        served(compiler, "variant-switched",
               writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, kSampled), device);
    ASSERT_TRUE(library != nil);
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatBGRA8Unorm);
    id<MTLRenderPipelineState> states[2] = {[device newRenderPipelineStateWithDescriptor:d error:nil],
                                            [device newRenderPipelineStateWithDescriptor:d error:nil]};
    id<MTLTexture> sampled = image(device, MTLTextureUsageShaderRead);
    id<MTLSamplerState> cubic = cubicSampler(device);
    ASSERT_TRUE(states[0] != nil && states[1] != nil && sampled != nil && cubic != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    MTLRenderPassDescriptor *pass = colourPass(device, false);
    const auto draws = [&](id<MTLRenderCommandEncoder> encoder) {
        for (unsigned i = 0; i < 16; i++) {
            [encoder setRenderPipelineState:states[i & 1]];
            [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
    };
    for (unsigned round = 0; round < 9; round++) {
        @autoreleasepool {
            id<MTLCommandBuffer> commands = [queue commandBuffer];
            id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass];
            texture(encoder, 2, sampled);
            sampler(encoder, 2, cubic);
            for (id<MTLRenderPipelineState> state : states) {
                [encoder setRenderPipelineState:state];
                for (const auto &[name, variant] : ((SwitchMetalPipeline *)state)->_variants)
                    ASSERT_TRUE(ready(*variant)) << name;
            }
            record_count work = {};
            const bool measured = round == 8;
            if (measured)
                record_count_start();
            draws(encoder);
            if (measured) {
                record_count_stop(&work);
                EXPECT_EQ(work.locks, 0u);
                EXPECT_EQ(work.mallocs, 0u);
            }
            [encoder endEncoding];
            [commands commit];
            [commands waitUntilCompleted];
            ASSERT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
        }
    }
}

// `state` (BGRA8Unorm and Depth32Float, its record fetching) drawn with a depth test that writes takes its DepthWrite
// variant's program, and the compiler has been asked `requests` times in all
void drawsDepthWrite(id<MTLDevice> device, id<MTLRenderPipelineState> state, RenderCompiler &compiler, size_t requests)
{
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    gm20b::RenderVariant forms;
    forms.fragmentTests = uint8_t(gm20b::FragmentTests::DepthWrite);
    gm20b::Variant<gm20b::Pipeline> *variant = variantOf(pipeline, forms);
    ASSERT_NE(variant, nullptr);
    ASSERT_TRUE(ready(*variant));
    const uint32_t expected = program(variant->value()->methods, 2);
    ASSERT_NE(expected, program(pipeline->_raw->methods, 2));
    const size_t firstJob = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:colourPass(device, true)];
    [encoder setRenderPipelineState:state];
    [encoder setDepthStencilState:depthWrite(device)];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    EXPECT_EQ(drawnPrograms(firstJob, 2), (std::vector<uint32_t>{expected}));
    EXPECT_EQ(compiler.captured().size(), requests);
}

// A packed state's test class the pack lacks is asked of the compiler at the draw that needs it, as any other form is,
// and the draw takes it, never the state's own record
void packedTestClass()
{
    RenderCompiler compiler;
    MTLRenderPipelineDescriptor *description = descriptor(nil, MTLPixelFormatBGRA8Unorm);
    description.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    const gm20b::StageKey vertex{"v", ""}, fragment{"f", ""};
    std::vector<gm20b::PackEntry> entries = kSampled;
    entries.push_back({32, gm20b::EntryFetch, 2, 0, gm20b::SamplerNone, 0, 0});
    const std::string pack =
        writePack(gm20b::pipelineKey(description, &vertex, &fragment, "", "QuartzCore"), {}, {}, true, "", {}, entries);
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "variant-packed-tests", pack, device);
    ASSERT_TRUE(library != nil);
    description.vertexFunction = [library newFunctionWithName:@"v"];
    description.fragmentFunction = [library newFunctionWithName:@"f"];
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:description error:nil];
    ASSERT_TRUE(state != nil);
    ASSERT_TRUE(((SwitchMetalPipeline *)state)->_packed);
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:colourPass(device, true)];
    [encoder setRenderPipelineState:state];
    [encoder setDepthStencilState:depthWrite(device)];
    ASSERT_TRUE(compiler.wait(2)) << "the class of the pass's first state binding, then DepthWrite's";
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    drawsDepthWrite(device, state, compiler, 2);
}

// A state this process makes, of a Depth32Float attachment, whose record fetches: its creation makes the variant of
// each test class a draw can take (Pass, ReadOnly, DepthWrite) before it returns, so its draws ask nothing
void madeTestClasses()
{
    RenderCompiler compiler;
    const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm));
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "variant-made-tests", pack, device);
    ASSERT_TRUE(library != nil);
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatBGRA8Unorm);
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:nil];
    ASSERT_TRUE(state != nil);
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    ASSERT_TRUE(pipeline->_raw && !pipeline->_packed) << "the shader compiler makes the state at its creation";
    std::set<uint8_t> classes;
    for (const auto &request : compiler.captured())
        if (request.variant.fragmentTests)
            classes.insert(request.variant.fragmentTests);
    EXPECT_EQ(classes, (std::set<uint8_t>{uint8_t(gm20b::FragmentTests::Pass), uint8_t(gm20b::FragmentTests::ReadOnly),
                                          uint8_t(gm20b::FragmentTests::DepthWrite)}));
    for (const uint8_t tests : classes) {
        gm20b::RenderVariant forms;
        forms.fragmentTests = tests;
        gm20b::Variant<gm20b::Pipeline> *variant = variantOf(pipeline, forms);
        ASSERT_NE(variant, nullptr) << unsigned(tests);
        EXPECT_EQ(variant->state(), State::Ready) << unsigned(tests) << ": made before its creation returned";
    }
    drawsDepthWrite(device, state, compiler, compiler.captured().size());
}

// A fetching state whose own record has no storage form and whose test classes are made with one (RenderBox's
// accumulator_coverage, HW Test gpu.gm20b.layered_shader_tests): a draw of two instances, which one triangle at a time
// cannot keep in raster order, is drawn by its DepthWrite variant inside the pixel interlock, on the driver
void instancedDrawOfAStorageTestClass()
{
    RenderCompiler compiler;
    compiler.shapes = [](const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record) {
        if (!request.variant.fragmentTests)
            return;
        gm20b::PackPipeline &pp = record.pipeline;
        pp.flags |= gm20b::PipelineStorageMetadata;
        pp.storageMask = pp.storageWriteMask = 1;
        pp.storageSamples = 1;
        pp.storageTic[0] = gm20b::formatInfo(MTLPixelFormatBGRA8Unorm)->tic0;
        record.blob = {0x1224, 1, 0x1a00, 0};
    };
    const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm));
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "variant-storage-tests", pack, device);
    ASSERT_TRUE(library != nil);
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatBGRA8Unorm);
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:nil];
    ASSERT_TRUE(state != nil);
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    ASSERT_TRUE(pipeline->_raw);
    ASSERT_EQ(pipeline->_raw->storageBits, 0) << "the state's own record fetches through the texture unit";
    gm20b::RenderVariant forms;
    forms.fragmentTests = uint8_t(gm20b::FragmentTests::DepthWrite);
    gm20b::Variant<gm20b::Pipeline> *variant = variantOf(pipeline, forms);
    ASSERT_NE(variant, nullptr);
    ASSERT_TRUE(ready(*variant));
    ASSERT_EQ(variant->value()->storageBits, 1);
    const size_t firstJob = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:colourPass(device, true)];
    [encoder setRenderPipelineState:state];
    [encoder setDepthStencilState:depthWrite(device)];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3 instanceCount:2];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    EXPECT_EQ(drawnPrograms(firstJob, 2), (std::vector<uint32_t>{program(variant->value()->methods, 2)}));
}

// A fetching state made for a Depth32Float attachment, drawn with a depth test that writes in a pass of none: the pass is
// given its hidden depth/stencil attachment, which Apple's GPU tests depth against too, and the draw takes the test
// class of that attachment (DepthWrite), never the class of a pass with none
void hiddenDepthTestClass()
{
    RenderCompiler compiler;
    const std::string pack = writePack(packKey(MTLPixelFormatBGRA8Unorm));
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "variant-hidden-depth", pack, device);
    ASSERT_TRUE(library != nil);
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatBGRA8Unorm);
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:nil];
    ASSERT_TRUE(state != nil);
    gm20b::RenderVariant forms;
    forms.fragmentTests = uint8_t(gm20b::FragmentTests::DepthWrite);
    gm20b::Variant<gm20b::Pipeline> *variant = variantOf((SwitchMetalPipeline *)state, forms);
    ASSERT_NE(variant, nullptr);
    ASSERT_TRUE(ready(*variant));
    const uint32_t expected = program(variant->value()->methods, 2);
    const size_t firstJob = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:colourPass(device, false)];
    [encoder setRenderPipelineState:state];
    [encoder setDepthStencilState:depthWrite(device)];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    EXPECT_EQ(drawnPrograms(firstJob, 2), (std::vector<uint32_t>{expected}));
}

}  // namespace

TEST(GM20BFacadeVariant, AVariantNoCompilerMadeIsAskedOnceAndNamedOnce)
{
    switch_ios::test::ExpectPassesInFreshProcess(failedVariant);
}

TEST(GM20BFacadeVariant, AVariantAskedBeforeACompilerServesIsMadeOnceOneServes)
{
    switch_ios::test::ExpectPassesInFreshProcess(unservedVariant);
}

TEST(GM20BFacadeVariant, SwitchingStatesInAPassFindsTheirVariantsWithoutALock)
{
    switch_ios::test::ExpectPassesInFreshProcess(switchedStates);
}

TEST(GM20BFacadeVariant, APackedStatesTestClassThePackLacksIsCompiledAndDrawn)
{
    switch_ios::test::ExpectPassesInFreshProcess(packedTestClass);
}

TEST(GM20BFacadeVariant, AMadeStatesTestClassesAreMadeWithIt)
{
    switch_ios::test::ExpectPassesInFreshProcess(madeTestClasses);
}

TEST(GM20BFacadeVariant, AnInstancedDrawOfAStorageTestClassStaysRaw)
{
    switch_ios::test::ExpectPassesInFreshProcess(instancedDrawOfAStorageTestClass);
}

TEST(GM20BFacadeVariant, ADrawInAPassOfNoDepthTakesItsHiddenAttachmentsTestClass)
{
    switch_ios::test::ExpectPassesInFreshProcess(hiddenDepthTestClass);
}
