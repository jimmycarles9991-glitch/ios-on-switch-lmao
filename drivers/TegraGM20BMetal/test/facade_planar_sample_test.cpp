// SPDX-License-Identifier: MIT
// A 4:2:2 YCbCr surface texture written and sampled in one command buffer on the facade over the kernel model: its
// image is made from its planes by the conversion pass between the work that writes the planes and the work that samples
// it, in the command stream, as an M4 Max samples the planes the earlier encoders left (c38-audit/m2v-planar-plan/probe).
// Each scenario is a process of its own.
#include "facade_compute.h"
#include "bazel/testing/fresh_process.h"
#include "../push.h"

#import <CoreVideo/CoreVideo.h>
#import <IOSurface/IOSurface.h>
#include <algorithm>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using namespace harness;

constexpr uint32_t kW = 64, kH = 48;
constexpr MTLPixelFormat kYcbcr422 = MTLPixelFormat(502);
const uint32_t kThreads[3] = {8, 8, 1};

std::string text(id object)
{
    return [object description].UTF8String ?: "";
}

// What the jobs pushed, in order: a pass's colour target 0, a kernel's launch, a copy engine launch's destination
struct Event {
    enum Kind {
        Target,
        Launch,
        Copy
    } kind;

    uint64_t address;
};

std::vector<Event> events(size_t first)
{
    // NVB197_SET_COLOR_TARGET_A/B/FORMAT(0), NVB1C0_SEND_PCAS_A, NVB0B5_OFFSET_OUT_UPPER/LOWER and LAUNCH_DMA
    constexpr uint32_t kTargetA = 0x0800, kTargetB = 0x0804, kTargetFormat = 0x0810, kSendPcasA = 0x02b4,
                       kOutUpper = 0x0408, kOutLower = 0x040c, kLaunchDma = 0x0300;
    std::vector<Event> out;
    uint64_t target = 0, to = 0;
    const auto method = [&](uint32_t subc, uint32_t m, uint32_t v) {
        if (subc == gm20b::SUBC_3D && m == kTargetA)
            target = (target & 0xffffffffull) | uint64_t(v) << 32;
        else if (subc == gm20b::SUBC_3D && m == kTargetB)
            target = (target & ~0xffffffffull) | v;
        else if (subc == gm20b::SUBC_3D && m == kTargetFormat && v)
            out.push_back({Event::Target, target});
        else if (subc == gm20b::SUBC_COMPUTE && m == kSendPcasA)
            out.push_back({Event::Launch, 0});
        else if (subc == gm20b::SUBC_COPY && m == kOutUpper)
            to = (to & 0xffffffffull) | uint64_t(v) << 32;
        else if (subc == gm20b::SUBC_COPY && m == kOutLower)
            to = (to & ~0xffffffffull) | v;
        else if (subc == gm20b::SUBC_COPY && m == kLaunchDma)
            out.push_back({Event::Copy, to});
    };
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, subc = (h >> 13) & 7,
                           m = (h & 0xfff) << 2;
            if (op == 4) {
                method(subc, m, count);
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i)
                method(subc, op == 1 ? m + 4 * k : op == 5 && k ? m + 4 : m, w[i]);
        }
    }
    return out;
}

size_t indexOf(const std::vector<Event> &e, Event::Kind kind, uint64_t address)
{
    for (size_t i = 0; i < e.size(); ++i)
        if (e[i].kind == kind && e[i].address == address)
            return i;
    return SIZE_MAX;
}

size_t countOf(const std::vector<Event> &e, Event::Kind kind, uint64_t address)
{
    return size_t(std::count_if(e.begin(), e.end(), [&](const Event &x) {
        return x.kind == kind && x.address == address;
    }));
}

// The colour target of the first pass after event `at`, 0 for none
uint64_t nextPass(const std::vector<Event> &e, size_t at)
{
    for (size_t i = at + 1; i < e.size(); ++i)
        if (e[i].kind == Event::Target)
            return e[i].address;
    return 0;
}

size_t passesBetween(const std::vector<Event> &e, size_t from, size_t to)
{
    size_t n = 0;
    for (size_t i = from; i < to && i < e.size(); ++i)
        n += e[i].kind == Event::Target;
    return n;
}

// The pack: the BGRA8 state sampling fragment texture 0 through sampler 0, the 4:2:2 texture's conversion state and the
// 8-bit store states a pass into it ends with
std::string planarPack(std::vector<gm20b::PackKernel> kernels = {}, std::vector<gm20b::PackComputeFacts> facts = {})
{
    const gm20b::PackEntry sampled = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0};
    return writePack(packKey(MTLPixelFormatBGRA8Unorm), std::move(kernels), std::move(facts), true, "",
                     {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float},
                      {"raw.ycbcr_store.ycbcr_store_luma8_frag", MTLPixelFormatR8Unorm},
                      {"raw.ycbcr_store.ycbcr_store_chroma8_frag", MTLPixelFormatRG8Unorm}},
                     {sampled});
}

struct Scene {
    id<MTLDevice> device;
    id<MTLRenderPipelineState> state;
    id<MTLSamplerState> sampler;
    id<MTLTexture> out, planar;
    CVPixelBufferRef pixels = nullptr;
    uint64_t image = 0, outAt = 0;

    ~Scene()
    {
        if (pixels)
            CVPixelBufferRelease(pixels);
    }
};

// A device over `pack`, its sampling state, a 64x64 BGRA8 target and a 4:2:2 texture of `usage` over a 64x48 surface
void makeScene(Scene &s, const std::string &pack, MTLTextureUsage usage)
{
    s.device = open(pack);
    ASSERT_TRUE(s.device != nil);
    NSError *error = nil;
    id<MTLLibrary> library =
        [s.device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    s.state =
        [s.device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatBGRA8Unorm) error:&error];
    ASSERT_TRUE(s.state != nil) << text(error);
    s.sampler = [s.device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]];
    MTLTextureDescriptor *rt = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                  width:64
                                                                                 height:64
                                                                              mipmapped:NO];
    rt.storageMode = MTLStorageModePrivate;
    rt.usage = MTLTextureUsageRenderTarget;
    s.out = [s.device newTextureWithDescriptor:rt];
    ASSERT_EQ(CVPixelBufferCreate(
                  kCFAllocatorDefault, kW, kH, kCVPixelFormatType_422YpCbCr8BiPlanarFullRange,
                  (__bridge CFDictionaryRef)
                      @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}},
                  &s.pixels),
              kCVReturnSuccess);
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(s.pixels);
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceYCbCrMatrix"), CFSTR("ITU_R_709_2"));
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kYcbcr422 width:kW height:kH mipmapped:NO];
    d.usage = usage;
    s.planar = [s.device newTextureWithDescriptor:d iosurface:surface plane:0];
    ASSERT_TRUE(s.planar != nil);
    ASSERT_TRUE(((SwitchMetalTexture *)s.planar)->_gm20bOk && ((SwitchMetalTexture *)s.out)->_gm20bOk);
    s.image = ((SwitchMetalTexture *)s.planar)->_raw.va;
    s.outAt = ((SwitchMetalTexture *)s.out)->_raw.va;
}

void clearInto(id<MTLCommandBuffer> cb, id<MTLTexture> t)
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = t;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.25, 0.5, 0.75, 1);
    [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
}

void sampleInto(id<MTLCommandBuffer> cb, const Scene &s, id<MTLTexture> sampled = nil)
{
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = s.out;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:s.state];
    [e setFragmentTexture:sampled ?: s.planar atIndex:0];
    [e setFragmentSamplerState:s.sampler atIndex:0];
    const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

void run(id<MTLCommandBuffer> cb, bool judged = true)
{
    [cb commit];
    [cb waitUntilCompleted];
    if (judged)
        EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << text(cb.error);
}

// A pass clears it, then a pass samples it: the clear into its stand-in and the two planes' stores, the image drawn from
// the planes, then the sampling pass
void renderThenSample()
{
    Scene s;
    makeScene(s, planarPack(), MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[s.device newCommandQueue] commandBuffer];
    clearInto(cb, s.planar);
    sampleInto(cb, s);
    run(cb);
    const std::vector<Event> e = events(first);
    ASSERT_EQ(countOf(e, Event::Target, s.image), 1u) << "one conversion, after the stores";
    const size_t image = indexOf(e, Event::Target, s.image);
    EXPECT_GE(passesBetween(e, 0, image), 3u) << "the clear into the stand-in and the luma and chroma stores first";
    EXPECT_EQ(nextPass(e, image), s.outAt) << "the sampling pass right after the conversion";
}

// A pass clears it, then a pass samples a view of it in its own format, which samples its image: drawn again after the
// stores, as for the texture itself
void renderThenSampleAView()
{
    Scene s;
    makeScene(s, planarPack(), MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    id<MTLTexture> view = [s.planar newTextureViewWithPixelFormat:kYcbcr422];
    ASSERT_TRUE(view != nil && ((SwitchMetalTexture *)view)->_gm20bOk);
    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[s.device newCommandQueue] commandBuffer];
    clearInto(cb, s.planar);
    sampleInto(cb, s, view);
    run(cb);
    const std::vector<Event> e = events(first);
    ASSERT_EQ(countOf(e, Event::Target, s.image), 1u) << "one conversion, after the stores";
    const size_t image = indexOf(e, Event::Target, s.image);
    EXPECT_GE(passesBetween(e, 0, image), 3u) << "the clear into the stand-in and the luma and chroma stores first";
    EXPECT_EQ(nextPass(e, image), s.outAt);
}

// A pass samples it, then a pass clears it: the image drawn before the sampling pass from the planes as they were, the
// clear and its stores after
void sampleThenRender()
{
    Scene s;
    makeScene(s, planarPack(), MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[s.device newCommandQueue] commandBuffer];
    sampleInto(cb, s);
    clearInto(cb, s.planar);
    run(cb);
    const std::vector<Event> e = events(first);
    ASSERT_EQ(countOf(e, Event::Target, s.image), 1u) << "converted once, before the range";
    const size_t image = indexOf(e, Event::Target, s.image), out = indexOf(e, Event::Target, s.outAt);
    EXPECT_EQ(nextPass(e, image), s.outAt) << "the sampling pass right after the conversion";
    EXPECT_GE(passesBetween(e, out + 1, e.size()), 3u) << "the clear into the stand-in and the two stores after it";
}

// A copy of bytes into its luma plane, then a pass samples it (a texture made to be read alone, with no stand-in): the
// image drawn after the copy from the planes it wrote
void copyThenSample()
{
    Scene s;
    makeScene(s, planarPack(), MTLTextureUsageShaderRead);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    const gm20b::Texture &luma = *((SwitchMetalTexture *)s.planar)->_raw.luma;
    id<MTLBuffer> bytes = [s.device newBufferWithLength:kW * kH options:MTLResourceStorageModeShared];
    memset(bytes.contents, 0x5a, kW * kH);
    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[s.device newCommandQueue] commandBuffer];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromBuffer:bytes
               sourceOffset:0
          sourceBytesPerRow:kW
        sourceBytesPerImage:kW * kH
                 sourceSize:MTLSizeMake(kW, kH, 1)
                  toTexture:s.planar
           destinationSlice:0
           destinationLevel:0
          destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
    sampleInto(cb, s);
    run(cb);
    const std::vector<Event> e = events(first);
    ASSERT_EQ(countOf(e, Event::Target, s.image), 1u);
    const size_t image = indexOf(e, Event::Target, s.image);
    size_t copied = SIZE_MAX;
    for (size_t i = 0; i < e.size() && copied == SIZE_MAX; ++i)
        if (e[i].kind == Event::Copy && e[i].address >= luma.va && e[i].address < luma.va + uint64_t(luma.pitch) * kH)
            copied = i;
    ASSERT_NE(copied, SIZE_MAX) << "the copy into the luma plane";
    EXPECT_LT(copied, image) << "the image drawn from the planes the copy wrote";
    EXPECT_EQ(nextPass(e, image), s.outAt);
}

// A pass clears the luma plane's own R8Unorm texture, then a pass of the same target loads it and samples the 4:2:2
// texture over the same surface: the second pass would fuse into the first but for the plane under its sample, and the
// image is drawn again between them
void planeWriteThenSampleOverIt()
{
    const gm20b::PackEntry sampled = {0, gm20b::EntryTexture, 2, 0, gm20b::SamplerSlot, 0, 0};
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatR8Unorm), {}, {}, true, "",
                                          {{"raw.ycbcr.ycbcr_converted_frag", MTLPixelFormatRGBA16Float}}, {sampled}));
    ASSERT_TRUE(device != nil);
    NSError *error = nil;
    id<MTLLibrary> library = [device newLibraryWithURL:[NSURL fileURLWithPath:@(writeMetallib().c_str())] error:&error];
    ASSERT_TRUE(library != nil) << text(error);
    id<MTLRenderPipelineState> state =
        [device newRenderPipelineStateWithDescriptor:descriptor(library, MTLPixelFormatR8Unorm) error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    CVPixelBufferRef pixels = nullptr;
    ASSERT_EQ(CVPixelBufferCreate(
                  kCFAllocatorDefault, kW, kH, kCVPixelFormatType_422YpCbCr8BiPlanarFullRange,
                  (__bridge CFDictionaryRef)
                      @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}},
                  &pixels),
              kCVReturnSuccess);
    IOSurfaceRef surface = CVPixelBufferGetIOSurface(pixels);
    IOSurfaceSetValue(surface, CFSTR("IOSurfaceYCbCrMatrix"), CFSTR("ITU_R_709_2"));
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kYcbcr422 width:kW height:kH mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> planar = [device newTextureWithDescriptor:d iosurface:surface plane:0];
    MTLTextureDescriptor *r8 =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:kW height:kH mipmapped:NO];
    r8.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> luma = [device newTextureWithDescriptor:r8 iosurface:surface plane:0];
    ASSERT_TRUE(planar != nil && luma != nil && ((SwitchMetalTexture *)planar)->_gm20bOk &&
                ((SwitchMetalTexture *)luma)->_gm20bOk);
    const uint64_t image = ((SwitchMetalTexture *)planar)->_raw.va;
    const size_t first = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = luma;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.5, 0, 0, 1);
    [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
    pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass];
    [e setRenderPipelineState:state];
    [e setFragmentTexture:planar atIndex:0];
    [e setFragmentSamplerState:[device newSamplerStateWithDescriptor:[MTLSamplerDescriptor new]] atIndex:0];
    const float vertices[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    [e setVertexBytes:vertices length:sizeof vertices atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
    run(cb);
    const std::vector<Event> ev = events(first);
    ASSERT_EQ(countOf(ev, Event::Target, image), 2u) << "the image before the range and again after the plane's clear";
    const size_t before = indexOf(ev, Event::Target, image);
    size_t again = before + 1;
    while (again < ev.size() && !(ev[again].kind == Event::Target && ev[again].address == image))
        ++again;
    EXPECT_GE(passesBetween(ev, before + 1, again), 1u) << "the clear of the plane, a pass of its own";
    EXPECT_NE(nextPass(ev, again), 0u) << "the sampling pass after the image drawn again";
    CVPixelBufferRelease(pixels);
}

// Kernel 0 over the 4:2:2 texture at slot 0, read through its image's own header (window 0), made under its map
gm20b::PackKernel samplingRecord(uint32_t maxThreads)
{
    gm20b::AirHash air;
    air.fill(uint8_t(kKernelSeed));
    const gm20b::BoundTexture bound = {0, kYcbcr422, MTLTextureType2D, 1, 1, {2, 3, 4, 5}, false, 1};
    const std::string key =
        gm20b::computeKey(air, "", maxThreads, false, kThreads, "", gm20b::computeTextures(&bound, 1), "");
    gm20b::PackKernel k = {};
    k.keyHash = gm20b::computeKeyHash(key);
    k.regionWord = -1;
    k.qmd[kThreadsWord] = kThreads[0] << 16;
    k.qmd[kThreadsWord + 1] = kThreads[1] | kThreads[2] << 16;
    k.entries = 1;
    k.setBytes = 16;
    k.entry[0] = {0, uint8_t(gm20b::KernelTexture), 0, 0};
    k.readOnlyEntries = 1;
    k.mapsStated = 1;
    return k;
}

// A pass clears it, then a kernel samples it: the conversion between the stores and the launch. The first command buffer
// finds the kernel in the pack off the encoding thread; the second is judged.
void renderThenKernel()
{
    const uint32_t maxThreads = uint32_t([MTLComputePipelineDescriptor new].maxTotalThreadsPerThreadgroup);
    Scene s;
    makeScene(s, planarPack({samplingRecord(maxThreads)}, {kernelFacts(0, maxThreads)}),
              MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    ASSERT_FALSE(::testing::Test::HasFatalFailure());
    metal_complete_states(dir().c_str());
    const std::string library = kernelLibrary();
    dispatch_data_t data =
        dispatch_data_create(library.data(), library.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    NSError *error = nil;
    id<MTLLibrary> kernels = [s.device newLibraryWithData:data error:&error];
    ASSERT_TRUE(kernels != nil) << text(error);
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = [kernels newFunctionWithName:@(kernelName(0).c_str())];
    id<MTLComputePipelineState> state =
        [s.device newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
    ASSERT_TRUE(state != nil) << text(error);
    const auto renderThenDispatch = [&](bool judged) {
        id<MTLCommandBuffer> cb = [[s.device newCommandQueue] commandBuffer];
        clearInto(cb, s.planar);
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:state];
        [e setTexture:s.planar atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(kW / kThreads[0], kH / kThreads[1], 1)
            threadsPerThreadgroup:MTLSizeMake(kThreads[0], kThreads[1], kThreads[2])];
        [e endEncoding];
        run(cb, judged);
    };
    renderThenDispatch(false);
    const size_t first = kernel_model::model.jobs.size();
    renderThenDispatch(true);
    const std::vector<Event> e = events(first);
    ASSERT_EQ(countOf(e, Event::Target, s.image), 1u);
    const size_t image = indexOf(e, Event::Target, s.image);
    size_t launch = SIZE_MAX;
    for (size_t i = 0; i < e.size() && launch == SIZE_MAX; ++i)
        if (e[i].kind == Event::Launch)
            launch = i;
    EXPECT_GE(passesBetween(e, 0, image), 3u) << "the clear into the stand-in and the two stores first";
    ASSERT_NE(launch, SIZE_MAX) << "the kernel ran raw";
    EXPECT_LT(image, launch) << "the kernel samples the image made after the stores";
}

}  // namespace

TEST(GM20BFacadePlanarSample, ARenderThenASampleConvertsBetweenThem)
{
    switch_ios::test::ExpectPassesInFreshProcess(renderThenSample);
}

TEST(GM20BFacadePlanarSample, ARenderThenASampleOfAViewConvertsBetweenThem)
{
    switch_ios::test::ExpectPassesInFreshProcess(renderThenSampleAView);
}

TEST(GM20BFacadePlanarSample, APlaneWriteThenASampleOverItAreNotFused)
{
    switch_ios::test::ExpectPassesInFreshProcess(planeWriteThenSampleOverIt);
}

TEST(GM20BFacadePlanarSample, ASampleThenARenderConvertsBeforeTheSample)
{
    switch_ios::test::ExpectPassesInFreshProcess(sampleThenRender);
}

TEST(GM20BFacadePlanarSample, ACopyIntoThePlanesThenASampleConvertsAfterTheCopy)
{
    switch_ios::test::ExpectPassesInFreshProcess(copyThenSample);
}

TEST(GM20BFacadePlanarSample, ARenderThenAKernelSampleConvertsBeforeTheLaunch)
{
    switch_ios::test::ExpectPassesInFreshProcess(renderThenKernel);
}
