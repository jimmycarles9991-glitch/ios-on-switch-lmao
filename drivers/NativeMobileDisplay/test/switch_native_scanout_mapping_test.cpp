// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <vector>
#define IONewZero(type, count) new (std::nothrow) type[count]()

class OSObject {
public:
    unsigned refs = 1;

    void release()
    {
        --refs;
    }
};

class IOMemoryDescriptor : public OSObject {
public:
    unsigned completes = 0;

    void complete(int)
    {
        ++completes;
    }
};

constexpr int kIODirectionOut = 2;
#define OSSafeReleaseNULL(object) \
    do {                          \
        if (object)               \
            (object)->release();  \
        (object) = nullptr;       \
    } while (0)
unsigned unholds;

extern "C" void tegra_gm20b_surface_unhold(void *)
{
    ++unholds;
}

#include "../SwitchPlaneSlot.h"
#include "../SwitchNativePlane.h"
#include "../SwitchDisplayComposition.h"
#include "../../TegraVIC/SwitchNativeScanout.h"
#include "../../TegraDisplayFlip/TegraScanoutCopy.h"

namespace {
SwitchNativeScanout nextNative, nextPitch;
unsigned leases, acquisitions, pitchAcquisitions, deliveries;
int nativeError, pitchError;
uint64_t completedThrough = 0;  // tokens with a value up to this have completed

struct Mapping {
    unsigned maps = 0, unmaps = 0, count = 0;
    uint64_t first = 0;
    bool fail = false;

    bool map(uint64_t, const uint64_t *pages, uint32_t n, bool write)
    {
        EXPECT_FALSE(write);
        ++maps;
        count = n;
        first = pages[0];
        return !fail;
    }

    void unmap(uint64_t, uint32_t)
    {
        ++unmaps;
    }
};

template <unsigned Slots> struct Display {
    static constexpr unsigned kPlaneSlots = Slots;
    static constexpr unsigned kPlaneOverlayDependencies = 3, kPlaneDependencies = 6;
    static constexpr uint64_t kPlaneSlotSpan = 20ULL << 20;
    using PlaneSlot = switch_display::PlaneSlot;
    PlaneSlot planeSlots[kPlaneSlots] = {};
    using PlaneSet = switch_display::Composition;
    PlaneSet planeNext;
    bool planeNextSet = false, stats = true, windowIdle = false;
    uint32_t planeSlotUses = 0, stPlaneMaps = 0, stPlaneNative = 0, flushes = 0;
    bool busy[kPlaneSlots] = {};
    Mapping probeDomain;

    bool planeSlotBusy(int slot) const
    {
        return busy[slot];
    }

    bool planeWindowIdle() const
    {
        return windowIdle;
    }

    static const tgm_layout *surfaceLayout(OSObject *, tgm_layout &)
    {
        return nullptr;
    }

    static uint64_t planeSlotIova(int slot)
    {
        return 0x10000000ULL + uint64_t(slot) * kPlaneSlotSpan;
    }

    void cleanDomain()
    {
        ++flushes;
    }

    int planeUseNoted(int slot, OSObject *)
    {
        return slot;
    }

    void planeUseRelease(unsigned)
    {}

    void planeUseHoldScanned()
    {}

    ~Display()
    {
        for (unsigned i = 0; i < kPlaneSlots; ++i) {
            planeDropSlot(i);
            delete[] planeSlots[i].native;
        }
    }

#include "../SwitchPlaneSlots.h"
#include "../SwitchNativePlaneDisplay.h"
};
}

int switch_native_scanout_acquire(OSObject *, SwitchNativeScanout *out, bool)
{
    ++acquisitions;
    *out = nativeError ? SwitchNativeScanout{} : nextNative;
    if (out->lease)
        ++leases;
    return nativeError;
}

int switch_native_scanout_pitch(OSObject *, SwitchNativeScanout *out)
{
    ++pitchAcquisitions;
    *out = pitchError ? SwitchNativeScanout{} : nextPitch;
    if (out->lease)
        ++leases;
    return pitchError;
}

int switch_gpu_backing_token_external_status(const switch_gpu_backing_token *token)
{
    return token->value <= completedThrough ? 1 : 0;
}

std::vector<switch_gpu_backing_token> promised;  // the overlay surface's pending writer promises

int switch_gpu_backing_promise_tokens(switch_gpu_backing *, uint32_t, uint32_t, switch_gpu_backing_token *tokens,
                                      uint32_t capacity)
{
    if (promised.size() > capacity)
        return -28;
    for (size_t i = 0; i < promised.size(); ++i)
        tokens[i] = promised[i];
    return int(promised.size());
}

void switch_native_scanout_deliver(SwitchNativeScanout *value)
{
    if (value->lease)
        ++deliveries;
}

void switch_native_scanout_release(SwitchNativeScanout *value)
{
    if (value->lease) {
        EXPECT_GT(leases, 0u);
        --leases;
    }
    *value = {};
}

namespace {
class NativeScanoutMapping : public testing::Test {
protected:
    native_planes::Layer layer;
    native_planes::Image image;
    std::array<uint64_t, 1024> pages;
    Display<2> *display;
    bool resolved;

    void SetUp() override
    {
        leases = acquisitions = pitchAcquisitions = deliveries = 0;
        completedThrough = 0;
        nativeError = pitchError = 0;
        for (uint32_t i = 0; i < pages.size(); ++i)
            pages[i] = 0x80000000ULL + uint64_t(i) * 4096;
        layer = {};
        layer.kind = native_planes::kKindNv12Video;
        layer.surface.format = native_planes::fourcc('4', '2', '0', 'v');
        layer.surface.width = 1280;
        layer.surface.height = 720;
        layer.surface.planes = 2;
        layer.surface.stride[0] = layer.surface.stride[1] = 1280;
        layer.surface.planeHeight[0] = 720;
        layer.surface.planeHeight[1] = 360;
        layer.surface.offset[1] = 1280 * 720;
        layer.src = {0, 0, 1280, 720};
        layer.dst = {0, 437, 720, 405};
        nextNative = {};
        nextNative.lease = reinterpret_cast<switch_gpu_cpu_lease *>(1);
        nextNative.view.native = reinterpret_cast<switch_gpu_backing *>(1);
        nextNative.view.producer = {73, 1, SWITCH_GPU_ENGINE_NVDEC, 163};
        auto &layout = nextNative.view.layout;
        layout.format = layer.surface.format;
        layout.plane_count = 2;
        layout.color_matrix = 2;
        layout.color_range = 1;
        layout.planes[0] = {0, 1536 * 736, 1536, 736, 1536, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
        layout.planes[1] = {1536 * 736, 1536 * 368, 768, 368, 1536, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
        nextNative.allocation.allocation_id = nextNative.allocation.allocation_generation =
            nextNative.allocation.content_generation = 1;
        nextNative.allocation.pages = pages.data();
        nextNative.allocation.page_count = uint32_t(pages.size());
        nextPitch = nextNative;
        nextPitch.allocation.allocation_id = 2;
        nextPitch.view.producer = {92, 1, SWITCH_GPU_ENGINE_VIC, 164};
        display = new Display<2>;
        resolved = false;
    }

    void TearDown() override
    {
        delete display;
        EXPECT_EQ(leases, 0u);
    }

    int admit()
    {
        return display->planeNativeSlotFor(nullptr, layer, 720, 1280, image, resolved);
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

TEST_F(NativeScanoutMapping, UnsupportedCropUsesQueuedPitchAllocationAndAliasOffset)
{
    nextNative.view.crop.x = 2;
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
    display->planeSlots[0].native->view.native = reinterpret_cast<switch_gpu_backing *>(0x1000);
    promised = {{100, 0, SWITCH_GPU_ENGINE_PROMISE, 2048}};
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
    promised.clear();
    display->planeSlots[0].native->view.native = nullptr;
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
