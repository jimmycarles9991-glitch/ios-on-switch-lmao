// SPDX-License-Identifier: MIT
// Kernels on the facade over the kernel model (facade_compute.h) reaching an RGB10A8_2P_XR10 texture over a
// two-plane surface (xr_pass.h), which Apple's Metal makes with ShaderWrite and lets a kernel store into: the facade
// keeps the texture raw, keys the kernel ":xr2p" at its slot, and runs a store and a load through the planes' own headers
// (the record's texture and plane entries). A load of one made without ShaderRead fails the command buffer, as Metal's
// validation stops it. A BGR10_XR texture's float reads take its codes, its half reads its stand-in. None of them opens
// m2v.
#include "facade_compute.h"
#include "NativeMetalComputeArtifact.h"
#include "reflection_blob_writer.h"
#include "../xr.h"

#include "../../../kits/SwitchGM20B/SwitchGM20BTexture.h"

#include <gtest/gtest.h>
#include <string>

namespace {

using namespace harness;

constexpr uint32_t kSide = 64;
constexpr uint32_t kCompute = 1, kSendPcasA = 0x02b4;  // NVB1C0_SEND_PCAS_A
const uint32_t kThreads[3] = {8, 8, 1};
// the library's kernels the records are made for
constexpr uint32_t kStore = 0, kLoad = 1, kFloatRead = 2, kHalfRead = 3;
constexpr uint32_t kR32Uint = 98, kRgba16Float = 97, kRg32Uint = 101;   // VkFormat
constexpr uint8_t kRgba[4] = {2, 3, 4, 5}, kRgbZero[4] = {2, 3, 4, 0};  // MTLTextureSwizzle

id<MTLDevice> gDevice;
id<MTLComputePipelineState> gStates[4];

// The header word 0 the direct compile records for the translator's view of `vkFormat` (direct_compute.cpp
// Entries::texture), the texture's default swizzle composed in
uint32_t viewWord0(uint32_t vkFormat, bool storage)
{
    const uint32_t rgba[4] = {3, 4, 5, 6};
    uint32_t word0 = 0;
    switch_gm20b_view_word0(vkFormat, rgba, storage, &word0);
    return word0;
}

// Kernel `i`'s record over a two-plane texture at slot 0: its words' and its alpha bytes' entries, read only where `load`
gm20b::PackKernel planesRecord(uint32_t i, uint32_t maxThreads, bool load)
{
    gm20b::AirHash air;
    air.fill(uint8_t(kKernelSeed + i));
    gm20b::BoundTexture bound = {0, 550, MTLTextureType2D, 1, 1, {2, 3, 4, 5}};
    bound.planes = true;
    const std::string key =
        gm20b::computeKey(air, "", maxThreads, false, kThreads, "", gm20b::computeTextures(&bound, 1), "");
    gm20b::PackKernel k = {};
    k.keyHash = gm20b::computeKeyHash(key);
    k.regionWord = -1;
    k.qmd[kThreadsWord] = kThreads[0] << 16;
    k.qmd[kThreadsWord + 1] = kThreads[1] | kThreads[2] << 16;
    k.entries = 2;
    k.setBytes = 32;
    k.entry[0] = {0, uint8_t(gm20b::KernelTexture), 0, viewWord0(98, !load)};        // VK_FORMAT_R32_UINT
    k.entry[1] = {16, uint8_t(gm20b::KernelTexturePlane), 0, viewWord0(13, !load)};  // VK_FORMAT_R8_UINT
    k.readOnlyEntries = load ? 3 : 0;
    return k;
}

// A state's reflection whose one binding is a read-only texture at slot 0 of `components`
std::string readsSlotZero(MTLDataType components)
{
    NmbReflectionNode root = {}, binding = {};
    root.count = 1;
    binding.kind = gm20b::ReflectionBinding;
    binding.name = "t";
    binding.values[0] = MTLBindingTypeTexture;
    binding.values[1] = MTLBindingAccessReadOnly;
    binding.values[6] = components;
    binding.values[7] = 1;
    for (NmbReflectionNode *n : {&root, &binding})
        std::fill(std::begin(n->children), std::end(n->children), gm20b::kReflectionAbsent);
    return reflection_blob::of({root, binding}, {1}, 1);
}

// Kernel `i`'s record of a load of slot 0 keyed at `format` and `swizzle` under `samplers` (computeSamplers' text),
// through a view of `vkFormat`
gm20b::PackKernel readRecord(uint32_t i, uint32_t maxThreads, uint64_t format, uint32_t vkFormat,
                             const std::string &samplers = "", const uint8_t *swizzle = kRgba)
{
    gm20b::AirHash air;
    air.fill(uint8_t(kKernelSeed + i));
    const gm20b::BoundTexture bound = {0, format, MTLTextureType2D,
                                       1, 1,      {swizzle[0], swizzle[1], swizzle[2], swizzle[3]}};
    const std::string key =
        gm20b::computeKey(air, "", maxThreads, false, kThreads, "", gm20b::computeTextures(&bound, 1), samplers);
    gm20b::PackKernel k = {};
    k.keyHash = gm20b::computeKeyHash(key);
    k.regionWord = -1;
    k.qmd[kThreadsWord] = kThreads[0] << 16;
    k.qmd[kThreadsWord + 1] = kThreads[1] | kThreads[2] << 16;
    k.entries = 1;
    k.setBytes = 16;
    k.entry[0] = {0, uint8_t(gm20b::KernelTexture), 0, viewWord0(vkFormat, false)};
    k.readOnlyEntries = 1;
    return k;
}

// A sampler of a mip filter, which the translator's fetches of extended-range codes do not take
MTLSamplerDescriptor *mipSampler()
{
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.mipFilter = MTLSamplerMipFilterLinear;
    return d;
}

std::string samplerAtZero(MTLSamplerDescriptor *d)
{
    const gm20b::BoundSampler s = {0,
                                   d.minFilter,
                                   d.magFilter,
                                   d.mipFilter,
                                   d.sAddressMode,
                                   d.tAddressMode,
                                   d.rAddressMode,
                                   d.maxAnisotropy,
                                   d.compareFunction,
                                   d.borderColor,
                                   d.lodMinClamp,
                                   d.lodMaxClamp,
                                   bool(d.normalizedCoordinates),
                                   bool(d.lodAverage)};
    return gm20b::computeSamplers(&s, 1);
}

// A surface of Metal's two planes of RGB10A8_2P_XR10: the color words, 4 bytes a texel, and the alpha bytes
IOSurfaceRef twoPlanes()
{
    NSDictionary *words = @{
        (id)kIOSurfacePlaneWidth : @(kSide),
        (id)kIOSurfacePlaneHeight : @(kSide),
        (id)kIOSurfacePlaneBytesPerElement : @4,
        (id)kIOSurfacePlaneBytesPerRow : @(kSide * 4),
        (id)kIOSurfacePlaneOffset : @0,
        (id)kIOSurfacePlaneSize : @(kSide * 4 * kSide)
    };
    NSDictionary *alphas = @{
        (id)kIOSurfacePlaneWidth : @(kSide),
        (id)kIOSurfacePlaneHeight : @(kSide),
        (id)kIOSurfacePlaneBytesPerElement : @1,
        (id)kIOSurfacePlaneBytesPerRow : @(kSide),
        (id)kIOSurfacePlaneOffset : @(kSide * 4 * kSide),
        (id)kIOSurfacePlaneSize : @(kSide * kSide)
    };
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfacePixelFormat : @(uint32_t('b3a8')),
        (id)kIOSurfaceAllocSize : @(kSide * 5 * kSide),
        (id)kIOSurfacePlaneInfo : @[ words, alphas ]
    });
}

id<MTLTexture> xrTexture(IOSurfaceRef s, MTLTextureUsage usage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550)
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    d.usage = usage;
    d.storageMode = MTLStorageModeShared;
    return [gDevice newTextureWithDescriptor:d iosurface:s plane:0];
}

// The compute launches of every job submitted from `first` on
size_t launches(size_t first)
{
    size_t n = 0;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, sub = (h >> 13) & 7,
                           method = (h & 0xfff) << 2;
            if (op == 4)
                continue;
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i) {
                const uint32_t m = op == 1 ? method + 4 * k : op == 5 ? method + (k ? 4 : 0) : method;
                n += sub == kCompute && m == kSendPcasA;
            }
        }
    }
    return n;
}

struct Ran {
    MTLCommandBufferStatus status;
    NSInteger error = 0;
    size_t launches = 0;
};

// `sampler`: bound at slot 0 ahead of the state and the texture where `samplerFirst`, else after them
Ran dispatch(uint32_t kernel, id<MTLTexture> t, id<MTLSamplerState> sampler = nil, bool samplerFirst = false)
{
    const size_t jobs = kernel_model::model.jobs.size();
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    if (sampler && samplerFirst)
        [e setSamplerState:sampler atIndex:0];
    [e setComputePipelineState:gStates[kernel]];
    [e setTexture:t atIndex:0];
    if (sampler && !samplerFirst)
        [e setSamplerState:sampler atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake(kSide / kThreads[0], kSide / kThreads[1], 1)
        threadsPerThreadgroup:MTLSizeMake(kThreads[0], kThreads[1], kThreads[2])];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return {cb.status, cb.error.code, launches(jobs)};
}

class FacadeKernelXr : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const uint32_t maxThreads = uint32_t([MTLComputePipelineDescriptor new].maxTotalThreadsPerThreadgroup);
        const std::string reflections = reflection_blob::section(
            {{kernelFacts(kFloatRead, maxThreads).keyHash, true, readsSlotZero(MTLDataTypeFloat)},
             {kernelFacts(kHalfRead, maxThreads).keyHash, true, readsSlotZero(MTLDataTypeHalf)}});
        gDevice = open(writePack(
            packKey(MTLPixelFormatBGRA8Unorm),
            {planesRecord(kStore, maxThreads, false), planesRecord(kLoad, maxThreads, true),
             readRecord(kFloatRead, maxThreads, MTLPixelFormatBGR10_XR, kR32Uint),
             readRecord(kFloatRead, maxThreads, gm20b::kXrStandInFormat, kRgba16Float, samplerAtZero(mipSampler())),
             readRecord(kHalfRead, maxThreads, gm20b::kXrStandInFormat, kRgba16Float),
             readRecord(kFloatRead, maxThreads, MTLPixelFormatBGR10_XR, kR32Uint, "", kRgbZero),
             readRecord(kFloatRead, maxThreads, 550, kRg32Uint)},
            {kernelFacts(kStore, maxThreads), kernelFacts(kLoad, maxThreads), kernelFacts(kFloatRead, maxThreads),
             kernelFacts(kHalfRead, maxThreads)},
            true, reflections,
            {{"raw.xr.xr_planes_merge_frag", MTLPixelFormat(103)},
             {"raw.xr.xr_planes_split_frag", MTLPixelFormat(53)},
             {gm20b::xrConversionOf(uint32_t(MTLPixelFormatBGR10_XR)), MTLPixelFormatRGBA16Float}}));
        ASSERT_TRUE(gDevice != nil);
        metal_complete_states(dir().c_str());
        const std::string bytes = kernelLibrary();
        dispatch_data_t data =
            dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithData:data error:&error];
        ASSERT_TRUE(library != nil) << (error.description.UTF8String ?: "");
        for (uint32_t i : {kStore, kLoad, kFloatRead, kHalfRead}) {
            MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
            d.computeFunction = [library newFunctionWithName:@(kernelName(i).c_str())];
            gStates[i] = [gDevice newComputePipelineStateWithDescriptor:d
                                                                options:MTLPipelineOptionNone
                                                             reflection:nil
                                                                  error:&error];
            ASSERT_TRUE(gStates[i] != nil) << (error.description.UTF8String ?: "");
        }
    }

    IOSurfaceRef planes = twoPlanes();

    void TearDown() override
    {
        if (planes)
            CFRelease(planes);
    }
};

TEST_F(FacadeKernelXr, AStoreRunsRawThroughThePlanes)
{
    ASSERT_TRUE(planes);
    id<MTLTexture> t = xrTexture(planes, MTLTextureUsageShaderWrite);
    ASSERT_TRUE(t);
    // the first dispatch finds the state's kernel in the pack off the encoding thread; the second has it
    dispatch(kStore, t);
    const Ran ran = dispatch(kStore, t);
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted) << ran.error;
    EXPECT_EQ(ran.launches, 1u);
}

TEST_F(FacadeKernelXr, ALoadRunsRawThroughThePlanes)
{
    ASSERT_TRUE(planes);
    id<MTLTexture> t = xrTexture(planes, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    ASSERT_TRUE(t);
    dispatch(kLoad, t);
    const Ran ran = dispatch(kLoad, t);
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted) << ran.error;
    EXPECT_EQ(ran.launches, 1u);
}

TEST_F(FacadeKernelXr, ALoadWithoutShaderReadFailsTheCommandBuffer)
{
    ASSERT_TRUE(planes);
    id<MTLTexture> t = xrTexture(planes, MTLTextureUsageShaderWrite);
    ASSERT_TRUE(t);
    dispatch(kLoad, t);
    const Ran ran = dispatch(kLoad, t);
    EXPECT_EQ(ran.status, MTLCommandBufferStatusError);
    EXPECT_EQ(ran.error, MTLCommandBufferErrorInvalidResource);
    EXPECT_EQ(ran.launches, 0u);
}

id<MTLTexture> bgr10xr()
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGR10_XR
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModePrivate;
    return [gDevice newTextureWithDescriptor:d];
}

// gpu.compute.exact_round (boot 2): a kernel's float reads of BGR10_XR and BGRA10_XR textures through their
// RGBA16Float stand-ins were a half's rounding off. A kernel reading a BGR10_XR texture's floats is keyed at the
// texture's own format and reads its codes: the pack here holds no record of it at the stand-in's
TEST_F(FacadeKernelXr, AFloatReadOfAnExtendedRangeTextureReadsItsCodes)
{
    id<MTLTexture> t = bgr10xr();
    ASSERT_TRUE(t);
    dispatch(kFloatRead, t);
    const Ran ran = dispatch(kFloatRead, t);
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted) << ran.error;
    EXPECT_EQ(ran.launches, 1u);
}

// A half read keeps the stand-in, exact in halves, and so does a float read beside a sampler of a mip filter, which the
// codes' fetches do not take, whether the sampler is bound before the state and the texture or after them
TEST_F(FacadeKernelXr, AHalfReadAndAFloatReadBesideAMipFilterReadTheStandIn)
{
    id<MTLTexture> t = bgr10xr();
    id<MTLSamplerState> mip = [gDevice newSamplerStateWithDescriptor:mipSampler()];
    ASSERT_TRUE(t && mip);
    dispatch(kHalfRead, t);
    Ran ran = dispatch(kHalfRead, t);
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted) << ran.error;
    EXPECT_EQ(ran.launches, 1u);
    for (const bool samplerFirst : {false, true}) {
        dispatch(kFloatRead, t, mip, samplerFirst);
        ran = dispatch(kFloatRead, t, mip, samplerFirst);
        EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted) << ran.error << " sampler first " << samplerFirst;
        EXPECT_EQ(ran.launches, 1u) << "sampler first " << samplerFirst;
    }
}

// RGB10A8_2P_XR10 read alone over a surface's one plane is held as BGR10_XR's words (TegraGM20BMetalXrPlanes.h
// xrWordsTexture), which an M4 Max reads with alpha 0. Boot 2's kernel was keyed at 550, its record viewing the slot as
// a word and an alpha byte side by side, which no header of the texture is, and admission failed the command buffer
// (gpu.gm20b.xr_next_plane, xr_planes_off16; the pack here holds that record too). A kernel reading it is keyed at
// BGR10_XR with alpha zero and reads the words
TEST_F(FacadeKernelXr, WordsAloneAreReadAsBgr10XrWithAlphaZero)
{
    IOSurfaceRef one = IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(kSide),
        (id)kIOSurfaceHeight : @(kSide),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfacePixelFormat : @(uint32_t('w30r'))
    });
    ASSERT_TRUE(one);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormat(550)
                                                                                 width:kSide
                                                                                height:kSide
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    SwitchMetalTexture *t = (SwitchMetalTexture *)[gDevice newTextureWithDescriptor:d iosurface:one plane:0];
    ASSERT_TRUE(t != nil && t->_gm20bOk && t->_raw.format == MTLPixelFormatBGR10_XR && !t->_raw.xrPlanes);
    dispatch(kFloatRead, (id<MTLTexture>)t);
    const Ran ran = dispatch(kFloatRead, (id<MTLTexture>)t);
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted) << ran.error;
    EXPECT_EQ(ran.launches, 1u);
    CFRelease(one);
}

}  // namespace
