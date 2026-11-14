// SPDX-License-Identifier: MIT
// Kernels on the raw facade over the kernel model (raw_facade_compute.h), as mediaanalysisd's Core Image conversion bound
// them on boot 21 (326 command buffers sent to m2v, compute.image at slot 1): luma (R8, slot 0) and chroma (RG8, slot 2)
// written and an RGBA8 texture (slot 1) read, every texture an IOSurface's whose rows are 16 bytes off 32. Admitted
// raw, the encoder fills each texture's image with the copy engine before the launch and copies the written ones back
// after it (raw_kernel_shadow.h). A memoryless texture a kernel reads is read through header 0; one it stores fails the
// command buffer (InvalidResource). None of them opens m2v.
#include "raw_facade_compute.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using namespace harness;

constexpr uint32_t kWidth = 332, kHeight = 78, kRgbaRow = 1328, kPlaneRow = 336;
constexpr uint32_t kCopy = 4, kCompute = 1;
constexpr uint32_t kLaunchDma = 0x0300, kSendPcasA = 0x02b4;  // NVB0B5_LAUNCH_DMA, NVB1C0_SEND_PCAS_A
const uint32_t kThreads[3] = {2, 2, 1};

id<MTLDevice> gDevice;
id<MTLComputePipelineState> gState;

// The kernel's record: textures 0 and 2 written, 1 read, keyed by the formats the dispatch binds
raw::PackKernel conversionRecord(uint32_t maxThreads)
{
    raw::AirHash air;
    air.fill(uint8_t(kKernelSeed));
    raw::BoundTexture bound[3] = {{0, MTLPixelFormatR8Unorm, MTLTextureType2D, 1, 1, {2, 3, 4, 5}},
                                  {1, MTLPixelFormatRGBA8Unorm, MTLTextureType2D, 1, 1, {2, 3, 4, 5}},
                                  {2, MTLPixelFormatRG8Unorm, MTLTextureType2D, 1, 1, {2, 3, 4, 5}}};
    const std::string key =
        raw::computeKey(air, "", maxThreads, false, kThreads, "", raw::computeTextures(bound, 3), "");
    raw::PackKernel k = {};
    k.keyHash = raw::computeKeyHash(key);
    k.regionWord = -1;
    k.qmd[kThreadsWord] = kThreads[0] << 16;
    k.qmd[kThreadsWord + 1] = kThreads[1] | kThreads[2] << 16;
    k.entries = 3;
    k.setBytes = 48;
    for (uint8_t i = 0; i < 3; ++i)
        k.entry[i] = {uint16_t(16 * i), uint8_t(raw::KernelTexture), i, 0};
    k.readOnlyEntries = 1u << 1;
    return k;
}

IOSurfaceRef surface(uint32_t width, uint32_t height, uint32_t bytesPerElement, uint32_t rows, uint32_t fourcc)
{
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (id)kIOSurfaceWidth : @(width),
        (id)kIOSurfaceHeight : @(height),
        (id)kIOSurfaceBytesPerElement : @(bytesPerElement),
        (id)kIOSurfaceBytesPerRow : @(rows),
        (id)kIOSurfaceAllocSize : @(rows * height),
        (id)kIOSurfacePixelFormat : @(fourcc)
    });
}

id<MTLTexture> texture(MTLPixelFormat format, uint32_t width, uint32_t height, MTLTextureUsage usage, IOSurfaceRef from)
{
    MTLTextureDescriptor *d =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:width height:height mipmapped:NO];
    d.usage = usage;
    if (!from) {
        d.storageMode = MTLStorageModeMemoryless;
        return [gDevice newTextureWithDescriptor:d];
    }
    d.storageMode = MTLStorageModeShared;
    return [gDevice newTextureWithDescriptor:d iosurface:from plane:0];
}

// The copy engine's data copies (C) and the compute launches (L) of every job submitted from `first` on, in order
std::string copiesAndLaunches(size_t first)
{
    std::string out;
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, sub = (h >> 13) & 7,
                           method = (h & 0xfff) << 2;
            if (op == 4) {  // immediate data (Push::m below 0x2000, the copy engine's launches among them): no payload
                if (sub == kCopy && method == kLaunchDma && (count & 3))
                    out += 'C';
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i) {
                const uint32_t m = op == 1 ? method + 4 * k : op == 5 ? method + (k ? 4 : 0) : method;
                if (sub == kCopy && m == kLaunchDma && (w[i] & 3))
                    out += 'C';
                else if (sub == kCompute && m == kSendPcasA)
                    out += 'L';
            }
        }
    }
    return out;
}

struct Ran {
    MTLCommandBufferStatus status;
    NSInteger error = 0;
    std::string order;
    int opens = 0;
};

Ran dispatch(id<MTLTexture> luma, id<MTLTexture> rgba, id<MTLTexture> chroma)
{
    const size_t jobs = kernel_model::model.jobs.size();
    const int opens = gOpens;
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:gState];
    [e setTexture:luma atIndex:0];
    [e setTexture:rgba atIndex:1];
    [e setTexture:chroma atIndex:2];
    [e dispatchThreadgroups:MTLSizeMake(kWidth / 2, kHeight / 2, 1)
        threadsPerThreadgroup:MTLSizeMake(kThreads[0], kThreads[1], kThreads[2])];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    return {cb.status, cb.error.code, copiesAndLaunches(jobs), gOpens - opens};
}

class FacadeKernelShadow : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const uint32_t maxThreads = uint32_t([MTLComputePipelineDescriptor new].maxTotalThreadsPerThreadgroup);
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {conversionRecord(maxThreads)}),
                       [HarnessM2VMethods class], [HarnessComputeM2V class]);
        ASSERT_TRUE(gDevice != nil);
        raw_metal_complete_states(dir().c_str());
        const std::string bytes = kernelLibrary();
        dispatch_data_t data =
            dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithData:data error:&error];
        ASSERT_TRUE(library != nil) << (error.description.UTF8String ?: "");
        MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
        d.computeFunction = [library newFunctionWithName:@(kernelName(0).c_str())];
        gState =
            [gDevice newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
        ASSERT_TRUE(gState != nil) << (error.description.UTF8String ?: "");
    }

    IOSurfaceRef lumaRows = surface(kWidth, kHeight, 1, kPlaneRow, 'L008');
    IOSurfaceRef rgbaRows = surface(kWidth, kHeight, 4, kRgbaRow, 'RGBA');
    IOSurfaceRef chromaRows = surface(kWidth / 2, kHeight / 2, 2, kPlaneRow, '2C08');

    void TearDown() override
    {
        for (IOSurfaceRef s : {lumaRows, rgbaRows, chromaRows})
            if (s)
                CFRelease(s);
    }
};

TEST_F(FacadeKernelShadow, MediaanalysisdsConversionRunsRawThroughTheImages)
{
    ASSERT_TRUE(lumaRows && rgbaRows && chromaRows);
    ASSERT_EQ(IOSurfaceGetBytesPerRow(rgbaRows) % 32, 16u);
    ASSERT_EQ(IOSurfaceGetBytesPerRow(lumaRows) % 32, 16u);
    id<MTLTexture> luma = texture(MTLPixelFormatR8Unorm, kWidth, kHeight, MTLTextureUsageShaderWrite, lumaRows);
    id<MTLTexture> rgba = texture(MTLPixelFormatRGBA8Unorm, kWidth, kHeight, MTLTextureUsageShaderRead, rgbaRows);
    id<MTLTexture> chroma =
        texture(MTLPixelFormatRG8Unorm, kWidth / 2, kHeight / 2, MTLTextureUsageShaderWrite, chromaRows);
    ASSERT_TRUE(luma && rgba && chroma);
    // the first dispatch finds the state's kernel in the pack off the encoding thread; the second has it
    dispatch(luma, rgba, chroma);
    const Ran ran = dispatch(luma, rgba, chroma);
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(ran.opens, 0) << "m2v ran it";
    const size_t launch = ran.order.find('L');
    ASSERT_NE(launch, std::string::npos) << ran.order;
    EXPECT_EQ(ran.order.find('L', launch + 1), std::string::npos) << ran.order;
    EXPECT_GE(launch, 3u) << "the three images filled before the launch: " << ran.order;
    EXPECT_GE(ran.order.size() - launch - 1, 2u) << "the two written images copied back after it: " << ran.order;
}

TEST_F(FacadeKernelShadow, AMemorylessTextureIsReadThroughHeaderZero)
{
    id<MTLTexture> luma = texture(MTLPixelFormatR8Unorm, kWidth, kHeight, MTLTextureUsageShaderWrite, lumaRows);
    id<MTLTexture> rgba = texture(MTLPixelFormatRGBA8Unorm, kWidth, kHeight,
                                  MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget, nullptr);
    id<MTLTexture> chroma =
        texture(MTLPixelFormatRG8Unorm, kWidth / 2, kHeight / 2, MTLTextureUsageShaderWrite, chromaRows);
    ASSERT_TRUE(luma && rgba && chroma);
    dispatch(luma, rgba, chroma);
    const Ran ran = dispatch(luma, rgba, chroma);
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(ran.opens, 0);
    const size_t launch = ran.order.find('L');
    ASSERT_NE(launch, std::string::npos) << "launched raw, slot 1 bound to header 0: " << ran.order;
    EXPECT_GE(launch, 2u) << "the planes' images filled: " << ran.order;
    EXPECT_GE(ran.order.size() - launch - 1, 2u) << "and copied back: " << ran.order;
}

TEST_F(FacadeKernelShadow, AStoreToAMemorylessTextureFailsTheCommandBuffer)
{
    id<MTLTexture> luma = texture(MTLPixelFormatR8Unorm, kWidth, kHeight,
                                  MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget, nullptr);
    id<MTLTexture> rgba = texture(MTLPixelFormatRGBA8Unorm, kWidth, kHeight, MTLTextureUsageShaderRead, rgbaRows);
    id<MTLTexture> chroma =
        texture(MTLPixelFormatRG8Unorm, kWidth / 2, kHeight / 2, MTLTextureUsageShaderWrite, chromaRows);
    ASSERT_TRUE(luma && rgba && chroma);
    dispatch(luma, rgba, chroma);
    const Ran ran = dispatch(luma, rgba, chroma);
    EXPECT_EQ(ran.status, MTLCommandBufferStatusError);
    EXPECT_EQ(ran.error, MTLCommandBufferErrorInvalidResource);
    EXPECT_EQ(ran.opens, 0);
    EXPECT_EQ(ran.order.find('L'), std::string::npos) << ran.order;
}

}  // namespace
