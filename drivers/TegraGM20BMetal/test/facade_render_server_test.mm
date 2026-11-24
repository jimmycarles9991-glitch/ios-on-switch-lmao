// SPDX-License-Identifier: MIT
// The render server's device (metal_prepare, then metal_device_open) over the kernel model: the memory its frames left
// idle stays through an idle second, and memory pressure gives it back (GpuMemoryRelease.h).
#include "facade_harness.h"
#include <chrono>
#include <thread>

namespace {

using namespace std::chrono_literals;

size_t freed()
{
    std::lock_guard<std::mutex> l(kernel_model::callLock);
    return kernel_model::model.freed.size();
}

TEST(FacadeRenderServer, AnIdleSecondGivesNothingBackAndMemoryPressureDoes)
{
    const std::string pack = emptyPack();
    metal_prepare(pack.c_str(), 0);
    NSString *why = nil;
    id<MTLDevice> device = metal_device_open(pack.c_str(), 0, harness::kRegistryID, &why);
    ASSERT_NE(device, nil) << (why ? why.UTF8String : "");
    @autoreleasepool {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                     width:64
                                                                                    height:64
                                                                                 mipmapped:NO];
        d.storageMode = MTLStorageModePrivate;
        d.usage = MTLTextureUsageRenderTarget;
        id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = [device newTextureWithDescriptor:d];
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:pass] endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    }
    std::this_thread::sleep_for(300ms);
    const size_t drawn = freed();
    // GpuMemoryRelease's idle second, the timer's leeway and a margin, from the drain
    std::this_thread::sleep_for(1000ms + 100ms + 400ms);
    EXPECT_EQ(freed(), drawn) << "an idle second gave the render server's memory back";
    gRawRelease.release();
    EXPECT_GT(freed(), drawn) << "memory pressure gave none of the render server's idle memory back";
}

}  // namespace
