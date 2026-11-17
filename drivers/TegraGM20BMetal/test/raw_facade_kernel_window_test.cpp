// SPDX-License-Identifier: MIT
// A kernel's buffer bound at an address off 16 bytes whose record names no residual word for it, on the raw facade over the
// kernel model (raw_facade_compute.h): its bytes reach the kernel through a window of them on 16 that the copy engine
// fills before the launch and copies back after it (raw_kernel_windows.h), where raw once refused it as compute.off16. The
// same buffer bound in place by a later dispatch is another resource over those bytes: the window is copied back before
// that dispatch. A texture over a buffer whose rows and start are 16 bytes off 32 is sampled through a pool image the copy
// engine fills (raw_gpu.cpp createBufferTexture), where raw once refused to make it. None of it opens m2v.
#include "raw_facade_compute.h"

#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

using namespace harness;

constexpr uint32_t kCopy = 4, kCompute = 1, kBytes = 256, kOffset = 4;
// NVB0B5 OFFSET_IN_UPPER .. OFFSET_OUT_LOWER, LINE_LENGTH_IN, LAUNCH_DMA; NVB1C0 SEND_PCAS_A
constexpr uint32_t kOffsetIn = 0x0400, kOffsetOutLower = 0x040c, kLineLength = 0x0418, kLaunchDma = 0x0300,
                   kSendPcasA = 0x02b4;

id<MTLDevice> gDevice;
id<MTLComputePipelineState> gState, gTextureState;

// Kernel 0's record with one buffer entry at slot 0 that it writes, its residual word none
raw::PackKernel windowRecord(uint32_t maxThreads)
{
    raw::PackKernel k = kernelRecord(0, maxThreads);
    k.entries = 1;
    k.setBytes = 16;
    k.entry[0] = {0, uint8_t(raw::KernelBuffer), 0, 0};
    return k;
}

// Kernel 1's record: the RGBA8 texture at slot 0, read
raw::PackKernel textureRecord(uint32_t maxThreads)
{
    raw::AirHash air;
    air.fill(uint8_t(kKernelSeed + 1));
    const uint32_t threads[3] = {2, 1, 1};
    raw::BoundTexture bound[1] = {{0, MTLPixelFormatRGBA8Unorm, MTLTextureType2D, 1, 1, {2, 3, 4, 5}}};
    const std::string key =
        raw::computeKey(air, "", maxThreads, false, threads, "", raw::computeTextures(bound, 1), "");
    raw::PackKernel k = kernelRecord(1, maxThreads);
    k.keyHash = raw::computeKeyHash(key);
    k.entries = 1;
    k.setBytes = 16;
    k.entry[0] = {0, uint8_t(raw::KernelTexture), 0, 0};
    k.readOnlyEntries = 1;
    return k;
}

struct Copy {
    uint64_t in, out;
    uint32_t bytes;
};

struct Submitted {
    std::string order;  // C a data copy, L a launch
    std::vector<Copy> copies;
};

// The copy engine's data copies and the compute launches of every job submitted from `first` on, in order
Submitted submitted(size_t first)
{
    Submitted s;
    uint32_t offsets[4] = {}, line = 0;
    auto method = [&](uint32_t sub, uint32_t m, uint32_t value) {
        if (sub == kCopy && m >= kOffsetIn && m <= kOffsetOutLower)
            offsets[(m - kOffsetIn) / 4] = value;
        else if (sub == kCopy && m == kLineLength)
            line = value;
        else if (sub == kCopy && m == kLaunchDma && (value & 3)) {
            s.order += 'C';
            s.copies.push_back(
                {uint64_t(offsets[0]) << 32 | offsets[1], uint64_t(offsets[2]) << 32 | offsets[3], line});
        } else if (sub == kCompute && m == kSendPcasA) {
            s.order += 'L';
        }
    };
    for (size_t j = first; j < kernel_model::model.jobs.size(); ++j) {
        const std::vector<uint32_t> &w = kernel_model::model.jobs[j].words;
        for (size_t i = 0; i < w.size();) {
            const uint32_t h = w[i++], op = h >> 29, count = (h >> 16) & 0x1fff, sub = (h >> 13) & 7,
                           m = (h & 0xfff) << 2;
            if (op == 4) {  // immediate data: the value in the header's count field
                method(sub, m, count);
                continue;
            }
            for (uint32_t k = 0; k < count && i < w.size(); ++k, ++i)
                method(sub, op == 1 ? m + 4 * k : op == 5 ? m + (k ? 4 : 0) : m, w[i]);
        }
    }
    return s;
}

class FacadeKernelWindow : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const uint32_t maxThreads = uint32_t([MTLComputePipelineDescriptor new].maxTotalThreadsPerThreadgroup);
        gDevice =
            open(writePack(packKey(MTLPixelFormatBGRA8Unorm), {windowRecord(maxThreads), textureRecord(maxThreads)},
                           {kernelFacts(0, maxThreads), kernelFacts(1, maxThreads)}),
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
        d.computeFunction = [library newFunctionWithName:@(kernelName(1).c_str())];
        gTextureState =
            [gDevice newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&error];
        ASSERT_TRUE(gTextureState != nil) << (error.description.UTF8String ?: "");
    }

    // One command buffer of a dispatch per offset, each binding `buffer` at slot 0 there, in one encoder
    struct Ran {
        MTLCommandBufferStatus status;
        Submitted work;
        int opens;
    };

    Ran dispatch(id<MTLBuffer> buffer, std::initializer_list<NSUInteger> offsets)
    {
        const size_t jobs = kernel_model::model.jobs.size();
        const int opens = gOpens;
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:gState];
        for (NSUInteger offset : offsets) {
            [e setBuffer:buffer offset:offset atIndex:0];
            [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        }
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        return {cb.status, submitted(jobs), gOpens - opens};
    }
};

TEST_F(FacadeKernelWindow, ABufferOffSixteenRunsRawThroughAWindowFilledAndCopiedBack)
{
    id<MTLBuffer> buffer = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    ASSERT_TRUE(buffer != nil);
    const uint64_t at = buffer.gpuAddress + kOffset;
    ASSERT_NE(at & 15, 0u);
    dispatch(buffer, {kOffset});  // the first finds the kernel in the pack off the encoding thread; the second has it
    const Ran ran = dispatch(buffer, {kOffset});
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(ran.opens, 0) << "m2v ran it";
    ASSERT_EQ(ran.work.order, "CLC");
    const Copy fill = ran.work.copies[0], back = ran.work.copies[1];
    EXPECT_EQ(fill.in, at);
    EXPECT_EQ(fill.out & 15, 0u) << "the window is on 16";
    EXPECT_EQ(fill.bytes, kBytes - kOffset);
    EXPECT_EQ(back.in, fill.out);
    EXPECT_EQ(back.out, at);
    EXPECT_EQ(back.bytes, kBytes - kOffset);
}

TEST_F(FacadeKernelWindow, TheWindowIsCopiedBackBeforeTheBufferIsReachedInPlace)
{
    id<MTLBuffer> buffer = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    ASSERT_TRUE(buffer != nil);
    dispatch(buffer, {kOffset, 0});
    const Ran ran = dispatch(buffer, {kOffset, 0});
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(ran.opens, 0);
    EXPECT_EQ(ran.work.order, "CLCL") << "fill, the window's launch, its copy back, the in-place launch";
}

TEST_F(FacadeKernelWindow, ATextureOverABufferWithRowsOffThirtyTwoIsSampledThroughItsPoolImage)
{
    constexpr NSUInteger kRow = 48, kStart = 48, kWidth = 12, kHeight = 8;
    id<MTLBuffer> buffer = [gDevice newBufferWithLength:kStart + kRow * kHeight options:MTLResourceStorageModeShared];
    ASSERT_TRUE(buffer != nil);
    ASSERT_EQ((buffer.gpuAddress + kStart) % 32, 16u);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                 width:kWidth
                                                                                height:kHeight
                                                                             mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead;
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [buffer newTextureWithDescriptor:d offset:kStart bytesPerRow:kRow];
    ASSERT_TRUE(texture != nil);
    auto run = [&] {
        const size_t jobs = kernel_model::model.jobs.size();
        const int opens = gOpens;
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:gTextureState];
        [e setTexture:texture atIndex:0];
        [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        return Ran{cb.status, submitted(jobs), gOpens - opens};
    };
    run();
    const Ran ran = run();
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(ran.opens, 0) << "m2v ran it";
    EXPECT_EQ(ran.work.order, "CL") << "its pool image filled from the rows, then the launch; nothing written back";
}

// raw_dispatch_regions.h: threads past 2^32 - 1 on an axis have no thread_position_in_grid; the command buffer fails
// with an error line, nothing launched, m2v never opened (raw once refused it to m2v as compute.nokernel)
TEST_F(FacadeKernelWindow, AGridPastAThreadsPositionFailsTheCommandBuffer)
{
    id<MTLBuffer> buffer = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    ASSERT_TRUE(buffer != nil);
    for (int run = 0; run < 2; ++run) {
        const size_t jobs = kernel_model::model.jobs.size();
        const int opens = gOpens;
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:gState];
        [e setBuffer:buffer offset:0 atIndex:0];
        [e dispatchThreads:MTLSizeMake(NSUInteger(1) << 32, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
        EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInvalidResource);
        EXPECT_EQ(gOpens - opens, 0);
        EXPECT_EQ(submitted(jobs).order.find('L'), std::string::npos);
    }
}

TEST_F(FacadeKernelWindow, ABufferOnSixteenStaysInPlace)
{
    id<MTLBuffer> buffer = [gDevice newBufferWithLength:kBytes options:MTLResourceStorageModeShared];
    ASSERT_TRUE(buffer != nil);
    dispatch(buffer, {16});
    const Ran ran = dispatch(buffer, {16});
    EXPECT_EQ(ran.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(ran.work.order, "L");
}

}  // namespace
