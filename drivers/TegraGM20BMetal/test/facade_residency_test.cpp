// SPDX-License-Identifier: MIT
// Residency sets (TegraGM20BMetalDeviceBase.mm gm20bNameResidencySets), through the facade over the kernel model: a
// program may reach a set's allocations by GPU address alone, so a job of a command buffer that uses a set holding a
// host-visible allocation is host-written and External, from the command buffer's set and from its queue's. A set of
// Private allocations alone names nothing at a range's seal, however many it holds: the set takes its host-visible
// allocations once, at its commit.
#include "facade_harness.h"

#include <IOSurface/IOSurface.h>
#include <gtest/gtest.h>

namespace {

using namespace harness;

constexpr NSUInteger kLength = 4096;

IOSurfaceRef makeSurface()
{
    return IOSurfaceCreate((__bridge CFDictionaryRef) @{
        (__bridge NSString *)kIOSurfaceWidth : @32,
        (__bridge NSString *)kIOSurfaceHeight : @32,
        (__bridge NSString *)kIOSurfaceBytesPerElement : @4,
        (__bridge NSString *)kIOSurfaceBytesPerRow : @256,
        (__bridge NSString *)kIOSurfaceAllocSize : @(256 * 32),
        (__bridge NSString *)kIOSurfacePixelFormat : @(uint32_t('BGRA')),
    });
}

id<MTLTexture> makeSurfaceTexture(id<MTLDevice> device, IOSurfaceRef surface, MTLTextureUsage usage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:32
                                                                                height:32
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = usage;
    return [device newTextureWithDescriptor:d iosurface:surface plane:0];
}

id<MTLTexture> makeBufferTexture(id<MTLDevice> device, MTLTextureUsage usage)
{
    id<MTLBuffer> backing = [device newBufferWithLength:256 * 32 options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:32
                                                                                height:32
                                                                             mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    d.usage = usage;
    return [backing newTextureWithDescriptor:d offset:0 bytesPerRow:256];
}

long counted(id<MTLDevice> device, NSString *key)
{
    return [counters(device)[key] longValue];
}

id<MTLResidencySet> setOf(id<MTLDevice> device, NSArray<id<MTLAllocation>> *allocations)
{
    MTLResidencySetDescriptor *d = [MTLResidencySetDescriptor new];
    NSError *error = nil;
    id<MTLResidencySet> set = [device newResidencySetWithDescriptor:d error:&error];
    EXPECT_NE(set, nil);
    for (id<MTLAllocation> allocation in allocations)
        [set addAllocation:allocation];
    [set commit];
    return set;
}

// A fill of `target` alone, its command buffer using `set` (nil: none)
kernel_model::Job filled(id<MTLCommandQueue> queue, id<MTLBuffer> target, id<MTLResidencySet> set)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    if (set)
        [cb useResidencySet:set];
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b fillBuffer:target range:NSMakeRange(0, kLength) value:0x11];
    [b endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    return kernel_model::model.jobs.back();
}

bool coherent(const kernel_model::Job &job)
{
    return job.hostWritten && (job.flags & TegraGM20BJobExternal);
}

TEST(GM20BFacadeResidency, ASetHoldingASharedBufferMakesItsJobsHostWrittenAndExternal)
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_NE(device, nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> target = [device newBufferWithLength:kLength options:MTLResourceStorageModePrivate];
    id<MTLBuffer> shared = [device newBufferWithLength:kLength options:MTLResourceStorageModeShared];
    EXPECT_FALSE(coherent(filled(queue, target, nil))) << "the control: a private fill alone";
    id<MTLResidencySet> set = setOf(device, @[ shared, target ]);
    const long uses = counted(device, @"residency_set_uses"), named = counted(device, @"residency_named");
    EXPECT_TRUE(coherent(filled(queue, target, set))) << "a set's Shared buffer reached by GPU address alone";
    EXPECT_EQ(counted(device, @"residency_set_uses") - uses, 1);
    EXPECT_EQ(counted(device, @"residency_named") - named, 1) << "the Shared buffer alone";

    id<MTLCommandQueue> resident = [device newCommandQueue];
    [resident addResidencySet:set];
    EXPECT_TRUE(coherent(filled(resident, target, nil))) << "the queue's set";
    [resident removeResidencySet:set];
    EXPECT_FALSE(coherent(filled(resident, target, nil))) << "removed from the queue";
}

TEST(GM20BFacadeResidency, ASetOfPrivateAllocationsNamesNothingAtASeal)
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_NE(device, nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    NSMutableArray<id<MTLAllocation>> *privates = [NSMutableArray array];
    for (int i = 0; i < 256; i++)
        [privates addObject:[device newBufferWithLength:kLength options:MTLResourceStorageModePrivate]];
    id<MTLResidencySet> set = setOf(device, privates);
    const long named = counted(device, @"residency_named");
    EXPECT_FALSE(coherent(filled(queue, (id<MTLBuffer>)privates[0], set)));
    EXPECT_EQ(counted(device, @"residency_named") - named, 0) << "a seal walked the set's Private allocations";
}

TEST(GM20BFacadeResidency, SurfaceDrawablesAreNotNamedWrittenAndTextureAccessFollowsUsageAndResourceId)
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_NE(device, nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> target = [device newBufferWithLength:kLength options:MTLResourceStorageModePrivate];

    IOSurfaceRef surface = makeSurface();
    ASSERT_NE(surface, nullptr);
    id<MTLTexture> drawable =
        makeSurfaceTexture(device, surface, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    id<MTLTexture> unknownSurface = makeSurfaceTexture(device, surface, MTLTextureUsageUnknown);
    ASSERT_NE(drawable, nil);
    ASSERT_NE(unknownSurface, nil);
    id<MTLResidencySet> drawableSet = setOf(device, @[ drawable, unknownSurface ]);

    const uint32_t surfaceHandle = ((SwitchMetalTexture *)drawable)->_raw.import->bo.handle;
    ASSERT_NE(surfaceHandle, 0u);
    auto surfaceUse = [&](const kernel_model::Job &job) -> const TegraGM20BAbiSurfaceUse * {
        for (const TegraGM20BAbiSurfaceUse &u : job.uses)
            if (u.handle == surfaceHandle)
                return &u;
        return nullptr;
    };

    const long namedBefore = counted(device, @"residency_named");
    const kernel_model::Job unindexed = filled(queue, target, drawableSet);
    EXPECT_EQ(counted(device, @"residency_named") - namedBefore, 0)
        << "textures whose gpuResourceID was never asked cannot be reached via a residency set";
    EXPECT_FALSE(unindexed.hostWritten);
    EXPECT_EQ(unindexed.flags & TegraGM20BJobExternal, 0u);
    EXPECT_EQ(surfaceUse(unindexed), nullptr) << "unbound IOSurface drawables must not enter job.uses";

    (void)drawable.gpuResourceID;
    (void)unknownSurface.gpuResourceID;
    const kernel_model::Job surfaceRead = filled(queue, target, drawableSet);
    EXPECT_EQ(surfaceRead.flags & TegraGM20BJobExternal, 0u)
        << "a ShaderRead or Unknown IOSurface in a residency set must not mark the job External";
    const TegraGM20BAbiSurfaceUse *readUse = surfaceUse(surfaceRead);
    ASSERT_NE(readUse, nullptr) << "a gpuResourceID-named IOSurface must be named on the job";
    EXPECT_EQ(readUse->access & TegraGM20BSurfaceWrite, 0u)
        << "a ShaderRead or Unknown IOSurface in a residency set must not be marked written";

    id<MTLTexture> readOnlyBufTex = makeBufferTexture(device, MTLTextureUsageShaderRead);
    id<MTLTexture> writableBufTex = makeBufferTexture(device, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    ASSERT_NE(readOnlyBufTex, nil);
    ASSERT_NE(writableBufTex, nil);
    (void)readOnlyBufTex.gpuResourceID;
    (void)writableBufTex.gpuResourceID;

    const kernel_model::Job readJob = filled(queue, target, setOf(device, @[ readOnlyBufTex ]));
    EXPECT_TRUE(readJob.hostWritten) << "a ShaderRead Shared texture still drops stale L2 lines";
    EXPECT_EQ(readJob.flags & TegraGM20BJobExternal, 0u) << "a ShaderRead Shared texture does not flush External";

    const kernel_model::Job writeJob = filled(queue, target, setOf(device, @[ writableBufTex ]));
    EXPECT_TRUE(coherent(writeJob)) << "a ShaderWrite Shared texture is host-written and External";

    id<MTLTexture> parentTex = makeBufferTexture(device, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite);
    id<MTLTexture> viewTex = [parentTex newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm];
    ASSERT_NE(viewTex, nil);
    (void)viewTex.gpuResourceID;
    const kernel_model::Job viewJob = filled(queue, target, setOf(device, @[ parentTex ]));
    EXPECT_TRUE(coherent(viewJob)) << "a view's gpuResourceID activates its parent texture in a residency set";

    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.size = 64 * 1024;
    hd.storageMode = MTLStorageModeShared;
    id<MTLHeap> sharedHeap = [device newHeapWithDescriptor:hd];
    ASSERT_NE(sharedHeap, nil);
    const kernel_model::Job heapJob = filled(queue, target, setOf(device, @[ sharedHeap ]));
    EXPECT_TRUE(coherent(heapJob)) << "a Shared MTLHeap in a residency set is host-written and External";

    CFRelease(surface);
}

TEST(GM20BFacadeResidency, CommitSnapshotSurvivesQueueRemovalSetRecommitAndRetainsUntilCompletion)
{
    id<MTLDevice> device = open(writePack(packKey(MTLPixelFormatBGRA8Unorm)));
    ASSERT_NE(device, nil);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> target = [device newBufferWithLength:kLength options:MTLResourceStorageModePrivate];
    id<MTLSharedEvent> gate = [device newSharedEvent];
    ASSERT_NE(gate, nil);

    __weak id<MTLBuffer> weakShared = nil;
    id<MTLCommandBuffer> cb = nil;
    id<MTLResidencySet> set = nil;
    @autoreleasepool {
        id<MTLBuffer> shared = [device newBufferWithLength:kLength options:MTLResourceStorageModeShared];
        ASSERT_NE(shared, nil);
        weakShared = shared;
        set = setOf(device, @[ shared ]);
        [queue addResidencySet:set];

        cb = [queue commandBuffer];
        [cb encodeWaitForEvent:gate value:1];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b fillBuffer:target range:NSMakeRange(0, kLength) value:0x22];
        [b endEncoding];
        [cb commit];

        [queue removeResidencySet:set];
        [set removeAllAllocations];
        [set commit];
    }

    EXPECT_NE(weakShared, nil) << "an in-flight command buffer must retain its committed residency snapshot";
    gate.signaledValue = 1;
    [cb waitUntilCompleted];
    EXPECT_EQ(cb.status, MTLCommandBufferStatusCompleted);
    EXPECT_TRUE(coherent(kernel_model::model.jobs.back()))
        << "the commit-time snapshot must survive queue removal and set recommit before seal";
    EXPECT_EQ(weakShared, nil) << "completion must release the snapshotted residency allocations";
}

}  // namespace
