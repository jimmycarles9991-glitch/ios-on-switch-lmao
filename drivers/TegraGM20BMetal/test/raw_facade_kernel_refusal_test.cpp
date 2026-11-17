// SPDX-License-Identifier: MIT
// A dispatch whose kernel the compiler refused (HW Test gpu.upload.blit_buffer and gpu.upload.replace_region: "a kernel
// the pack lacks is refused ... a texture view of VkFormat 1000470001 NVK has no header for"): its command buffer leaves
// the raw path with an Internal error named by compute.nokernel, never replayed elsewhere.
#include "raw_facade_compute.h"
#include "../raw/runtime/raw_cache.h"

#include <gtest/gtest.h>

namespace {

using namespace harness;

constexpr uint32_t kRefusedThreads = kKernels + 3;

id<MTLDevice> gDevice;
id<MTLComputePipelineState> gState;

class FacadeKernelRefusal : public ::testing::Test {
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
        gDevice = open(writePack(packKey(MTLPixelFormatBGRA8Unorm), kernels, facts), [HarnessM2VMethods class],
                       [HarnessComputeM2V class]);
        ASSERT_TRUE(gDevice != nil);
        raw::AirHash air;
        air.fill(kKernelSeed);
        const uint32_t threads[3] = {kRefusedThreads, 1, 1};
        raw::CacheKey refused;
        refused.key = "kernel | " + raw::computeKey(air, "", maxThreads, false, threads, "", "", "");
        raw::RecordCache cache;
        ASSERT_TRUE(cache.attach(dir(), 0, 1 << 20));
        ASSERT_TRUE(cache.storeKernel(refused, nullptr, "a texture view of VkFormat 1000470001 NVK has no header for"));
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
};

TEST_F(FacadeKernelRefusal, ADispatchOfARefusedKernelNamesComputeNoKernel)
{
    id<MTLCommandBuffer> cb = [[gDevice newCommandQueue] commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:gState];
    [e dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(kRefusedThreads, 1, 1)];
    [e endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    NSString *path = [(id)cb valueForKey:@"switchRawPath"];
    ASSERT_TRUE(path != nil);
    EXPECT_TRUE([path isEqualToString:@"error:compute.nokernel"]) << path.UTF8String;
    EXPECT_EQ(cb.status, MTLCommandBufferStatusError);
    ASSERT_TRUE(cb.error != nil);
    EXPECT_TRUE([cb.error.domain isEqualToString:MTLCommandBufferErrorDomain]);
    EXPECT_EQ(cb.error.code, MTLCommandBufferErrorInternal);
    EXPECT_EQ(gOpens, 0);
    EXPECT_EQ([m2vRecord(gDevice)[@"needs"] count], 0u);
}

}  // namespace
