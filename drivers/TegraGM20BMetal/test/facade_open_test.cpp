// SPDX-License-Identifier: MIT
// What no process opens its m2v device for: a library's own answers, a library Metal would not load, memory the kernel
// has none of, pages it cannot wire, a YCbCr target state raw draws, texture layouts and a full sampler header table. Each
// answer is the device's or Metal's nil.
#include "facade_harness.h"
#include "../srgb_pair.h"
#include "../ycbcr_store.h"
#include "bazel/testing/fresh_process.h"

#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#include <gtest/gtest.h>
#include <sys/mman.h>

namespace {

using namespace harness;

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

std::string writeFile(const std::string &name, const std::string &bytes)
{
    const std::string path = dir() + "/" + name;
    std::ofstream(path, std::ios::binary).write(bytes.data(), std::streamsize(bytes.size()));
    return path;
}

NSURL *fileURL(const std::string &path)
{
    return [NSURL fileURLWithPath:@(path.c_str())];
}

NSArray<NSString *> *externNames(id<MTLLibrary> library)
{
    return ((NSArray<NSString *> * (*)(id, SEL)) objc_msgSend)(library, sel_registerName("externFunctionNames"));
}

void libraryAnswers()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> kernels = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    ASSERT_TRUE(kernels != nil) << text(error);
    EXPECT_TRUE([kernels.functionNames isEqualToArray:(@[ @"v", @"f", @"k" ])]) << text(kernels.functionNames);
    EXPECT_EQ(externNames(kernels).count, 0u);
    EXPECT_EQ(kernels.type, MTLLibraryTypeExecutable);
    EXPECT_EQ([kernels newFunctionWithName:@"k"].options, MTLFunctionOptionNone);

    const std::string ci = made_metallib::universal({"tint", "swap_rb"}, 0xB0, 0xEE, {5, 5});
    id<MTLLibrary> externs = [device newLibraryWithFile:@(writeFile("ci.metallib", ci).c_str()) error:&error];
    ASSERT_TRUE(externs != nil) << text(error);
    EXPECT_EQ(externs.functionNames.count, 0u);
    EXPECT_TRUE([externNames(externs) isEqualToArray:(@[ @"tint", @"swap_rb" ])]) << text(externNames(externs));
    id<MTLFunction> tint =
        ((id (*)(id, SEL, NSString *))objc_msgSend)(externs, sel_registerName("newExternFunctionWithName:"), @"tint");
    ASSERT_TRUE(tint != nil) << "Core Image's extern, its filter link's input";
    EXPECT_TRUE([tint.name isEqualToString:@"tint"]);
    EXPECT_NE([externs newFunctionWithName:@"swap_rb"], nil);
    const std::string exported = made_metallib::universal({"exported"}, 0xC0, 0xEE, {3});
    id<MTLLibrary> dylib = [device newLibraryWithFile:@(writeFile("dylib.metallib", exported).c_str()) error:&error];
    ASSERT_TRUE(dylib != nil) << text(error);
    EXPECT_EQ([dylib newFunctionWithName:@"exported"], nil) << "a dynamic library's export makes no function";
    EXPECT_EQ([dylib newFunctionWithName:@"exported" constantValues:[MTLFunctionConstantValues new] error:&error], nil);
    EXPECT_EQ(error.code, MTLLibraryErrorFunctionNotFound);

    const std::string none = made_metallib::universal({});
    id<MTLLibrary> empty = [device newLibraryWithURL:fileURL(writeFile("none.metallib", none)) error:&error];
    ASSERT_TRUE(empty != nil) << "a metallib that lists no function loads: " << text(error);
    EXPECT_EQ(empty.functionNames.count, 0u);
    EXPECT_EQ([empty newFunctionWithName:@"v"], nil);
}

void unreadLibraries()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    EXPECT_EQ([device newLibraryWithURL:fileURL(dir() + "/absent.metallib") error:&error], nil);
    EXPECT_NE(error, nil);
    error = nil;
    EXPECT_EQ([device newLibraryWithURL:[NSURL URLWithString:@"https://example.invalid/default.metallib"] error:&error],
              nil);
    EXPECT_NE(error, nil);
    error = nil;
    EXPECT_EQ([device newLibraryWithFile:@(writeFile("text.metallib", "not a metallib").c_str()) error:&error], nil);
    EXPECT_EQ(error.code, MTLLibraryErrorUnsupported) << text(error);
    std::string noAir = made_metallib::universal({"f"});
    noAir[28] = 0x02;
    error = nil;
    dispatch_data_t data = dispatch_data_create(noAir.data(), noAir.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    EXPECT_EQ([device newLibraryWithData:data error:&error], nil);
    EXPECT_EQ(error.code, MTLLibraryErrorUnsupported) << text(error);
    EXPECT_EQ([device newDefaultLibrary], nil) << "the test binary's bundle has no default.metallib";
    error = nil;
    EXPECT_EQ([device newDefaultLibraryWithBundle:[NSBundle mainBundle] error:&error], nil);
    EXPECT_NE(error, nil);
}

void noMemory()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    MTLTextureDescriptor *converted =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(gm20b::kRG8Srgb)
                                                           width:64
                                                          height:64
                                                       mipmapped:NO];
    converted.storageMode = MTLStorageModePrivate;
    converted.usage = MTLTextureUsageShaderRead;
    ASSERT_TRUE([device newTextureWithDescriptor:converted] != nil);
    constexpr NSUInteger kLarge = 64u << 20;
    // the pool's chunks are the task's pages imported (gpu.cpp newChunk), block-linear chunks the kernel's own
    kernel_model::model.refuseAllocs = kernel_model::model.refuseImports = 1000;
    EXPECT_EQ([device newBufferWithLength:kLarge options:MTLResourceStorageModeShared], nil);
    EXPECT_EQ([device newBufferWithLength:kLarge options:MTLResourceStorageModePrivate], nil);
    std::vector<uint8_t> bytes(kLarge, 0x5A);
    EXPECT_EQ([device newBufferWithBytes:bytes.data() length:kLarge options:MTLResourceStorageModeShared], nil);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:4096
                                                                                height:4096
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    EXPECT_EQ([device newTextureWithDescriptor:d], nil);
    d.storageMode = MTLStorageModeShared;
    EXPECT_EQ([device newTextureWithDescriptor:d], nil);
    converted.width = converted.height = 4096;
    EXPECT_EQ([device newTextureWithDescriptor:converted], nil);
    converted.storageMode = MTLStorageModeShared;
    EXPECT_EQ([device newTextureWithDescriptor:converted], nil);
    kernel_model::model.refuseAllocs = kernel_model::model.refuseImports = 0;
    EXPECT_NE([device newBufferWithLength:kLarge options:MTLResourceStorageModeShared], nil) << "memory again: made";
}

void unwiredPages()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const size_t length = 4 * getpagesize();
    void *pages = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    ASSERT_NE(pages, MAP_FAILED);
    kernel_model::model.refuseImports = 1;
    EXPECT_EQ(
        [device newBufferWithBytesNoCopy:pages length:length options:MTLResourceStorageModeShared deallocator:nil],
        nil);
    EXPECT_NE(
        [device newBufferWithBytesNoCopy:pages length:length options:MTLResourceStorageModeShared deallocator:nil], nil)
        << "the next import is taken";
}

// A two-plane YCbCr target's state the pack holds for its stand-in is drawn by raw from its creation: m2v's twin, and its
// functions', wait for a command buffer that falls back
void ycbcrTargetState()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormat(gm20b::kYcbcrTargetStandIn))));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormat(500)) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    EXPECT_TRUE([((NSNumber * (*)(id, SEL)) objc_msgSend)(state, sel_registerName("switchPackHit")) boolValue])
        << "drawn from the pack";
}

// The plain layout of a texture whose memory no block-linear image is made in (an uncompressed one), as m2v's frontend
// answered it; a heap's or buffer's texture has none; a no-copy texture of it is refused, as it was
void plainLayouts()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    const auto layoutOf = [&](MTLTextureDescriptor *d, BOOL backed) {
        return ((id (*)(id, SEL, id, BOOL))objc_msgSend)(
            device, sel_registerName("newTextureLayoutWithDescriptor:isHeapOrBufferBacked:"), d, backed);
    };
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:64
                                                                                height:32
                                                                             mipmapped:NO];
    id layout = layoutOf(d, NO);
    ASSERT_TRUE(layout != nil);
    EXPECT_EQ(((NSUInteger (*)(id, SEL))objc_msgSend)(layout, sel_registerName("size")), 64u * 32 * 4);
    EXPECT_EQ(layoutOf(d, YES), nil);
    std::vector<uint32_t> linear(64 * 32), memory(64 * 32), back(64 * 32);
    for (size_t i = 0; i < linear.size(); i++)
        linear[i] = uint32_t(i * 2654435761u);
    const MTLRegion all = MTLRegionMake2D(0, 0, 64, 32);
    ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger, NSUInteger, void *, NSUInteger, NSUInteger,
               MTLRegion))objc_msgSend)(
        layout,
        sel_registerName("copyFromLinearBytes:linearOffset:linearBytesPerRow:linearBytesPerImage:toTextureMemory:"
                         "textureSlice:textureLevel:textureRegion:"),
        linear.data(), 0, 256, 0, memory.data(), 0, 0, all);
    ((void (*)(id, SEL, const void *, NSUInteger, NSUInteger, MTLRegion, void *, NSUInteger, NSUInteger,
               NSUInteger))objc_msgSend)(
        layout,
        sel_registerName("copyFromTextureMemory:textureSlice:textureLevel:textureRegion:toLinearBytes:linearOffset:"
                         "linearBytesPerRow:linearBytesPerImage:"),
        memory.data(), 0, 0, all, back.data(), 0, 256, 0);
    EXPECT_EQ(memory, linear) << "rows packed";
    EXPECT_EQ(back, linear);
    MTLTextureDescriptor *volume = [MTLTextureDescriptor new];
    volume.textureType = MTLTextureType3D;
    volume.pixelFormat = MTLPixelFormatR16Float;
    volume.width = 8;
    volume.height = 4;
    volume.depth = 3;
    id volumeLayout = layoutOf(volume, NO);
    ASSERT_TRUE(volumeLayout != nil);
    EXPECT_EQ(((NSUInteger (*)(id, SEL))objc_msgSend)(volumeLayout, sel_registerName("size")), 8u * 4 * 3 * 2);
    EXPECT_EQ(((id (*)(id, SEL, void *, NSUInteger, id, id))objc_msgSend)(
                  device, sel_registerName("newTextureWithBytesNoCopy:length:descriptor:deallocator:"), memory.data(),
                  memory.size() * 4, d, nil),
              nil);
}

// The `i`th of a run of samplers with headers of their own: four address modes, then a level clamp on the 1/64 grid
MTLSamplerDescriptor *distinctSampler(NSUInteger i)
{
    static const MTLSamplerAddressMode modes[] = {MTLSamplerAddressModeClampToEdge, MTLSamplerAddressModeRepeat,
                                                  MTLSamplerAddressModeMirrorRepeat, MTLSamplerAddressModeClampToZero};
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.sAddressMode = d.tAddressMode = modes[i % 4];
    d.mipFilter = MTLSamplerMipFilterLinear;
    d.lodMaxClamp = 1.0f + float(i / 4) / 64.0f;
    return d;
}

// Samplers of one descriptor share their compare form's header, so far more of them live than the table has slots for
// headers of their own; distinct ones fill it, and the one made then is nil as Metal answers a resource it has no room for,
// until a slot is free again
void samplerTable()
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_TRUE(device != nil);
    NSMutableArray<id<MTLSamplerState>> *held = [NSMutableArray array];
    MTLSamplerDescriptor *same = distinctSampler(1);
    for (int i = 0; i < 1500; i++)
        if (id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:same])
            [held addObject:s];
    EXPECT_EQ(held.count, 1500u) << "1500 samplers of one descriptor: 1500 headers and one shared compare form";
    [held removeAllObjects];
    for (NSUInteger i = 0; i < 4096; i++) {
        id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:distinctSampler(i)];
        if (!s)
            break;
        [held addObject:s];
    }
    EXPECT_GT(held.count, 900u);
    EXPECT_LT(held.count, 1100u) << "a distinct sampler takes two of the table's 2048 slots";
    EXPECT_EQ([device newSamplerStateWithDescriptor:distinctSampler(4000)], nil);
    [held removeLastObject];
    EXPECT_NE([device newSamplerStateWithDescriptor:distinctSampler(4000)], nil) << "a freed slot is taken again";
}

// A wrapping sampler whose chroma-subsampled address map variant (clamped to the edge) no other header in the table has
MTLSamplerDescriptor *wrappingSampler(float lodMax)
{
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.sAddressMode = d.tAddressMode = MTLSamplerAddressModeRepeat;
    d.minFilter = d.magFilter = MTLSamplerMinMagFilterLinear;
    d.mipFilter = MTLSamplerMipFilterNearest;
    d.lodMaxClamp = lodMax;
    return d;
}

// A draw sampling a 4:2:2 texture through a wrapping sampler takes a sampler header under the texture's address map; with
// the table full its command buffer fails out of memory, as Metal's does for a resource it has no room for. (4:2:2, not
// 4:2:0: the kernel model has no native surface views, which a 4:2:0 surface's planes are read through.)
void planarSamplerTableFull()
{
    const gm20b::PackEntry sampled = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0};
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float}}, {sampled}));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    CVPixelBufferRef pixels = nullptr;
    ASSERT_EQ(CVPixelBufferCreate(
                  kCFAllocatorDefault, 64, 48, kCVPixelFormatType_422YpCbCr8BiPlanarFullRange,
                  (__bridge CFDictionaryRef)
                      @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}},
                  &pixels),
              kCVReturnSuccess);
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixels);
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceYCbCrMatrix"), CFSTR("ITU_R_709_2"));
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(502) width:64 height:48 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> planar = [device newTextureWithDescriptor:td iosurface:surface plane:0];
    ASSERT_TRUE(planar != nil);
    id<MTLSamplerState> drawnFirst = [device newSamplerStateWithDescriptor:wrappingSampler(4)];
    id<MTLSamplerState> drawnFull = [device newSamplerStateWithDescriptor:wrappingSampler(8)];
    ASSERT_TRUE(drawnFirst != nil && drawnFull != nil);
    id<MTLCommandBuffer> first = drawSampling(device, state, planar, drawnFirst);
    EXPECT_EQ(first.status, MTLCommandBufferStatusCompleted) << text(first.error);
    NSMutableArray<id<MTLSamplerState>> *held = [NSMutableArray array];
    for (NSUInteger i = 0; i < 4096; i++)
        if (id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:distinctSampler(i)])
            [held addObject:s];
        else
            break;
    // a sampler of a descriptor made already takes one slot, its own, so these take the last one a distinct one could not
    while (id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:distinctSampler(0)])
        [held addObject:s];
    ASSERT_LT(held.count, 4096u) << "the table is full";
    ASSERT_EQ([device newSamplerStateWithDescriptor:wrappingSampler(12)], nil);
    id<MTLCommandBuffer> full = drawSampling(device, state, planar, drawnFull);
    EXPECT_EQ(full.status, MTLCommandBufferStatusError);
    EXPECT_EQ(full.error.code, MTLCommandBufferErrorOutOfMemory) << text(full.error);
    [held removeAllObjects];
    id<MTLCommandBuffer> freed = drawSampling(device, state, planar, drawnFull);
    EXPECT_EQ(freed.status, MTLCommandBufferStatusCompleted) << text(freed.error);
    CVPixelBufferRelease(pixels);
}

// A draw sampling an R8Unorm texture through an opaque white border reads the border as R8Unorm holds it, a header of its
// own (Sampler::bordered); with the table full its command buffer fails out of memory, as the planar address map's does
void borderSamplerTableFull()
{
    const gm20b::PackEntry sampled = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0};
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, {sampled}));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    MTLTextureDescriptor *td =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:16 height:16 mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> red = [device newTextureWithDescriptor:td];
    MTLSamplerDescriptor *border = [MTLSamplerDescriptor new];
    border.sAddressMode = border.tAddressMode = MTLSamplerAddressModeClampToBorderColor;
    border.borderColor = MTLSamplerBorderColorOpaqueWhite;
    id<MTLSamplerState> bordered = [device newSamplerStateWithDescriptor:border];
    ASSERT_TRUE(red != nil && bordered != nil);
    NSMutableArray<id<MTLSamplerState>> *held = [NSMutableArray array];
    for (NSUInteger i = 0; i < 4096; i++)
        if (id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:distinctSampler(i)])
            [held addObject:s];
        else
            break;
    while (id<MTLSamplerState> s = [device newSamplerStateWithDescriptor:distinctSampler(0)])
        [held addObject:s];
    ASSERT_LT(held.count, 4096u) << "the table is full";
    id<MTLCommandBuffer> full = drawSampling(device, state, red, bordered);
    EXPECT_EQ(full.status, MTLCommandBufferStatusError);
    EXPECT_EQ(full.error.code, MTLCommandBufferErrorOutOfMemory) << text(full.error);
    [held removeAllObjects];
    id<MTLCommandBuffer> freed = drawSampling(device, state, red, bordered);
    EXPECT_EQ(freed.status, MTLCommandBufferStatusCompleted) << text(freed.error);
}

// A sampler state of another device bound, which Metal's validation stops: the command buffer fails at its recording
void foreignSampler()
{
    const gm20b::PackEntry sampled = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0};
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {}, {}, true, "", {}, {sampled}));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:fileURL(writeMetallib()) error:&error];
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:16
                                                                                 height:16
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> sampledTexture = [device newTextureWithDescriptor:td];
    ASSERT_TRUE(sampledTexture != nil);
    id<MTLCommandBuffer> cb = drawSampling(device, state, sampledTexture, (id<MTLSamplerState>)[NSObject new]);
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal) << text(cb.error);
}

}  // namespace

TEST(GM20BFacadeOpen, AnUncompressedTextureHasThePlainLayoutAndNoNoCopyTexture)
{
    switch_ios::test::ExpectPassesInFreshProcess(plainLayouts);
}

TEST(GM20BFacadeOpen, ALibraryAnswersItsNamesTypeOptionsAndExternFunctions)
{
    switch_ios::test::ExpectPassesInFreshProcess(libraryAnswers);
}

TEST(GM20BFacadeOpen, ALibraryMetalWouldNotLoadIsNilWithMetalsError)
{
    switch_ios::test::ExpectPassesInFreshProcess(unreadLibraries);
}

TEST(GM20BFacadeOpen, NoMemoryIsNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(noMemory);
}

TEST(GM20BFacadeOpen, PagesTheKernelCannotWireAreNoBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(unwiredPages);
}

TEST(GM20BFacadeOpen, AYcbcrTargetStateOpensNoM2V)
{
    switch_ios::test::ExpectPassesInFreshProcess(ycbcrTargetState);
}

TEST(GM20BFacadeOpen, SamplersShareTheirCompareFormAndAFullTableIsNil)
{
    switch_ios::test::ExpectPassesInFreshProcess(samplerTable);
}

TEST(GM20BFacadeOpen, APlanarDrawWithTheHeaderTableFullFailsItsCommandBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(planarSamplerTableFull);
}

TEST(GM20BFacadeOpen, AnOpaqueBorderDrawWithTheHeaderTableFullFailsItsCommandBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(borderSamplerTableFull);
}

TEST(GM20BFacadeOpen, ASamplerOfAnotherDeviceFailsItsCommandBuffer)
{
    switch_ios::test::ExpectPassesInFreshProcess(foreignSampler);
}
