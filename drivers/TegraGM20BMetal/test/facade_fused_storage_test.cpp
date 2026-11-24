// SPDX-License-Identifier: MIT
// A state's layered storage form drawn in a one-layer pass that loads a linear texture over a buffer (HW Test
// metal.native.layered_fetch, one_layer_linear): the pass renders the texture through a block-linear backing, whose 2D
// array storage view the fetch reads, also where it follows a clear pass of the texture in the same command buffer,
// whose begin laid the texture out as a pitch target (TegraGM20BMetalRangeSeal.mm storageLaidOut: no fusion into that pass).
// A state's storage form of one layer drawn after a clear pass of a private block-linear texture is fused into that pass,
// whose begin laid the texture out as the form binds it (HW Test gpu.gm20b.pass_fusion's fetched pass), and so is one
// that tests a Depth32Float in its shader (that scene's depth arms): only a Depth16Unorm's begin differs then.
#include "facade_pass_scene.h"
#include "bazel/testing/fresh_process.h"

#include <unistd.h>

namespace {

using namespace harness;

constexpr uint32_t kR32UintWord0 = 0x6014920f;  // the storage view's word 0: R32_UINT, a 4-byte texel as RGBA8's

// The record of a state fetching colour 0 through its storage form (device_layered_fetch_test.cpp fetching)
void storageForm(const gm20b::CompileRequest &, gm20b::RuntimeRecord &record)
{
    gm20b::PackPipeline &pp = record.pipeline;
    gm20b::setEntryCount(pp, 3);
    pp.entry[2] = {32, gm20b::EntryFetch, 2, 0, gm20b::SamplerNone, 0, 0};
    pp.setBytes = 48;
    pp.memoryEntries = 7;
    pp.flags |= gm20b::PipelineFetches | gm20b::PipelineStorageMetadata;
    pp.storageMask = pp.storageWriteMask = 1;
    pp.storageSamples = 1;
    pp.storageTic[0] = kR32UintWord0;
    record.blob = {0x1224, 1, 0x1a00, 0};
}

// Its layered form: its vertex program writes Layer, the pass's length goes into push word 2
void layeredStorageForm(const gm20b::CompileRequest &request, gm20b::RuntimeRecord &record)
{
    storageForm(request, record);
    record.pipeline.arrayLengthWord = 3;
    record.pipeline.pushMask = 1u << 2;
    record.programs[0].image[53] |= 2;
}

id<MTLLibrary> served(RenderCompiler &compiler, const std::string &name, const std::string &pack,
                      id<MTLDevice> __strong &device)
{
    const std::string directory = dir() + "/" + name + "-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    if (!compiler.serve(directory) || !(device = open(pack)))
        return nil;
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    return [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
}

MTLRenderPassDescriptor *pass(id<MTLTexture> t, MTLLoadAction load)
{
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    p.colorAttachments[0].texture = t;
    p.colorAttachments[0].loadAction = load;
    p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
    return p;
}

long long fusedPasses(id<MTLDevice> device)
{
    return [counters(device)[@"fused_passes"] longLongValue];
}

// A clear pass of `target`, then a pass that loads it and draws `state` once, in one command buffer: the passes the
// driver fused
long long clearThenDraw(id<MTLDevice> device, id<MTLRenderPipelineState> state, id<MTLTexture> target)
{
    const long long before = fusedPasses(device);
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    [[commands renderCommandEncoderWithDescriptor:pass(target, MTLLoadActionClear)] endEncoding];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:pass(target, MTLLoadActionLoad)];
    [encoder setRenderPipelineState:state];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:1];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(commands, sel_registerName("switchMetalPath"));
    EXPECT_EQ(std::string(path.UTF8String ?: ""), "gm20b");
    return fusedPasses(device) - before;
}

void layeredFetchIntoALinearTextureAfterItsClear()
{
    RenderCompiler compiler;
    compiler.shapes = layeredStorageForm;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "fused-storage", writePack(packKey(MTLPixelFormatBGRA8Unorm)), device);
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatRGBA8Unorm) error:nil];
    ASSERT_TRUE(state != nil);
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    ASSERT_TRUE(pipeline->_raw);
    ASSERT_EQ(pipeline->_raw->storageBits, 1);
    ASSERT_TRUE(gm20b::layeredRecord(pipeline->_raw->rec->arrayLengthWord));
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:32
                                                                                height:32
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLBuffer> rows = [device newBufferWithLength:32 * 128 options:MTLResourceStorageModeShared];
    id<MTLTexture> linear = [rows newTextureWithDescriptor:d offset:0 bytesPerRow:128];
    ASSERT_TRUE(linear != nil);
    ASSERT_FALSE(((SwitchMetalTexture *)linear)->_raw.blockLinear);
    EXPECT_EQ(clearThenDraw(device, state, linear), 0) << "the layered form was fused into the clear's pitch-laid pass";
}

void storageFetchIntoAPrivateTextureAfterItsClear()
{
    RenderCompiler compiler;
    compiler.shapes = storageForm;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library =
        served(compiler, "fused-storage-private", writePack(packKey(MTLPixelFormatBGRA8Unorm)), device);
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatRGBA8Unorm) error:nil];
    ASSERT_TRUE(state != nil);
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    ASSERT_TRUE(pipeline->_raw);
    ASSERT_EQ(pipeline->_raw->storageBits, 1);
    ASSERT_FALSE(gm20b::layeredRecord(pipeline->_raw->rec->arrayLengthWord));
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:32
                                                                                height:32
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:d];
    ASSERT_TRUE(target != nil);
    ASSERT_TRUE(((SwitchMetalTexture *)target)->_raw.blockLinear);
    EXPECT_EQ(clearThenDraw(device, state, target), 1) << "the storage form's pass was not fused into the clear's";
}

void shaderDepthTestAfterAClearOfItsDepth32Float()
{
    RenderCompiler compiler;
    compiler.shapes = pass_scene::testsDepthInItsShader;
    id<MTLDevice> device = nil;
    id<MTLLibrary> library = served(compiler, "fused-depth", writePack(packKey(MTLPixelFormatBGRA8Unorm)), device);
    ASSERT_TRUE(library != nil);
    MTLRenderPipelineDescriptor *d = descriptor(library, MTLPixelFormatBGRA8Unorm);
    d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:nil];
    __unsafe_unretained SwitchMetalPipeline *made = (SwitchMetalPipeline *)state;
    ASSERT_TRUE(made && made->_raw && made->_raw->rec &&
                (made->_raw->rec->shaderTests.flags & gm20b::kShaderTestDepth));
    id<MTLTexture> colour = pass_scene::attachmentTexture(device, MTLPixelFormatBGRA8Unorm, 32, 32);
    id<MTLTexture> depth = pass_scene::attachmentTexture(device, MTLPixelFormatDepth32Float, 32, 32);
    ASSERT_TRUE(colour != nil && depth != nil);
    MTLRenderPassDescriptor *loads = pass_scene::pass(colour, depth, MTLLoadActionLoad);
    loads.colorAttachments[0].loadAction = MTLLoadActionLoad;
    const long long before = fusedPasses(device);
    id<MTLCommandBuffer> commands = [[device newCommandQueue] commandBuffer];
    [[commands renderCommandEncoderWithDescriptor:pass_scene::pass(colour, depth, MTLLoadActionClear)] endEncoding];
    id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:loads];
    [encoder setRenderPipelineState:state];
    [encoder setDepthStencilState:pass_scene::depthWrite(device)];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:1];
    [encoder endEncoding];
    [commands commit];
    [commands waitUntilCompleted];
    EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
    EXPECT_EQ(pass_scene::path(commands), "gm20b");
    EXPECT_EQ(fusedPasses(device) - before, 1) << "the shader-tested Depth32Float pass was not fused into the clear's";
}

}  // namespace

TEST(GM20BFacadeFusedStorage, ALayeredFetchIntoALinearTextureAfterItsClearIsRaws)
{
    switch_ios::test::ExpectPassesInFreshProcess(layeredFetchIntoALinearTextureAfterItsClear);
}

TEST(GM20BFacadeFusedStorage, AStorageFetchIntoAPrivateTextureFusesWithItsClear)
{
    switch_ios::test::ExpectPassesInFreshProcess(storageFetchIntoAPrivateTextureAfterItsClear);
}

TEST(GM20BFacadeFusedStorage, AStorageFormTestingADepth32FloatInItsShaderFusesWithItsClear)
{
    switch_ios::test::ExpectPassesInFreshProcess(shaderDepthTestAfterAClearOfItsDepth32Float);
}
