// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "switch_native_scanout_harness.h"

namespace {
class NativeScanoutMapping : public NativeScanoutFixture {
protected:
    Display<2> *display;

    void SetUp() override
    {
        NativeScanoutFixture::SetUp();
        display = new Display<2>;
    }

    void TearDown() override
    {
        delete display;
        EXPECT_EQ(leases, 0u);
        for (const auto &entry : backingRefs)
            EXPECT_EQ(entry.second, 0);
        for (const auto &entry : backingScanoutHolds)
            EXPECT_EQ(entry.second, 0);
    }

    int admit()
    {
        return NativeScanoutFixture::admit(*display);
    }
};

TEST_F(NativeScanoutMapping, NativeAllocationMapsOnceAndCarriesExactProducer)
{
    ASSERT_EQ(admit(), 0);
    EXPECT_TRUE(resolved);
    EXPECT_EQ(display->probeDomain.maps, 1u);
    EXPECT_EQ(display->probeDomain.first, pages[0]);
    EXPECT_EQ(leases, 1u);
    display->busy[0] = true;
    ASSERT_EQ(admit(), 0);
    EXPECT_EQ(display->probeDomain.maps, 1u);
    EXPECT_EQ(leases, 1u);
    display->planeNextSet = true;
    display->planeNext.window[0].on = true;
    display->planeNext.window[0].slot = 0;
    TegraScanoutCopy copy = {};
    switch_gpu_backing_token tokens[6] = {};
    display->planeNextDependencies(copy, tokens);
    ASSERT_EQ(copy.dependencyCount, 1u);
    ASSERT_NE(copy.dependencies, nullptr);
    EXPECT_EQ(copy.dependencies[0].value, 73u);
    EXPECT_EQ(copy.dependencies[0].engine, uint32_t(SWITCH_GPU_ENGINE_NVDEC));
}

TEST_F(NativeScanoutMapping, ContentGenerationChangeCannotReuseLiveImageMapping)
{
    ASSERT_EQ(admit(), 0);
    display->busy[0] = true;
    ++nextNative.allocation.content_generation;
    ASSERT_EQ(admit(), 1);
    EXPECT_EQ(display->probeDomain.maps, 2u);
    EXPECT_EQ(leases, 2u);
    display->busy[1] = true;
    ++nextNative.allocation.content_generation;
    EXPECT_EQ(admit(), -11);
    EXPECT_EQ(leases, 2u);
    display->busy[0] = false;
    display->planeDropSlot(0);
    EXPECT_EQ(leases, 1u);
}

// A decoder pool cycling through three buffers: each is mapped once, and at every latch only the frame window B shows
// keeps its lease, so the producer can write the others again.
TEST_F(NativeScanoutMapping, SteadyPlaybackMapsEachPoolBufferOnce)
{
    Display<4> pool;
    for (uint64_t frame = 0; frame < 30; ++frame) {
        nextNative.allocation.allocation_id = 10 + frame % 3;
        nextNative.allocation.content_generation = frame + 1;
        const int slot = pool.planeNativeSlotFor(nullptr, layer, 720, 1280, image, resolved);
        ASSERT_GE(slot, 0);
        EXPECT_EQ(pool.planeSlots[slot].native->allocation.content_generation, frame + 1);
        for (bool &busy : pool.busy)
            busy = false;
        pool.busy[slot] = true;
        pool.planeReleaseIdle();
        EXPECT_EQ(leases, 1u);
    }
    EXPECT_EQ(pool.probeDomain.maps, 3u);
    EXPECT_EQ(pool.stPlaneMaps, 3u);
    EXPECT_EQ(pool.stPlaneNative, 30u);  // every frame scanned in the decoder's own block-linear layout
    EXPECT_EQ(pool.probeDomain.unmaps, 0u);
    EXPECT_EQ(pool.flushes, 3u);
}

TEST_F(NativeScanoutMapping, IdleWindowReturnsSurfacesAndKeepsEngineMappings)
{
    ASSERT_EQ(admit(), 0);
    IOMemoryDescriptor memory;
    memory.refs = 2;
    display->planeSlots[1].memory = &memory;
    display->planeSlots[1].low = 0;
    display->planeSlots[1].high = 4096;
    display->planeSlots[1].iova = uint32_t(display->planeSpace.alloc(4096));
    display->planeReleaseIdle();
    EXPECT_EQ(leases, 0u);
    EXPECT_EQ(memory.completes, 0u);
    display->windowIdle = true;
    display->planeReleaseIdle();
    EXPECT_EQ(memory.completes, 1u);
    EXPECT_EQ(memory.refs, 1u);
    EXPECT_EQ(display->probeDomain.unmaps, 1u);
    EXPECT_EQ(display->planeSlots[0].mappedAllocation, 1u);
    ++nextNative.allocation.content_generation;
    EXPECT_EQ(admit(), 0);
    EXPECT_EQ(display->probeDomain.maps, 1u);
    EXPECT_EQ(leases, 1u);
}

TEST_F(NativeScanoutMapping, EvictedMappingIsUnmappedAndNewPagesAreMappedAgain)
{
    for (uint64_t allocation : {10, 11, 12, 10}) {
        nextNative.allocation.allocation_id = allocation;
        ASSERT_GE(admit(), 0);
        display->planeReleaseIdle();
    }
    EXPECT_EQ(display->probeDomain.maps, 4u);
    EXPECT_EQ(display->probeDomain.unmaps, 2u);
    EXPECT_EQ(leases, 0u);
    ++nextNative.allocation.allocation_generation;
    ASSERT_GE(admit(), 0);
    EXPECT_EQ(display->probeDomain.maps, 5u);
}

// A decoder crop scans the decoder's own block-linear picture through window B's offsets: no pitch representation, no
// VIC copy for the frame.
TEST_F(NativeScanoutMapping, ACropIsScannedInTheDecodersPictureWithNoPitchRepresentation)
{
    nextNative.view.crop.x = 2;
    nextNative.view.crop.y = 4;
    ASSERT_EQ(admit(), 0);
    EXPECT_EQ(pitchAcquisitions, 0u);
    EXPECT_EQ(leases, 1u);
    EXPECT_EQ(image.surfaceKind, 0x12u);
    EXPECT_EQ(image.hOffset, 2u);
    EXPECT_EQ(image.vOffset, 4u);
    EXPECT_EQ(display->stPlaneNative, 1u);
    EXPECT_EQ(display->planeSlots[0].native->view.producer.engine, uint32_t(SWITCH_GPU_ENGINE_NVDEC));
}

TEST_F(NativeScanoutMapping, ACropPastThePlanesUsesQueuedPitchAllocationAndAliasOffset)
{
    nextNative.view.crop.x = 1536 - 1280 + 2;
    nextPitch.pitchOffset = 4096;
    ASSERT_EQ(admit(), 0);
    EXPECT_EQ(pitchAcquisitions, 1u);
    EXPECT_EQ(leases, 1u);
    EXPECT_EQ(image.surfaceKind, 0u);
    EXPECT_EQ(image.start[0], 4096u);
    EXPECT_EQ(display->probeDomain.first, pages[1]);
    EXPECT_EQ(display->planeSlots[0].native->view.producer.engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
}

TEST_F(NativeScanoutMapping, FailedMappingUnwindsRetainedImageAndPartialPageTable)
{
    display->probeDomain.fail = true;
    EXPECT_EQ(admit(), -1);
    EXPECT_TRUE(resolved);
    EXPECT_EQ(leases, 0u);
    EXPECT_EQ(display->probeDomain.unmaps, 1u);
}

TEST_F(NativeScanoutMapping, OrdinarySurfaceCapturesExactBackingAndContentGeneration)
{
    nativeError = -2;
    ASSERT_EQ(admit(), 0);
    EXPECT_TRUE(resolved);
    EXPECT_EQ(pitchAcquisitions, 1u);
    EXPECT_EQ(display->probeDomain.maps, 1u);
    EXPECT_EQ(display->planeSlots[0].native->allocation.allocation_id, 2u);
    EXPECT_EQ(display->planeSlots[0].native->view.producer.value, 92u);
}

TEST_F(NativeScanoutMapping, BusyNativeSurfaceIsRetriedNotReadThroughItsPitchRepresentation)
{
    nativeError = -16;
    EXPECT_EQ(admit(), -16);
    EXPECT_EQ(pitchAcquisitions, 0u);
    EXPECT_EQ(display->probeDomain.maps, 0u);
    EXPECT_EQ(leases, 0u);
    nativeError = -22;
    EXPECT_EQ(admit(), -22);
    EXPECT_EQ(pitchAcquisitions, 0u);
    nativeError = int(SWITCH_GPU_BACKING_STALE);
    EXPECT_EQ(admit(), 0);
    EXPECT_EQ(pitchAcquisitions, 1u);
}

TEST_F(NativeScanoutMapping, IncompletePhysicalExtentCannotReachSmmu)
{
    nextNative.allocation.page_count = 1;
    EXPECT_EQ(admit(), -95);
    EXPECT_EQ(leases, 0u);
    EXPECT_EQ(display->probeDomain.maps, 0u);
}

TEST_F(NativeScanoutMapping, CpuOwnedOrdinarySurfaceDefersWithoutMappingOrDroppingItsProducer)
{
    nativeError = -2;
    pitchError = -16;
    EXPECT_EQ(admit(), -16);
    EXPECT_TRUE(resolved);
    EXPECT_EQ(display->probeDomain.maps, 0u);
    EXPECT_EQ(leases, 0u);
}
}  // namespace

// A recorded turn's request waits come with its picture: the set waits for them as for the picture's producer.
TEST_F(NativeScanoutMapping, ARecordedTurnsRequestWaitsOrderTheSetLikeItsProducer)
{
    const switch_gpu_backing_token waits[2] = {{300, 2, SWITCH_GPU_ENGINE_GM20B, 81},
                                               {301, 3, SWITCH_GPU_ENGINE_VIC, 82}};
    nextNative.view.waits = waits;
    nextNative.view.waitCount = 2;
    ASSERT_EQ(admit(), 0);
    display->planeNextSet = true;
    display->planeNext.window[0].on = true;
    display->planeNext.window[0].slot = 0;
    TegraScanoutCopy copy = {};
    switch_gpu_backing_token tokens[6] = {};
    display->planeNextDependencies(copy, tokens);
    ASSERT_EQ(copy.dependencyCount, 3u);
    EXPECT_EQ(copy.dependencies[0].value, 73u);
    EXPECT_EQ(copy.dependencies[1].value, 300u);
    EXPECT_EQ(copy.dependencies[2].value, 301u);
    completedThrough = 300;
    EXPECT_TRUE(display->planeOverlayWaits(*display->planeSlots[0].native));
    completedThrough = 301;
    EXPECT_FALSE(display->planeOverlayWaits(*display->planeSlots[0].native));
}

// A GPU-rendered overlay whose command buffer was held on a shared event: the set's fence acquires its writer promise.
TEST_F(NativeScanoutMapping, AnOverlaysWriterPromiseIsOneOfItsDependencies)
{
    ASSERT_EQ(admit(), 0);
    display->planeSlots[0].native->promises[0] = {100, 0, SWITCH_GPU_ENGINE_PROMISE, 2048};
    display->planeSlots[0].native->promiseCount = 1;
    display->planeNextSet = true;
    display->planeNext.window[0].on = true;
    display->planeNext.window[0].slot = 0;
    TegraScanoutCopy copy = {};
    switch_gpu_backing_token tokens[6] = {};
    display->planeNextDependencies(copy, tokens);
    ASSERT_EQ(copy.dependencyCount, 2u);
    EXPECT_EQ(copy.dependencies[1].engine, uint32_t(SWITCH_GPU_ENGINE_PROMISE));
    completedThrough = 73;
    EXPECT_TRUE(display->planeOverlayWaits(*display->planeSlots[0].native));
    completedThrough = 100;
    EXPECT_FALSE(display->planeOverlayWaits(*display->planeSlots[0].native));
    display->planeSlots[0].native->promiseCount = 0;
}

// The signals go when the set showing the frame latches, and a frame dropped before it was shown still delivers them
// when its slot goes back.
TEST_F(NativeScanoutMapping, AShownFrameDeliversAtItsLatchAndADroppedOneWhenItsSlotGoesBack)
{
    ASSERT_EQ(admit(), 0);
    Display<2>::PlaneSet shown = {};
    shown.window[0].on = true;
    shown.window[0].slot = 0;
    display->planeDeliver(shown);
    EXPECT_EQ(deliveries, 1u);
    ++nextNative.allocation.allocation_id;
    display->busy[0] = true;
    ASSERT_EQ(admit(), 1);
    display->busy[0] = false;
    display->planeReleaseLease(1);
    EXPECT_EQ(deliveries, 2u);
    EXPECT_EQ(leases, 1u);
}

TEST_F(NativeScanoutMapping, PlaneSlotForHoldsScanoutBeforeCompressionCheckAndUnwindsCleanly)
{
    auto *backing = reinterpret_cast<switch_gpu_backing *>(0x2000);
    IOMemoryDescriptor memory;
    memory.length = 64 * 1024;
    findPaBackings[memory.pa] = backing;

    unsigned checks = 0;
    int holdsDuringCheck = -1;
    onSurfaceCompressedCheck = [&](uint64_t pa) {
        EXPECT_EQ(pa, memory.pa);
        ++checks;
        holdsDuringCheck = backingScanoutHolds[backing];
    };

    surfaceCompressedFlag = true;
    EXPECT_EQ(display->planeSlotFor(&memory, 0, memory.length, false), -1);
    EXPECT_EQ(checks, 1u);
    EXPECT_EQ(holdsDuringCheck, 1);
    EXPECT_EQ(backingScanoutHolds[backing], 0);
    EXPECT_EQ(backingRefs[backing], 0);
    EXPECT_EQ(memory.prepares, memory.completes);
    EXPECT_EQ(memory.refs, 1u);
    EXPECT_EQ(display->probeDomain.unmaps, 1u);

    surfaceCompressedFlag = false;
    holdsDuringCheck = -1;
    const int slot = display->planeSlotFor(&memory, 0, memory.length, false);
    ASSERT_GE(slot, 0);
    EXPECT_EQ(checks, 2u);
    EXPECT_EQ(holdsDuringCheck, 1);
    EXPECT_EQ(backingScanoutHolds[backing], 1);
    EXPECT_EQ(backingRefs[backing], 1);

    display->planeDropSlot(unsigned(slot));
    EXPECT_EQ(backingScanoutHolds[backing], 0);
    EXPECT_EQ(backingRefs[backing], 0);

    // Backing registered after initial map: cache hit acquires scanout hold before compression check.
    findPaBackings.clear();
    onSurfaceCompressedCheck = {};
    const int lateSlot = display->planeSlotFor(&memory, 0, memory.length, false);
    ASSERT_GE(lateSlot, 0);
    EXPECT_EQ(backingScanoutHolds[backing], 0);

    findPaBackings[memory.pa] = backing;
    surfaceCompressedFlag = true;
    holdsDuringCheck = -1;
    onSurfaceCompressedCheck = [&](uint64_t) {
        holdsDuringCheck = backingScanoutHolds[backing];
    };
    EXPECT_EQ(display->planeSlotFor(&memory, 0, memory.length, false), -1);
    EXPECT_EQ(holdsDuringCheck, 1);
    EXPECT_EQ(backingScanoutHolds[backing], 0);
    EXPECT_EQ(backingRefs[backing], 0);
}

TEST_F(NativeScanoutMapping, PlaneNativeSlotForHoldsScanoutBeforeCompressionCheckAndReleasesOnLeaseAndDrop)
{
    auto *backing = nextNative.view.native;
    unsigned checks = 0;
    int holdsDuringCheck = -1;
    onSurfaceCompressedCheck = [&](uint64_t pa) {
        EXPECT_EQ(pa, pages[0]);
        ++checks;
        holdsDuringCheck = backingScanoutHolds[backing];
    };

    surfaceCompressedFlag = true;
    EXPECT_EQ(admit(), -95);
    EXPECT_EQ(checks, 1u);
    EXPECT_EQ(holdsDuringCheck, 1);
    EXPECT_EQ(backingScanoutHolds[backing], 0);
    EXPECT_EQ(backingRefs[backing], 0);
    EXPECT_EQ(leases, 0u);
    EXPECT_EQ(display->probeDomain.maps, 0u);

    surfaceCompressedFlag = false;
    holdsDuringCheck = -1;
    ASSERT_EQ(admit(), 0);
    EXPECT_EQ(checks, 2u);
    EXPECT_EQ(holdsDuringCheck, 1);
    EXPECT_EQ(backingScanoutHolds[backing], 1);
    EXPECT_EQ(backingRefs[backing], 1);

    // Active same-generation hit keeps the existing slot hold without leaking a duplicate hold.
    ASSERT_EQ(admit(), 0);
    EXPECT_EQ(backingScanoutHolds[backing], 1);
    EXPECT_EQ(backingRefs[backing], 1);

    display->planeReleaseLease(0);
    EXPECT_EQ(backingScanoutHolds[backing], 0);
    EXPECT_EQ(backingRefs[backing], 0);
    EXPECT_EQ(leases, 0u);

    // Surface compressed while idle: slot reuse acquires scanout hold before check and rejects.
    ++nextNative.allocation.content_generation;
    surfaceCompressedFlag = true;
    holdsDuringCheck = -1;
    EXPECT_EQ(admit(), -95);
    EXPECT_EQ(holdsDuringCheck, 1);
    EXPECT_EQ(backingScanoutHolds[backing], 0);
    EXPECT_EQ(backingRefs[backing], 0);
    EXPECT_EQ(leases, 0u);

    // Uncompressed reuse takes back the scanout hold and releases cleanly in planeDropSlot.
    surfaceCompressedFlag = false;
    holdsDuringCheck = -1;
    ASSERT_EQ(admit(), 0);
    EXPECT_EQ(holdsDuringCheck, 1);
    EXPECT_EQ(backingScanoutHolds[backing], 1);
    EXPECT_EQ(backingRefs[backing], 1);
    EXPECT_EQ(display->probeDomain.maps, 1u);

    display->planeDropSlot(0);
    EXPECT_EQ(backingScanoutHolds[backing], 0);
    EXPECT_EQ(backingRefs[backing], 0);
}

TEST_F(NativeScanoutMapping, PlaneSlotForCleanUsesPhysicalPagesAndContiguousSegmentsWithoutVaMap)
{
    IOMemoryDescriptor memory;
    memory.length = 16 * 4096;
    memory.segmentStep = 16 * 4096;

    const int slot = display->planeSlotFor(&memory, 0, memory.length, true);
    ASSERT_GE(slot, 0);
    EXPECT_EQ(mapCalls, 0u);
    EXPECT_EQ(memory.segmentCalls, 1u);
    EXPECT_EQ(hwPageCleanCalls, 1u);
    EXPECT_EQ(hwPageCleanPages, 16u);
    EXPECT_EQ(dcacheSyncCalls, 1u);

    EXPECT_EQ(display->planeSlotFor(&memory, 0, memory.length, true), slot);
    EXPECT_EQ(memory.segmentCalls, 1u);
    EXPECT_EQ(mapCalls, 0u);
    display->planeDropSlot(unsigned(slot));
}
