// SPDX-License-Identifier: MIT
#include "device_harness.h"
#include "facade_harness.h"
#include "predication_model.h"

namespace {

void runBlitJobs(size_t firstJob)
{
    predication_model::Gpu gpu(0);
    gpu.map = [](uint64_t va) {
        return kernel_model::model.host(va);
    };
    for (size_t j = firstJob; j < kernel_model::model.jobs.size(); ++j)
        for (const auto &push : kernel_model::model.jobs[j].pushes) {
            const auto *words =
                reinterpret_cast<const uint32_t *>(kernel_model::model.host(push.va, uint64_t(push.dwords) * 4));
            ASSERT_NE(words, nullptr);
            gpu.run(words, push.dwords, true);
        }
}

TEST(FacadeParkedHeap, SharedHeapParksOnGiveBackAndRestoresResidentAndReclaimedAtIdenticalContents)
{
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    @autoreleasepool {
        id<MTLDevice> device = facade::device();
        ASSERT_NE(device, nil);

        const size_t importsBefore = kernel_model::model.imports.size();
        id<MTLHeap> h = facade::heap(MTLStorageModeShared, 1u << 20);
        ASSERT_NE(h, nil);
        const uint32_t index = ((SwitchMetalHeap *)h)->_arena - 1;
        const uint64_t initialHandle = gm20b::DeviceHarness::arena(*gRM->raw, index).handle;
        ASSERT_NE(initialHandle, 0u);
        EXPECT_EQ(kernel_model::model.imports.size(), importsBefore);

        id<MTLBuffer> buf = [h newBufferWithLength:65536 options:MTLResourceStorageModeShared];
        ASSERT_NE(buf, nil);
        void *const stablePtr = buf.contents;
        ASSERT_NE(stablePtr, nullptr);
        memset(stablePtr, 0x5A, 65536);

        EXPECT_EQ([h setPurgeableState:MTLPurgeableStateVolatile], MTLPurgeableStateNonVolatile);
        gRawRelease.release();
        EXPECT_TRUE(gRM->raw->arenaPurged(index));
        EXPECT_GE(kernel_model::model.parkCalls, 1u);
        EXPECT_EQ(kernel_model::model.badUnmaps, 0u);
        EXPECT_EQ([h setPurgeableState:MTLPurgeableStateKeepCurrent], MTLPurgeableStateEmpty);

        // Resident unpark preserves the backing handle, [buf contents], and page contents.
        EXPECT_EQ([h setPurgeableState:MTLPurgeableStateNonVolatile], MTLPurgeableStateEmpty);
        EXPECT_FALSE(gRM->raw->arenaPurged(index));
        EXPECT_GE(kernel_model::model.unparkResident, 1u);
        EXPECT_EQ(buf.contents, stablePtr);
        EXPECT_EQ(gm20b::DeviceHarness::arena(*gRM->raw, index).handle, initialHandle);
        const auto *bytes = static_cast<const uint8_t *>(buf.contents);
        EXPECT_TRUE(std::all_of(bytes, bytes + 65536, [](uint8_t v) {
            return v == 0x5A;
        }));

        // Reclaimed restore maps a fresh BO at the exact stable [buf contents] via MapFixed and completes a GPU blit.
        EXPECT_EQ([h setPurgeableState:MTLPurgeableStateVolatile], MTLPurgeableStateNonVolatile);
        gRawRelease.release();
        ASSERT_TRUE(gRM->raw->arenaPurged(index));
        ASSERT_TRUE(kernel_model::model.reclaimParked(initialHandle));

        id<MTLBuffer> src = [device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
        ASSERT_NE(src, nil);
        std::vector<uint8_t> expected(65536);
        for (size_t i = 0; i < expected.size(); ++i)
            expected[i] = uint8_t((i * 29) ^ (i >> 8));
        memcpy(src.contents, expected.data(), expected.size());

        id<MTLCommandBuffer> cb = [[device newCommandQueue] commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
        [blit copyFromBuffer:src sourceOffset:0 toBuffer:buf destinationOffset:0 size:expected.size()];
        [blit endEncoding];
        const size_t firstJob = kernel_model::model.jobs.size();
        [cb commit];
        [cb waitUntilCompleted];
        ASSERT_EQ(cb.status, MTLCommandBufferStatusCompleted) << cb.error.description.UTF8String;
        runBlitJobs(firstJob);

        EXPECT_FALSE(gRM->raw->arenaPurged(index));
        EXPECT_EQ(buf.contents, stablePtr);
        EXPECT_GE(kernel_model::model.unparkReclaimed, 1u);
        EXPECT_GE(kernel_model::model.fixedMaps, 1u);
        EXPECT_NE(gm20b::DeviceHarness::arena(*gRM->raw, index).handle, initialHandle);
        EXPECT_EQ(kernel_model::model.bos.count(initialHandle), 0u);
        EXPECT_EQ(memcmp(buf.contents, expected.data(), expected.size()), 0);
        EXPECT_EQ(kernel_model::model.imports.size(), importsBefore);
        EXPECT_EQ(kernel_model::model.badUnmaps, 0u);

        if (NSDictionary *mem = harness::ask(device, "switchMetalMemory"))
            EXPECT_EQ([mem[@"host_zeroed"] unsignedLongLongValue], 0u);
    }
}

TEST(FacadeParkedHeap, IdleClientBufferChunkParksOnGiveBackAndUnparksResident)
{
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    @autoreleasepool {
        id<MTLDevice> device = facade::device();
        ASSERT_NE(device, nil);
        const unsigned parksBefore = kernel_model::model.parkCalls;
        const unsigned unparksBefore = kernel_model::model.unparkResident;
        @autoreleasepool {
            id<MTLBuffer> tmp = [device newBufferWithLength:65536 options:MTLResourceStorageModePrivate];
            ASSERT_NE(tmp, nil);
        }
        gRawRelease.release();
        EXPECT_GT(kernel_model::model.parkCalls, parksBefore);
        @autoreleasepool {
            id<MTLBuffer> again = [device newBufferWithLength:65536 options:MTLResourceStorageModePrivate];
            ASSERT_NE(again, nil);
        }
        EXPECT_GT(kernel_model::model.unparkResident, unparksBefore);
        EXPECT_EQ(kernel_model::model.badUnmaps, 0u);
    }
}

TEST(FacadeParkedHeap, FailedNonVolatileRestoreRollsBackPurgeableState)
{
    kernel_model::model.infoFlags |= TegraGM20BInfoBoPark;
    @autoreleasepool {
        id<MTLDevice> device = facade::device();
        ASSERT_NE(device, nil);

        id<MTLHeap> h = facade::heap(MTLStorageModeShared, 1u << 20);
        ASSERT_NE(h, nil);
        const uint32_t index = ((SwitchMetalHeap *)h)->_arena - 1;
        id<MTLBuffer> buf = [h newBufferWithLength:65536 options:MTLResourceStorageModeShared];
        ASSERT_NE(buf, nil);
        memset(buf.contents, 0x22, 65536);

        EXPECT_EQ([h setPurgeableState:MTLPurgeableStateVolatile], MTLPurgeableStateNonVolatile);
        gRawRelease.release();
        ASSERT_TRUE(gRM->raw->arenaPurged(index));

        kernel_model::model.forceUnparkBusy = true;
        EXPECT_EQ([h setPurgeableState:MTLPurgeableStateNonVolatile], MTLPurgeableStateEmpty);
        EXPECT_TRUE(gRM->raw->arenaPurged(index));
        EXPECT_EQ([h setPurgeableState:MTLPurgeableStateKeepCurrent], MTLPurgeableStateEmpty);
        kernel_model::model.forceUnparkBusy = false;

        EXPECT_EQ([h setPurgeableState:MTLPurgeableStateNonVolatile], MTLPurgeableStateEmpty);
        EXPECT_FALSE(gRM->raw->arenaPurged(index));
        EXPECT_EQ([h setPurgeableState:MTLPurgeableStateKeepCurrent], MTLPurgeableStateNonVolatile);
        EXPECT_EQ(kernel_model::model.badUnmaps, 0u);
    }
}

}  // namespace
