// SPDX-License-Identifier: MIT
// dispatchThreadgroupsWithIndirectBuffer: on the raw facade, end to end on the host (raw_facade_compute.h): the model GPU
// (model_gpu.h) launches the kernel over the grid an earlier launch of the same command buffer wrote into the arguments,
// which no CPU wrote. Before the raw path took the selector it was recorded as an invocation for m2v, whose stand-in here
// launches nothing.
#include "raw_facade_compute.h"
#include "model_gpu.h"
#include "../../../kits/SwitchGM20B/SwitchGM20BCompute.h"

#include <gtest/gtest.h>
#include <array>
#include <vector>

namespace {

using namespace harness;

// QMDV01_07's CTA_RASTER_WIDTH, then CTA_RASTER_HEIGHT and CTA_RASTER_DEPTH (clb1c0qmd.h, bits 384-447), and constant
// buffer 0's address (the root table)
constexpr uint32_t kRasterWidth = 12, kRasterHeightDepth = 13, kQmdRootLo = 29;
// Kernel 2 reads a grid builtin: m2v's DispatchRegion field at root push word 8
constexpr uint32_t kGridKernel = 2, kRootPush = 0x700;
constexpr int8_t kGridRegionWord = 8;

id<MTLDevice> gDevice;
id<MTLComputePipelineState> gStates[3];

class FacadeIndirectDispatch : public ::testing::Test {
protected:
    static void SetUpTestSuite()
    {
        const uint32_t maxThreads = uint32_t([MTLComputePipelineDescriptor new].maxTotalThreadsPerThreadgroup);
        std::vector<raw::PackKernel> kernels;
        std::vector<raw::PackComputeFacts> facts;
        for (uint32_t i = 0; i < kKernels; i++) {
            kernels.push_back(kernelRecord(i, maxThreads));
            facts.push_back(kernelFacts(i, maxThreads));
        }
        kernels[kGridKernel].regionWord = kGridRegionWord;
        kernels[kGridKernel].qmd[SWITCH_NVB1C0_QMD_WORD(NVB1C0_QMDV01_07_CONSTANT_BUFFER_VALID(0))] |= 1;
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), kernels, facts), [HarnessM2VMethods class],
                       [HarnessComputeM2V class]);
        ASSERT_TRUE(gDevice != nil);
        raw_metal_complete_states(dir().c_str());
        const std::string bytes = kernelLibrary();
        dispatch_data_t data =
            dispatch_data_create(bytes.data(), bytes.size(), nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
        NSError *error = nil;
        id<MTLLibrary> library = [gDevice newLibraryWithData:data error:&error];
        ASSERT_TRUE(library != nil) << (error.description.UTF8String ?: "");
        for (uint32_t i = 0; i < 3; i++) {
            MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
            d.computeFunction = [library newFunctionWithName:@(kernelName(i).c_str())];
            gStates[i] = [gDevice newComputePipelineStateWithDescriptor:d
                                                                options:MTLPipelineOptionNone
                                                             reflection:nil
                                                                  error:&error];
            ASSERT_TRUE(gStates[i] != nil) << i << ": " << (error.description.UTF8String ?: "");
        }
        // each state's kernel is looked up in the pack off the encoding thread: once found, every later dispatch has it
        id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        for (uint32_t i = 0; i < 3; i++) {
            [e setComputePipelineState:gStates[i]];
            [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(i + 1, 1, 1)];
        }
        [e endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    }
};

TEST_F(FacadeIndirectDispatch, TheGridAnEarlierLaunchWroteIsTheLaunchesRaster)
{
    id<MTLBuffer> arguments = [gDevice newBufferWithLength:64 options:MTLResourceStorageModeShared];
    ASSERT_TRUE(arguments != nil);
    memset(arguments.contents, 0, 64);
    const std::array<uint32_t, 3> grid = {70000, 3, 513};
    std::vector<std::array<uint32_t, 3>> launched;
    uint32_t writes = 0;
    model_gpu::Gpu gpu([&](uint32_t *qmd, predication_model::Gpu &) {
        if (kernelOfQmd(qmd) == 0) {
            memcpy(static_cast<uint8_t *>(arguments.contents) + 16, grid.data(), sizeof grid);
            writes++;
            return;
        }
        launched.push_back({qmd[kRasterWidth], qmd[kRasterHeightDepth] & 0xffff, qmd[kRasterHeightDepth] >> 16});
    });
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:gStates[0]];
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [e setComputePipelineState:gStates[1]];
    [e dispatchThreadgroupsWithIndirectBuffer:arguments
                         indirectBufferOffset:16
                        threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_EQ(writes, 1u) << "the writer launched once";
    ASSERT_EQ(launched.size(), 1u) << "the indirect dispatch launched on the raw path";
    EXPECT_EQ(launched[0], grid);
}

// gpu.raw.indirect_dispatch and metal.native.shared_event: the indirect dispatch of a kernel that reads its grid (m2v's
// DispatchRegion field) launches on the raw path with the region record of an indirect dispatch, where it failed its
// command buffer ("an indirect dispatch of a kernel compiled for the regions of an edge")
TEST_F(FacadeIndirectDispatch, AKernelThatReadsItsGridLaunchesOverTheGridAnEarlierLaunchWrote)
{
    id<MTLBuffer> arguments = [gDevice newBufferWithLength:64 options:MTLResourceStorageModeShared];
    ASSERT_TRUE(arguments != nil);
    memset(arguments.contents, 0, 64);
    const std::array<uint32_t, 3> grid = {3, 2, 2};
    std::vector<std::array<uint32_t, 3>> launched;
    std::vector<std::array<uint32_t, 8>> regions;
    model_gpu::Gpu gpu([&](uint32_t *qmd, predication_model::Gpu &g) {
        if (kernelOfQmd(qmd) == 0) {
            memcpy(arguments.contents, grid.data(), sizeof grid);
            return;
        }
        launched.push_back({qmd[kRasterWidth], qmd[kRasterHeightDepth] & 0xffff, qmd[kRasterHeightDepth] >> 16});
        const uint64_t root = uint64_t(qmd[kQmdRootLo + 1] & 0xff) << 32 | qmd[kQmdRootLo];
        std::array<uint32_t, 8> words{};
        memcpy(words.data(), g.at(root + kRootPush + 4 * kGridRegionWord), sizeof words);
        regions.push_back(words);
    });
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:gStates[0]];
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [e setComputePipelineState:gStates[kGridKernel]];
    [e dispatchThreadgroupsWithIndirectBuffer:arguments
                         indirectBufferOffset:0
                        threadsPerThreadgroup:MTLSizeMake(kGridKernel + 1, 1, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted) << (cb.error.description.UTF8String ?: "");
    ASSERT_EQ(launched.size(), 1u) << "the indirect dispatch launched on the raw path";
    EXPECT_EQ(launched[0], grid);
    const std::array<uint32_t, 8> want = {0, 0, 0, kGridKernel, 0, 0, 0, 0};
    EXPECT_EQ(regions[0], want) << "base 0, the nominal threadgroup packed (x - 1), grid 0: m2v's indirect record";
}

}  // namespace
