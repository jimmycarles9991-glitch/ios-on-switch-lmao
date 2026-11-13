// SPDX-License-Identifier: MIT
// A process that draws only states the pack holds never opens its m2v device (RawM2V); each thing only m2v does opens it
// once, at its first use, naming it. Each scenario is a process of its own: one raw device and one m2v device a process.
#include "raw_facade_harness.h"
#include "MetalABI.h"
#include "bazel/testing/fresh_process.h"

#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <mach/vm_page_size.h>
#include <set>

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

Opened openWithLibrary(const std::string &pack, Class methods = [HarnessM2VMethods class])
{
    const std::string metallib = writeMetallib();
    id<MTLDevice> device = open(pack, methods);
    if (!device)
        return {};
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(metallib.c_str())] error:&error];
    return {device, library};
}

void expectOpenedFor(id<MTLDevice> device, const char *need)
{
    NSDictionary *m2v = m2vRecord(device);
    EXPECT_EQ(gOpens, 1);
    EXPECT_TRUE([m2v[@"opened"] boolValue]);
    EXPECT_EQ([m2v[@"opens"] intValue], 1);
    EXPECT_STREQ([m2v[@"opened_for"] UTF8String], need) << text(m2v);
}

void packedDraw()
{
    const uint64_t atStart = footprint();
    const std::string pack = writePack(packKey(kPacked));
    const auto [device, library] = openWithLibrary(pack);
    ASSERT_TRUE(device != nil && library != nil);
    const uint64_t atOpen = footprint();
    MTLRenderPipelineDescriptor *d = descriptor(library, kPacked);
    EXPECT_EQ(std::string(raw_metal_pipeline_key(d).UTF8String), packKey(kPacked));
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:d error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    id<MTLCommandBuffer> cb = draw(device, state, kPacked);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
    const uint64_t atDraw = footprint();

    NSDictionary *drivers = counters(device);
    EXPECT_EQ([drivers[@"raw"] intValue], 1) << text(drivers);
    EXPECT_EQ([drivers[@"m2v"] intValue], 0) << text(drivers);
    // every m2v object is made on the m2v device, and every way to it is a need: none asked, none opened, no twin
    NSDictionary *m2v = m2vRecord(device);
    EXPECT_EQ(gOpens, 0);
    EXPECT_FALSE([m2v[@"opened"] boolValue]);
    EXPECT_EQ([m2v[@"needs"] count], 0u) << text(m2v);

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

void unpackedState()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil && library != nil);
    NSError *error = nil;
    (void)[device newRenderPipelineStateWithDescriptor:descriptor(library, kUnpacked) error:&error];
    expectOpenedFor(device, "a render state's twin");
}

void reflection()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil && library != nil);
    MTLAutoreleasedRenderPipelineReflection reflected = nil;
    NSError *error = nil;
    id<MTLRenderPipelineState> state = [device newRenderPipelineStateWithDescriptor:descriptor(library, kPacked)
                                                                            options:MTLPipelineOptionArgumentInfo
                                                                         reflection:&reflected
                                                                              error:&error];
    EXPECT_TRUE(state != nil) << text(error);
    expectOpenedFor(device, "a render state's twin with its reflection");
}

void foreignSampler()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    MTLSamplerDescriptor *compared = [MTLSamplerDescriptor new];
    compared.compareFunction = MTLCompareFunctionLess;
    EXPECT_TRUE([(id)[device newSamplerStateWithDescriptor:compared] isKindOfClass:NSClassFromString(@"RMSampler")]);
    EXPECT_EQ(gOpens, 0);
    MTLSamplerDescriptor *s = [MTLSamplerDescriptor new];
    s.sAddressMode = MTLSamplerAddressModeClampToBorderColor;
    s.borderColor = MTLSamplerBorderColorOpaqueWhite;
    (void)[device newSamplerStateWithDescriptor:s];
    expectOpenedFor(device, "a sampler's twin");
}

// A method of m2v's device class the raw device has none of answers respondsToSelector: at once, opens the m2v device at
// its first call and runs m2v's method on the raw device, as the subclass of m2v's class did
void inheritedMethod()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    EXPECT_TRUE([(id)device respondsToSelector:@selector(harnessSelfRegistry)]);
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([(HarnessM2VMethods *)device harnessSelfRegistry], kRegistryID);
    expectOpenedFor(device, "m2v's -[MTLDevice harnessSelfRegistry]");
    EXPECT_EQ([(HarnessM2VMethods *)device harnessSelfRegistry], kRegistryID);
    EXPECT_EQ(gOpens, 1);
}

// A compute state the pack answers is made without m2v, its answers the pack's; one it has no answers of opens m2v
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
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
    MTLComputePipelineDescriptor *other = [MTLComputePipelineDescriptor new];
    other.computeFunction = kernel;
    other.maxTotalThreadsPerThreadgroup = 256;
    (void)[device newComputePipelineStateWithDescriptor:other
                                                options:MTLPipelineOptionNone
                                             reflection:nil
                                                  error:&error];
    EXPECT_EQ(gOpens, 1);
    EXPECT_NE(m2vRecord(device)[@"needs"][@"a compute state the pack has no facts of"], nil) << text(m2vRecord(device));
}

// A buffer over an IOSurface's pages and a linear texture's row are the raw device's own answers
void surfaceBufferAndLinearRow()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    id<HarnessDeviceSPI> spi = (id<HarnessDeviceSPI>)device;
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @64,
        (id)kIOSurfaceHeight : @64,
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfacePixelFormat : @(uint32_t('BGRA'))
    });
    ASSERT_TRUE(surface != nullptr);
    id<MTLBuffer> buffer = [spi newBufferWithIOSurface:surface];
    ASSERT_TRUE(buffer != nil);
    EXPECT_EQ(buffer.contents, IOSurfaceGetBaseAddress(surface));
    EXPECT_EQ(buffer.length % vm_page_size, 0u);
    EXPECT_GE(buffer.length, IOSurfaceGetAllocSize(surface));
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:100
                                                                                height:4
                                                                             mipmapped:NO];
    const NSUInteger align = [device minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatBGRA8Unorm];
    ASSERT_GT(align, 0u);
    EXPECT_EQ([spi requiredLinearTextureBytesPerRowForDescriptor:d], (400 + align - 1) / align * align);
    d.pixelFormat = MTLPixelFormatASTC_4x4_LDR;
    EXPECT_EQ([spi requiredLinearTextureBytesPerRowForDescriptor:d], 0u);
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
    buffer = nil;
    CFRelease(surface);
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
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
}

// An event signalled between two raw passes is released by the GPU where it falls (raw_inline_signals.h) on a kernel with
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
    EXPECT_EQ([counters(device)[@"m2v"] intValue], 0) << text(counters(device));
    EXPECT_EQ(gOpens, 0);
}

// A fence and a binary archive are the device's own objects (RMFence, RMBinaryArchive), made without asking m2v
void fenceAndArchive()
{
    const auto [device, library] = openWithLibrary(writePack(packKey(kPacked)));
    ASSERT_TRUE(device != nil);
    id<MTLFence> fence = [device newFence];
    ASSERT_TRUE(fence != nil);
    EXPECT_STREQ(object_getClassName(fence), "RMFence");
    EXPECT_EQ(fence.device, device);
    NSError *error = nil;
    id<MTLBinaryArchive> archive =
        [device newBinaryArchiveWithDescriptor:[MTLBinaryArchiveDescriptor new] error:&error];
    ASSERT_TRUE(archive != nil) << error.localizedDescription.UTF8String;
    EXPECT_STREQ(object_getClassName(archive), "RMBinaryArchive");
    EXPECT_EQ(archive.device, device);
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(device)[@"needs"] count], 0u) << text(m2vRecord(device));
}

}  // namespace

TEST(RawFacadeLazy, PackedDrawOpensNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(packedDraw);
    show("packed_draw");
}

TEST(RawFacadeLazy, UnpackedStateOpensM2VForItsTwin)
{
    switch_ios::test::ExpectPassesInFreshProcess(unpackedState);
}

TEST(RawFacadeLazy, ReflectionOpensM2VForItsTwin)
{
    switch_ios::test::ExpectPassesInFreshProcess(reflection);
}

TEST(RawFacadeLazy, ForeignSamplerOpensM2VForItsTwin)
{
    switch_ios::test::ExpectPassesInFreshProcess(foreignSampler);
}

TEST(RawFacadeLazy, InheritedMethodOpensM2VAtItsFirstCall)
{
    switch_ios::test::ExpectPassesInFreshProcess(inheritedMethod);
}

TEST(RawFacadeLazy, AComputeStateThePackAnswersOpensNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(computeStates);
}

TEST(RawFacadeLazy, SurfaceBufferAndLinearRowOpenNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(surfaceBufferAndLinearRow);
}

TEST(RawFacadeLazy, DeviceIsItsOwn)
{
    switch_ios::test::ExpectPassesInFreshProcess(ownDevice);
}

TEST(RawFacadeLazy, FenceAndBinaryArchiveAreTheDevicesOwn)
{
    switch_ios::test::ExpectPassesInFreshProcess(fenceAndArchive);
}

TEST(RawFacadeLazy, ASignalBetweenTwoPassesSplitsNoJob)
{
    switch_ios::test::ExpectPassesInFreshProcess(signalBetweenPasses);
}
