// SPDX-License-Identifier: MIT
// States, reflections, samplers and resources a process makes on the device, each answered by the driver alone. Each
// scenario is a process of its own: one raw device a process.
#include "facade_harness.h"
#include "reflection_blob_writer.h"
#include "MetalABI.h"
#include "bazel/testing/fresh_process.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <cmath>
#include <mach/vm_page_size.h>
#include <algorithm>
#include <set>
#include <vector>

@protocol HarnessDeviceSPI
- (id<MTLBuffer>)newBufferWithIOSurface:(IOSurfaceRef)surface;
- (NSUInteger)requiredLinearTextureBytesPerRowForDescriptor:(MTLTextureDescriptor *)descriptor;
@end

namespace {

using namespace harness;

constexpr MTLPixelFormat kPacked = MTLPixelFormatBGRA8Unorm, kUnpacked = MTLPixelFormatRGBA8Unorm;

void note(const char *scenario, const std::string &line)
{
    std::ofstream(dir() + "/" + scenario + ".txt", std::ios::app) << line << "\n";
}

void show(const char *scenario)
{
    std::ifstream in(dir() + "/" + scenario + ".txt");
    for (std::string line; std::getline(in, line);)
        printf("%s: %s\n", scenario, line.c_str());
}

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

struct Opened {
    id<MTLDevice> device;
    id<MTLLibrary> library;
};

Opened openWithLibrary(const std::string &pack)
{
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(pack);
    if (!device)
        return {};
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    return {device, library};
}

void packedDraw()
{
    const uint64_t atStart = footprint();
    const std::string pack = writePack(packKey(kPacked));
    const auto [device, library] = openWithLibrary(pack);
    ASSERT_TRUE(device != nil && library != nil);
    const uint64_t atOpen = footprint();
    MTLRenderPipelineDescriptor *d = descriptor(library, kPacked);
    EXPECT_EQ(std::string(metal_pipeline_key(d).UTF8String), packKey(kPacked));
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    id<MTLCommandBuffer> cb = draw(device, state, kPacked);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    const uint64_t atDraw = footprint();

    NSDictionary *drivers = counters(device);
    EXPECT_EQ([drivers[@"gm20b"] intValue], 1) << text(drivers);

    // both programs run in place: bound imports of the pack file's own pages, holding what the file holds there
    NSDictionary *held = memory(device);
    EXPECT_EQ([held[@"heap"] unsignedLongLongValue], 0u) << text(held);
    EXPECT_GE([held[@"heap_in_place"] unsignedLongLongValue], 2 * kSlot) << text(held);
    const std::vector<FileImport> imports = boundFileImports(pack);
    ASSERT_FALSE(imports.empty());
    for (uint32_t k = 0; k < 2; k++) {
        const uint64_t at = kHeap + k * kSlot;
        const FileImport *in = nullptr;
        for (const FileImport &i : imports)
            if (at >= i.offset && at + kSlot <= i.offset + i.bytes)
                in = &i;
        ASSERT_NE(in, nullptr) << "program slot " << k << " is in no bound import of the pack";
        const std::vector<uint8_t> file = fileBytes(pack, at, kSlot);
        EXPECT_EQ(memcmp(reinterpret_cast<const void *>(in->address + (at - in->offset)), file.data(), kSlot), 0);
        EXPECT_EQ(file, slot(k));
    }

    char line[256];
    snprintf(
        line, sizeof line,
        "phys_footprint start %llu KB, raw device open %+lld KB, packed draw %+lld KB; currentAllocatedSize %llu KB; "
        "%s",
        (unsigned long long)(atStart >> 10), (long long)(int64_t(atOpen) - int64_t(atStart)) / 1024,
        (long long)(int64_t(atDraw) - int64_t(atOpen)) / 1024, (unsigned long long)(device.currentAllocatedSize >> 10),
        [[held.description stringByReplacingOccurrencesOfString:@"\n" withString:@" "] UTF8String]);
    note("packed_draw", line);
}

// A state of this device's functions that no record draws (the pack lacks it and no shader compiler serves the process)
// fails its creation with Metal's error, as a state Metal could not compile; it is never m2v's
void unpackedState()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil && library != nil);
    NSError *error = nil;
    EXPECT_EQ([device newRenderPipelineStateWithDescriptor:descriptor(library, kUnpacked) error:&error], nil);
    EXPECT_EQ(error.domain, MTLLibraryErrorDomain);
}

// One buffer binding, "uniforms" at index 2, as a blob of `stages` stages
std::string oneBinding(uint32_t stages)
{
    NmbReflectionNode root = {}, binding = {};
    root.count = 1;
    binding.kind = gm20b::ReflectionBinding;
    binding.name = "uniforms";
    binding.flags = 1;
    binding.values[0] = MTLBindingTypeBuffer;
    binding.values[2] = 2;
    for (NmbReflectionNode *n : {&root, &binding})
        std::fill(std::begin(n->children), std::end(n->children), gm20b::kReflectionAbsent);
    return reflection_blob::of({root, binding}, {1}, stages);
}

// A render and a compute state's reflections asked with argument info are the pack's (PackReflections): no m2v device
// opens, and without the option none is given
void reflection()
{
    const std::string metallib = writeMetallib();
    const uint64_t kernelState = computeStateHash(metallib, "k");
    const std::string key = packKey(kPacked);
    const std::string reflections = reflection_blob::section(
        {{gm20b::Pack::fnv(key.data(), key.size()), false, oneBinding(5)}, {kernelState, true, oneBinding(1)}});
    id<MTLDevice> device = open(writePack(key, {}, {{kernelState, 768, 32, 1024, 0}}, true, reflections));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    MTLAutoreleasedRenderPipelineReflection reflected = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked)
                                                                            options:MTLPipelineOptionArgumentInfo
                                                                         reflection:&reflected
                                                                              error:&error];
    EXPECT_TRUE(state != nil) << text(error);
    ASSERT_EQ(reflected.vertexBindings.count, 1u);
    EXPECT_STREQ(reflected.vertexBindings[0].name.UTF8String, "uniforms");
    EXPECT_EQ(reflected.vertexBindings[0].index, 2u);
    EXPECT_EQ(reflected.fragmentBindings.count, 0u);
    MTLAutoreleasedComputePipelineReflection computed = nil;
    id<MTLComputePipelineState> kernel = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"k"]
                                                                             options:MTLPipelineOptionBufferTypeInfo
                                                                          reflection:&computed
                                                                               error:&error];
    EXPECT_TRUE(kernel != nil) << text(error);
    ASSERT_EQ(computed.bindings.count, 1u);
    EXPECT_STREQ(computed.bindings[0].name.UTF8String, "uniforms");
    MTLAutoreleasedRenderPipelineReflection unasked = nil;
    (void)[device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked)
                                               options:MTLPipelineOptionNone
                                            reflection:&unasked
                                                 error:&error];
    EXPECT_EQ(unasked, nil);
}

void foreignSampler()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    MTLSamplerDescriptor *compared = [MTLSamplerDescriptor new];
    compared.compareFunction = MTLCompareFunctionLess;
    EXPECT_TRUE(
        [(id)[device newSamplerStateWithDescriptor:compared] isKindOfClass:NSClassFromString(@"SwitchMetalSampler")]);
    MTLSamplerDescriptor *white = [MTLSamplerDescriptor new];
    white.sAddressMode = MTLSamplerAddressModeClampToBorderColor;
    white.borderColor = MTLSamplerBorderColorOpaqueWhite;
    EXPECT_TRUE(
        [(id)[device newSamplerStateWithDescriptor:white] isKindOfClass:NSClassFromString(@"SwitchMetalSampler")])
        << "an opaque border is the raw sampler's";
    MTLSamplerDescriptor *mirrored = [MTLSamplerDescriptor new];
    mirrored.normalizedCoordinates = NO;
    mirrored.sAddressMode = MTLSamplerAddressModeMirrorClampToEdge;
    EXPECT_TRUE(
        [(id)[device newSamplerStateWithDescriptor:mirrored] isKindOfClass:NSClassFromString(@"SwitchMetalSampler")])
        << "pixel coordinates mirrored once are the raw sampler's, clamped to the edge";
    MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
    s.lodMinClamp = NAN;
    EXPECT_EQ([device newSamplerStateWithDescriptor:s], nil) << "a sampler the device has no form of is nil";
}

// A compute state the pack answers is made without m2v, its answers the pack's; one it has no answers of is the shader
// compiler's (facade_open_test), and with none this boot fails its creation, opening no m2v
void computeStates()
{
    const std::string metallib = writeMetallib();
    const uint64_t state = computeStateHash(metallib, "k");
    ASSERT_NE(state, 0u);
    id<MTLDevice> device = open(writePack(packKey(kPacked), {}, {{state, 768, 32, 1024, 0}}));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    id<MTLFunction> kernel = [library newFunctionWithName:@"k"];
    ASSERT_TRUE(kernel != nil) << text(error);
    id<MTLComputePipelineState> made = [device newComputePipelineStateWithFunction:kernel error:&error];
    ASSERT_TRUE(made != nil) << text(error);
    EXPECT_EQ(made.maxTotalThreadsPerThreadgroup, 768u);
    EXPECT_EQ(made.threadExecutionWidth, 32u);
    EXPECT_EQ(made.staticThreadgroupMemoryLength, 1024u);
    MTLComputePipelineDescriptor *other = [MTLComputePipelineDescriptor new];
    other.computeFunction = kernel;
    other.maxTotalThreadsPerThreadgroup = 256;
    EXPECT_EQ(
        [device newComputePipelineStateWithDescriptor:other options:MTLPipelineOptionNone reflection:nil error:&error],
        nil);
    EXPECT_NE(error, nil);
}

// A buffer over an IOSurface's pages and a linear texture's row are the device's own answers
void surfaceBufferAndLinearRow()
{
    const Opened opened = openWithLibrary(writePack(packKey(kPacked)));
    const id<MTLDevice> device = opened.device;
    ASSERT_TRUE(device != nil);
    id<HarnessDeviceSPI> spi = (id<HarnessDeviceSPI>)device;
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> copy = [device newBufferWithLength:4 options:MTLResourceStorageModeShared];
    ASSERT_TRUE(queue != nil && copy != nil);
    for (NSUInteger height : {64u, 80u, 96u, 112u}) {
        IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
            (id)kIOSurfaceWidth : @64,
            (id)kIOSurfaceHeight : @(height),
            (id)kIOSurfaceBytesPerElement : @4,
            (id)kIOSurfaceBytesPerRow : @256,
            (id)kIOSurfaceAllocSize : @(height * 256),
            (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
        });
        ASSERT_TRUE(surface != nullptr);
        id<MTLBuffer> buffer = [spi newBufferWithIOSurface:surface];
        ASSERT_TRUE(buffer != nil);
        EXPECT_EQ(buffer.contents, IOSurfaceGetBaseAddress(surface));
        EXPECT_EQ(buffer.length % vm_page_size, 0u);
        EXPECT_GE(buffer.length, IOSurfaceGetAllocSize(surface));
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        ASSERT_TRUE(blit != nil);
        [blit copyFromBuffer:buffer sourceOffset:0 toBuffer:copy destinationOffset:0 size:4];
        [blit endEncoding];
        const size_t before = kernel_model::model.jobs.size();
        const bool refused = kernel_model::model.unknownUsesRefused;
        kernel_model::model.unknownUsesRefused = true;
        [cb commit];
        [cb waitUntilCompleted];
        kernel_model::model.unknownUsesRefused = refused;
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
        EXPECT_GT(kernel_model::model.jobs.size(), before);
        buffer = nil;
        CFRelease(surface);
    }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:100
                                                                                height:4
                                                                             mipmapped:NO];
    const NSUInteger align = [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm];
    ASSERT_GT(align, 0u);
    EXPECT_EQ([spi requiredLinearTextureBytesPerRowForDescriptor:d], (400 + align - 1) / align * align);
    d.pixelFormat = MTLPixelFormatASTC_4x4_LDR;
    EXPECT_EQ([spi requiredLinearTextureBytesPerRowForDescriptor:d], 0u);
    const auto rowOf = [&](NSUInteger format) {
        d.pixelFormat = MTLPixelFormat(format);
        return [spi requiredLinearTextureBytesPerRowForDescriptor:d];
    };
    const auto aligned = [&](NSUInteger format, NSUInteger bytes) {
        const NSUInteger a = [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormat(format)];
        return a ? (100 * bytes + a - 1) / a * a : 0;
    };
    EXPECT_GT(aligned(MTLPixelFormatR8Snorm, 1), 0u);
    EXPECT_EQ(rowOf(MTLPixelFormatR8Snorm), aligned(MTLPixelFormatR8Snorm, 1));
    EXPECT_EQ(rowOf(MTLPixelFormatRGBA16Snorm), aligned(MTLPixelFormatRGBA16Snorm, 8));
    EXPECT_EQ(rowOf(26), aligned(26, 2));    // R16Bfloat
    EXPECT_EQ(rowOf(550), aligned(550, 8));  // RGB10A8_2P_XR10, raw's texel, not Metal's plane of 2
    EXPECT_EQ(rowOf(MTLPixelFormatDepth32Float), 0u);
}

// A heap's block-compressed textures (a chain, an array, and a single level in a shared heap, block-linear as the device
// makes it) are placed in its arena as the device lays them out, each one's size the device's answer (heapTextureAsk)
void heapBlockTextures()
{
    const Opened opened = openWithLibrary(writePack(packKey(kPacked)));
    const id<MTLDevice> device = opened.device;
    ASSERT_TRUE(device != nil);
    const auto heapOf = [&](MTLStorageMode mode) {
        MTLHeapDescriptor *h = [MTLHeapDescriptor new];
        h.storageMode = mode;
        h.size = 16 << 20;
        return [device newHeapWithDescriptor:h];
    };
    const id<MTLHeap> privateHeap = heapOf(MTLStorageModePrivate), sharedHeap = heapOf(MTLStorageModeShared);
    ASSERT_TRUE(privateHeap != nil && sharedHeap != nil);
    const auto described = [](MTLPixelFormat format, NSUInteger size, BOOL mipmapped) {
        return [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                                  width:size
                                                                 height:size * 3 / 5
                                                              mipmapped:mipmapped];
    };
    MTLTextureDescriptor *mips = described(MTLPixelFormatASTC_4x4_LDR, 100, YES),
                         *layers = described(MTLPixelFormatBC1_RGBA, 64, NO),
                         *single = described(MTLPixelFormatBC1_RGBA, 64, NO);
    layers.textureType = MTLTextureType2DArray;
    layers.arrayLength = 3;
    mips.storageMode = layers.storageMode = MTLStorageModePrivate;
    single.storageMode = MTLStorageModeShared;
    for (MTLTextureDescriptor *d in @[ mips, layers, single ]) {
        const MTLSizeAndAlign need = [device heapTextureSizeAndAlignWithDescriptor:d];
        EXPECT_GT(need.size, 0u);
        id<MTLTexture> t = [(d == single ? sharedHeap : privateHeap) newTextureWithDescriptor:d];
        ASSERT_TRUE([(id)t isKindOfClass:[SwitchMetalTexture class]]) << text(t);
        EXPECT_TRUE(((SwitchMetalTexture *)t)->_raw.arena);
        EXPECT_TRUE(((SwitchMetalTexture *)t)->_raw.blockLinear);
        EXPECT_EQ(((SwitchMetalTexture *)t)->_raw.span.bytes, need.size);
    }
}

// A CPU copy of a memoryless texture moves nothing, as Apple's driver: getBytes leaves the bytes, replaceRegion drops them
void memorylessCpuCopies()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:16
                                                                                height:16
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeMemoryless;
    d.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> t = [device newTextureWithDescriptor:d];
    ASSERT_TRUE(t != nil);
    std::vector<uint8_t> in(16 * 16 * 4, 0x5a), out(16 * 16 * 4, 0xab);
    [t replaceRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0 withBytes:in.data() bytesPerRow:64];
    [t getBytes:out.data() bytesPerRow:64 fromRegion:MTLRegionMake2D(0, 0, 16, 16) mipmapLevel:0];
    EXPECT_EQ(std::count(out.begin(), out.end(), 0xab), ptrdiff_t(out.size()));
}

// The device is an _MTLDevice of its own; its identity and every limits field m2v's device answered are its own answers
void ownDevice()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    Class cls = object_getClass(device), base = class_getSuperclass(cls);
    EXPECT_EQ(base, NSClassFromString(@"_MTLDevice") ?: [NSObject class]);
    EXPECT_TRUE([(id)device conformsToProtocol:@protocol(MTLDevice)]);
    EXPECT_EQ(device.registryID, kRegistryID);
    // fields no driver answers (m2v's device class had no method for them either): the base class's own value
    const std::set<std::string> unanswered = {"linearTextureArrayAlignmentBytes",
                                              "linearTextureArrayAlignmentSlice",
                                              "maxTileInlineDataSize",
                                              "minTilePixels",
                                              "maxLineWidth",
                                              "maxPointSize",
                                              "padmaxBufferLength",
                                              "maxIOCommandsInFlight",
                                              "maxAccelerationStructureLevels"};
    for (const LimitField &field : kLimitFields) {
        const SEL sel = sel_registerName(field.selector);
        Method own = class_getInstanceMethod(cls, sel), inherited = class_getInstanceMethod(base, sel);
        const bool answered =
            own && (!inherited || method_getImplementation(own) != method_getImplementation(inherited));
        EXPECT_EQ(answered, !unanswered.count(field.selector)) << field.selector;
    }
}

// An event signalled between two raw passes is released by the GPU where it falls (inline_signals.h) on a kernel with
// event words: the command buffer goes to the kernel as the same jobs it makes with no signal, and stays raw. On a kernel
// without them the signal ends its job
void signalBetweenPasses()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil && library != nil);
    NSError *error = nil;
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kPacked width:64 height:64 mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    id<MTLEvent> event = [device newEvent];
    ASSERT_TRUE(event != nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    const auto jobs = [&](uint64_t signal) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        for (int pass = 0; pass < 2; pass++) {
            MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
            p.colorAttachments[0].texture = target;
            p.colorAttachments[0].loadAction = MTLLoadActionLoad;
            p.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:p];
            [e setRenderPipelineState:state];
            const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
            [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [e endEncoding];
            if (signal && !pass)
                [cb encodeSignalEvent:event value:signal];
        }
        const size_t before = kernel_model::model.jobs.size();
        [cb commit];
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
        return kernel_model::model.jobs.size() - before;
    };
    jobs(0);
    const size_t plain = jobs(0);
    EXPECT_GE(plain, 1u);
    EXPECT_EQ(jobs(1), plain + 1) << "a kernel without event words: the signal ends its job";
    EXPECT_TRUE(kernel_model::model.eventWords.empty());
    kernel_model::model.kernelEvents = true;
    EXPECT_EQ(jobs(2), plain);
    EXPECT_FALSE(kernel_model::model.eventWords.empty());
}

// A fence and a binary archive are the device's own objects (SwitchMetalFence, SwitchMetalBinaryArchive), made without asking m2v
void fenceAndArchive()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    id<MTLFence> fence = [device newFence];
    ASSERT_TRUE(fence != nil);
    EXPECT_STREQ(object_getClassName(fence), "SwitchMetalFence");
    EXPECT_EQ(fence.device, device);
    NSError *error = nil;
    id<MTLBinaryArchive> archive =
        [device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error];
    ASSERT_TRUE(archive != nil) << error.localizedDescription.UTF8String;
    EXPECT_STREQ(object_getClassName(archive), "SwitchMetalBinaryArchive");
    EXPECT_EQ(archive.device, device);
}

}  // namespace

TEST(GM20BFacadeLazy, PackedDrawOpensNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(packedDraw);
    show("packed_draw");
}

TEST(GM20BFacadeLazy, AnUnpackedStateIsMetalsCreationFailure)
{
    switch_ios::test::ExpectPassesInFreshProcess(unpackedState);
}

TEST(GM20BFacadeLazy, ReflectionsOfPackedStatesOpenNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(reflection);
}

TEST(GM20BFacadeLazy, ForeignSamplerIsNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(foreignSampler);
}

TEST(GM20BFacadeLazy, AComputeStateThePackAnswersOpensNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(computeStates);
}

TEST(GM20BFacadeLazy, SurfaceBufferAndLinearRowOpenNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(surfaceBufferAndLinearRow);
}

TEST(GM20BFacadeLazy, AHeapsBlockCompressedTexturesArePlacedInItsArena)
{
    switch_ios::test::ExpectPassesInFreshProcess(heapBlockTextures);
}

TEST(GM20BFacadeLazy, ACpuCopyOfAMemorylessTextureMovesNothing)
{
    switch_ios::test::ExpectPassesInFreshProcess(memorylessCpuCopies);
}

TEST(GM20BFacadeLazy, DeviceIsItsOwn)
{
    switch_ios::test::ExpectPassesInFreshProcess(ownDevice);
}

TEST(GM20BFacadeLazy, FenceAndBinaryArchiveAreTheDevicesOwn)
{
    switch_ios::test::ExpectPassesInFreshProcess(fenceAndArchive);
}

TEST(GM20BFacadeLazy, ASignalBetweenTwoPassesSplitsNoJob)
{
    switch_ios::test::ExpectPassesInFreshProcess(signalBetweenPasses);
}
