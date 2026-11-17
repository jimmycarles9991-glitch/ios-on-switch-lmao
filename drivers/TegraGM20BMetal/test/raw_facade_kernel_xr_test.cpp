// SPDX-License-Identifier: MIT
// Kernels on the raw facade over the kernel model (raw_facade_compute.h) reaching an RGB10A8_2P_XR10 texture over a
// two-plane surface (raw_xr_pass.h), which Apple's Metal makes with ShaderWrite and lets a kernel store into: the facade
// keeps the texture raw, keys the kernel ":xr2p" at its slot, and runs a store and a load through the planes' own headers
// (the record's texture and plane entries). A load of one made without ShaderRead fails the command buffer, as Metal's
// validation stops it. None of them opens m2v.
#include "raw_facade_compute.h"

#include "../../../kits/SwitchGM20B/SwitchGM20BTexture.h"

#include <gtest/gtest.h>
#include <string>

namespace {

using namespace harness;

constexpr uint32_t kSide = 64;
constexpr uint32_t kCompute = 1, kSendPcasA = 0x02b4;  // NVB1C0_SEND_PCAS_A
const uint32_t kThreads[3] = {8, 8, 1};
constexpr uint32_t kStore = 0, kLoad = 1;  // the library's kernels the records are made for

id<MTLDevice> gDevice;
id<MTLComputePipelineState> gStates[2];

// The header word 0 the direct compile records for the translator's view of `vkFormat` (raw_direct_compute.cpp
// Entries::texture), the texture's default swizzle composed in
uint32_t viewWord0(uint32_t vkFormat, bool storage)
{
    const uint32_t rgba[4] = {3, 4, 5, 6};
    uint32_t word0 = 0;
    switch_gm20b_view_word0(vkFormat, rgba, storage, &word0);
    return word0;
}

// Kernel `i`'s record over a two-plane texture at slot 0: its words' and its alpha bytes' entries, read only where `load`
raw::PackKernel planesRecord(uint32_t i, uint32_t maxThreads, bool load)
{
    raw::AirHash air;
    air.fill(uint8_t(kKernelSeed + i));
    raw::BoundTexture bound = {0, 550, MTLTextureType2D, 1, 1, {2, 3, 4, 5}};
    bound.planes = true;
    const std::string key =
        raw::computeKey(air, "", maxThreads, false, kThreads, "", raw::computeTextures(&bound, 1), "");
    raw::PackKernel k = {};
    k.keyHash = raw::computeKeyHash(key);
    k.regionWord = -1;
    k.qmd[kThreadsWord] = kThreads[0] << 16;
    k.qmd[kThreadsWord + 1] = kThreads[1] | kThreads[2] << 16;
    k.entries = 2;
    k.setBytes = 32;
    k.entry[0] = {0, uint8_t(raw::KernelTexture), 0, viewWord0(98, !load)};        // VK_FORMAT_R32_UINT
    k.entry[1] = {16, uint8_t(raw::KernelTexturePlane), 0, viewWord0(13, !load)};  // VK_FORMAT_R8_UINT
    k.readOnlyEntries = load ? 3 : 0;
    return k;
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
    int opens = 0;
};

Ran dispatch(uint32_t kernel, id<MTLTexture> t)
{
    const size_t jobs = kernel_model::model.jobs.size();
    const int opens = gOpens;
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:gStates[kernel]];
    [e setTexture:t atIndex:0];
    [e dispatchThreadgroups:MTLSizeMake(kSide / kThreads[0], kSide / kThreads[1], 1)
        threadsPerThreadgroup:MTLSizeMake(kThreads[0], kThreads[1], kThreads[2])];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return {cb.status, cb.error.code, launches(jobs), gOpens - opens};
}

class FacadeKernelXr : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const uint32_t maxThreads = uint32_t([MTLComputePipelineDescriptor new].maxTotalThreadsPerThreadgroup);
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm),
                                 {planesRecord(kStore, maxThreads, false), planesRecord(kLoad, maxThreads, true)},
                                 {kernelFacts(kStore, maxThreads), kernelFacts(kLoad, maxThreads)}, true, "",
                                 {{"raw.xr.xr_planes_merge_frag", MTLPixelFormat(103)},
                                  {"raw.xr.xr_planes_split_frag", MTLPixelFormat(53)}}),
                       [HarnessM2VMethods class], [HarnessComputeM2V class]);
        ASSERT_TRUE(gDevice != nil);
        raw_metal_complete_states(dir().c_str());
        const std::string bytes = kernelLibrary();
        dispatch_data_t data =
            dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithData:data error:&error];
        ASSERT_TRUE(library != nil) << (error.description.UTF8String ?: "");
        for (uint32_t i : {kStore, kLoad}) {
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
    EXPECT_EQ(ran.opens, 0) << "m2v ran it";
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
    EXPECT_EQ(ran.opens, 0) << "m2v ran it";
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
    EXPECT_EQ(ran.opens, 0);
    EXPECT_EQ(ran.launches, 0u);
}

}  // namespace
