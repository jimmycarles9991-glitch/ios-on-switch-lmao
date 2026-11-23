// SPDX-License-Identifier: MIT
// Rule storage.compressed (storage_form.h) through the facade: a fetching state's storage form drawn into a private
// target on compressed memory views the target where it lies. The command buffer completes on the driver and the
// target is never moved to plain memory, in its first pass or a later one.
#include "facade_variant_compiler.h"
#include "bazel/testing/fresh_process.h"

#include <objc/message.h>
#include <unistd.h>

namespace {

using namespace harness;

constexpr uint32_t kR32UintWord0 = 0x6014920f;  // the storage view's word 0: R32_UINT, a 4-byte texel as RGBA8's

// The record of a state fetching colour 0 through its storage form (device_fetch_test.cpp fetchingRecord)
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

NSString *facts(id<MTLDevice> device, id<MTLTexture> texture)
{
    return ((id (*)(id, SEL, id))objc_msgSend)((id)device, sel_registerName("switchMetalProbeFacts:"), texture);
}

// A private RGBA8 target on compressed memory: the first of its size asks for the memory, a later one takes it
id<MTLTexture> compressedTarget(id<MTLDevice> device)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:256
                                                                                 height:256
                                                                              mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> asking = [device newTextureWithDescriptor:td];
    EXPECT_EQ(harness::compressionSettled(device), 1);
    id<MTLTexture> t = [device newTextureWithDescriptor:td];
    (void)asking;
    return [facts(device, t) containsString:@"comptags=1"] ? t : nil;
}

void aStorageFormDrawsItsCompressedTargetInPlace()
{
    {
        std::lock_guard<std::mutex> l(kernel_model::callLock);
        kernel_model::model.infoFlags |= TegraGM20BInfoCompression;
    }
    RenderCompiler compiler;
    compiler.shapes = storageForm;
    const std::string directory = dir() + "/storage-compressed-" + std::to_string(getpid());
    mkdir(directory.c_str(), 0700);
    ASSERT_TRUE(compiler.serve(directory));
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const std::string cache = directory + "/cache";
    mkdir(cache.c_str(), 0700);
    metal_complete_states(cache.c_str(), nullptr);
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:nil];
    ASSERT_TRUE(library != nil);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatRGBA8Unorm) error:nil];
    ASSERT_TRUE(state != nil);
    __unsafe_unretained SwitchMetalPipeline *pipeline = (SwitchMetalPipeline *)state;
    ASSERT_TRUE(pipeline->_raw);
    ASSERT_EQ(pipeline->_raw->storageBits, 1);
    id<MTLTexture> target = compressedTarget(device);
    ASSERT_TRUE(target != nil) << "no compressed memory was made";
    id<MTLCommandQueue> queue = [device newCommandQueue];
    for (const MTLLoadAction load : {MTLLoadActionClear, MTLLoadActionLoad}) {
        id<MTLCommandBuffer> commands = [queue commandBuffer];
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        p.colorAttachments[0].texture = target;
        p.colorAttachments[0].loadAction = load;
        p.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> encoder = [commands renderCommandEncoderWithDescriptor:p];
        [encoder setRenderPipelineState:state];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4 instanceCount:1];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:2];
        [encoder endEncoding];
        [commands commit];
        [commands waitUntilCompleted];
        EXPECT_EQ(commands.status, MTLCommandBufferStatusCompleted) << commands.error.description.UTF8String;
        NSString *path = ((NSString * (*)(id, SEL)) objc_msgSend)(commands, sel_registerName("switchMetalPath"));
        EXPECT_EQ(std::string(path.UTF8String ?: ""), "gm20b");
        EXPECT_TRUE([facts(device, target) containsString:@"comptags=1"])
            << "moved to plain memory: " << facts(device, target).UTF8String;
    }
}

}  // namespace

TEST(GM20BFacadeStorageCompressed, AStorageFormDrawsItsCompressedTargetInPlace)
{
    switch_ios::test::ExpectPassesInFreshProcess(aStorageFormDrawsItsCompressedTargetInPlace);
}
