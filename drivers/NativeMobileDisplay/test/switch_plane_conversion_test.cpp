// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cstdlib>
#include <functional>
#include "../NativePlanes.h"
#include "../../TegraVIC/SwitchNativeScanout.h"
#include "../../TegraVIC/SwitchNativeVICImage.h"
#include "../../TegraVIC/SwitchVICScanoutConvert.h"
#include "../SwitchScanoutBuffer.h"
#define IONewZero(type, count)         static_cast<type *>(std::calloc(count, sizeof(type)))
#define IODelete(pointer, type, count) std::free(pointer)

struct switch_gpu_backing {
    uint64_t id, content, bytes;
    unsigned refs;
};

struct switch_gpu_cpu_lease {};

struct OriginalLegacySwapRequest {};

class OSObject {};

using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnNotReady = -10, kIOReturnAborted = -14;
constexpr int TEGRA_SCANOUT_SOURCE_LOST = -5;

namespace {
unsigned allocations, conversions, leases, backingCount, observations;
bool offGate;
std::function<void()> duringAdmission;

uint64_t mach_absolute_time()
{
    return 100;
}

void native_legacy_transfer_observed(int, const OriginalLegacySwapRequest *, uint64_t)
{
    ++observations;
}

int convertError, exportError;
TegraVICImage lastInput;
TegraVICOp lastOperation;
switch_gpu_cpu_lease lease;
uint64_t pages[4096];
}

switch_gpu_backing *switch_display_scanout_buffer(size_t bytes)
{
    EXPECT_TRUE(offGate);
    ++allocations;
    ++backingCount;
    return new switch_gpu_backing{allocations + 10, 0, bytes, 1};
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
    ++backing->refs;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
    if (!--backing->refs) {
        --backingCount;
        delete backing;
    }
}

extern "C" void switch_gpu_backing_describe(switch_gpu_backing *backing, switch_gpu_backing_info *info)
{
    *info = {};
    info->allocation_id = backing->id;
    info->allocation_generation = 1;
    info->content_generation = backing->content;
    info->pages = pages;
    info->page_count = uint32_t((backing->bytes + 4095) >> 12);
}

extern "C" int switch_gpu_backing_export_read(switch_gpu_backing *backing, switch_gpu_backing_token *producer,
                                              switch_gpu_cpu_lease **out)
{
    if (exportError)
        return exportError;
    *out = &lease;
    *producer = {backing->content + (uint64_t(2) << 32), 1, SWITCH_GPU_ENGINE_VIC, 11};
    ++leases;
    return 0;
}

void switch_native_scanout_release(SwitchNativeScanout *scanout)
{
    if (scanout->lease)
        --leases;
    if (scanout->view.native)
        switch_gpu_backing_release(scanout->view.native);
    *scanout = {};
}

int switch_vic_scanout_convert(const SwitchNativeScanout *source, const TegraVICImage *input,
                               switch_gpu_backing *target, uint32_t width, uint32_t height, uint32_t pitch,
                               const TegraVICOp *operation, SwitchNativeScanout *output)
{
    EXPECT_TRUE(offGate);
    if (duringAdmission)
        duringAdmission();
    EXPECT_NE(source->lease, nullptr);
    EXPECT_EQ(width, operation->dstW);
    EXPECT_EQ(height, operation->dstH);
    EXPECT_GE(pitch, width * 4);
    ++conversions;
    lastInput = *input;
    lastOperation = *operation;
    if (convertError)
        return convertError;
    ++target->content;
    *output = {};
    const int error = switch_gpu_backing_export_read(target, &output->view.producer, &output->lease);
    if (!error) {
        switch_gpu_backing_retain(target);
        output->view.native = target;
        switch_gpu_backing_describe(target, &output->allocation);
    }
    return error;
}

namespace {
struct Event {
    unsigned fires = 0;

    void interruptOccurred(void *, void *, int)
    {
        ++fires;
    }
};

struct Loop {
    int runAction(int (*action)(OSObject *, void *, void *, void *, void *), OSObject *object)
    {
        offGate = false;
        return action(object, nullptr, nullptr, nullptr, nullptr);
    }
};

struct NativeLegacyDisplay : OSObject {
    bool transferred = false, copyStopping = false, latchRetire = true, planeNextSet = true;
    unsigned kicks = 0, lost = 0, taken = 0;
    static constexpr int kNoBuffer = -1;
    Event event;
    Event *copySource = &event;
    Loop loop;
    const OriginalLegacySwapRequest *request = nullptr;

    Loop *getWorkLoop()
    {
        return &loop;
    }

    const OriginalLegacySwapRequest *firstUntaken()
    {
        return request;
    }

    const OriginalLegacySwapRequest *pendingSwapRequest()
    {
        return request;
    }

    void kickSwapQueue()
    {
        ++kicks;
    }

    void noteTaken(const OriginalLegacySwapRequest *, uint64_t)
    {
        ++taken;
    }

    void dropLostFrame(const OriginalLegacySwapRequest *, int)
    {
        ++lost;
    }

    static constexpr unsigned kPlaneSlots = 8;
    static constexpr uint64_t kPlaneSlotSpan = 20ULL << 20;

    struct Slot {
        SwitchNativeScanout *native = nullptr;
    } planeSlots[kPlaneSlots];

    SwitchNativeScanout snapshots[kPlaneSlots];
    bool busy[kPlaneSlots] = {};
    bool failMap = false;
    unsigned mappings = 0;

    NativeLegacyDisplay()
    {
        for (unsigned i = 0; i < kPlaneSlots; ++i)
            planeSlots[i].native = &snapshots[i];
    }

    bool planeSlotBusy(int slot) const
    {
        return busy[slot] || planeConversionUses(slot);
    }

    void planeDropSlot(unsigned slot)
    {
        switch_native_scanout_release(&snapshots[slot]);
    }

    void planeReleaseLease(unsigned slot)
    {
        switch_native_scanout_release(&snapshots[slot]);
    }

    int planeMapSnapshot(SwitchNativeScanout &snapshot, const native_planes::Image &, int avoid)
    {
        for (unsigned slot = 0; slot < kPlaneSlots; ++slot) {
            if (int(slot) != avoid && snapshots[slot].lease && snapshots[slot].view.native == snapshot.view.native &&
                snapshots[slot].allocation.content_generation == snapshot.allocation.content_generation) {
                switch_native_scanout_release(&snapshot);
                return int(slot);
            }
        }
        if (!failMap) {
            for (unsigned slot = 0; slot < kPlaneSlots; ++slot) {
                if (int(slot) == avoid || busy[slot])
                    continue;
                planeDropSlot(slot);
                snapshots[slot] = snapshot;
                snapshot = {};
                ++mappings;
                return int(slot);
            }
        }
        switch_native_scanout_release(&snapshot);
        return -11;
    }

#include "../SwitchPlaneConversionDisplay.h"

    ~NativeLegacyDisplay()
    {
        stopPlaneConversion();
        for (unsigned i = 0; i < kPlaneSlots; ++i)
            planeDropSlot(i);
        convertedStop();
    }
};

using Display = NativeLegacyDisplay;

class PlaneConversion : public testing::Test {
protected:
    Display *display;
    OriginalLegacySwapRequest request;
    native_planes::Layer layer;
    native_planes::Image image;

    void SetUp() override
    {
        allocations = conversions = leases = backingCount = observations = 0;
        offGate = false;
        duringAdmission = {};
        convertError = exportError = 0;
        display = new Display;
        display->request = &request;
        auto &source = display->snapshots[0];
        source.view.native = new switch_gpu_backing{1, 3, 256 * 96, 1};
        ++backingCount;
        source.lease = &lease;
        ++leases;
        switch_gpu_backing_describe(source.view.native, &source.allocation);
        display->busy[0] = true;
        layer = {};
        layer.kind = native_planes::kKindNv12Video;
        layer.surface.format = native_planes::fourcc('4', '2', '0', 'v');
        layer.surface.width = 128;
        layer.surface.height = 64;
        layer.surface.planes = 2;
        layer.surface.colorMatrix = SWITCH_VIC_MATRIX_709;
        layer.surface.stride[0] = layer.surface.stride[1] = 256;
        layer.surface.planeHeight[0] = 64;
        layer.surface.planeHeight[1] = 32;
        layer.surface.offset[1] = 256 * 64;
        layer.surface.allocation = layer.surface.descriptorBytes = 256 * 96;
        layer.src = {0, 0, 128, 64};
        layer.dst = {100, 200, 256, 128};
    }

    void TearDown() override
    {
        delete display;
        EXPECT_EQ(leases, 0u);
        EXPECT_EQ(backingCount, 0u);
    }

    int offer()
    {
        return display->planeConvertSlot(0, layer, 720, 1280, image, &request);
    }

    void work()
    {
        offGate = true;
        display->planeConversionWork();
        EXPECT_FALSE(offGate);
    }

    int convert()
    {
        const int result = offer();
        if (result != -115)
            return result;
        work();
        return display->planeConversion->status ? display->planeConversion->status : offer();
    }

    void advance()
    {
        ++display->snapshots[0].allocation.content_generation;
    }
};

TEST_F(PlaneConversion, ConvertsOnceAndReusesTheSameContentWithoutAllocatingOrSubmitting)
{
    const int slot = convert();
    ASSERT_GE(slot, 0);
    display->busy[slot] = true;
    EXPECT_FALSE(image.yuv);
    EXPECT_EQ(image.position, (200u << 16) | 100u);
    EXPECT_EQ(image.size, (128u << 16) | 256u);
    EXPECT_EQ(lastInput.standard, TEGRA_VIC_YCC_709);
    EXPECT_EQ(lastOperation.srcW, 128u << 16);
    EXPECT_EQ(convert(), slot);
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(conversions, 1u);
    EXPECT_EQ(display->mappings, 1u);
    EXPECT_GT(display->snapshots[slot].view.producer.value, UINT32_MAX);
}

TEST_F(PlaneConversion, NewContentCannotOverwriteLiveConvertedScanout)
{
    const int first = convert();
    ASSERT_GE(first, 0);
    display->busy[first] = true;
    const auto *old = display->snapshots[first].view.native;
    advance();
    const int next = convert();
    ASSERT_GE(next, 0);
    EXPECT_NE(next, first);
    EXPECT_NE(display->snapshots[next].view.native, old);
    EXPECT_EQ(allocations, 2u);
    EXPECT_EQ(old->content, 1u);
}

TEST_F(PlaneConversion, RetiredBuffersAreReusedWithoutNewAllocation)
{
    const int first = convert();
    ASSERT_GE(first, 0);
    const auto *backing = display->snapshots[first].view.native;
    advance();
    const int next = convert();
    ASSERT_GE(next, 0);
    EXPECT_EQ(display->snapshots[next].view.native, backing);
    EXPECT_EQ(allocations, 1u);
    EXPECT_EQ(conversions, 2u);
}

TEST_F(PlaneConversion, PoolExhaustionPreservesEveryLiveImage)
{
    for (unsigned i = 0; i < 4; ++i) {
        const int slot = convert();
        ASSERT_GE(slot, 0);
        display->busy[slot] = true;
        advance();
    }
    EXPECT_EQ(convert(), -11);
    EXPECT_EQ(allocations, 4u);
    EXPECT_EQ(conversions, 4u);
}

TEST_F(PlaneConversion, AliasOffsetAndStrideArePartOfContentIdentity)
{
    ASSERT_GE(convert(), 0);
    display->snapshots[0].pitchOffset = 4096;
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(conversions, 2u);
    EXPECT_EQ(lastInput.base[0], 4096u);
    layer.surface.stride[0] = 512;
    layer.surface.descriptorBytes *= 2;
    ASSERT_GE(convert(), 0);
    EXPECT_EQ(conversions, 3u);
    EXPECT_EQ(lastInput.pitch[0], 512u);
}

TEST_F(PlaneConversion, FailedSubmissionDoesNotCreateCachedImage)
{
    convertError = -5;
    EXPECT_EQ(convert(), -5);
    EXPECT_EQ(display->mappings, 0u);
    convertError = 0;
    EXPECT_GE(convert(), 0);
    EXPECT_EQ(conversions, 2u);
    EXPECT_EQ(allocations, 1u);
}

TEST_F(PlaneConversion, AcceptedConversionSurvivesTemporaryDisplayMappingPressure)
{
    display->failMap = true;
    EXPECT_EQ(convert(), -11);
    display->failMap = false;
    EXPECT_GE(convert(), 0);
    EXPECT_EQ(conversions, 1u);
    EXPECT_EQ(allocations, 1u);
}

TEST_F(PlaneConversion, NewContentQueuesWithoutAllocationOrEngineAdmissionUnderTheDisplayGate)
{
    EXPECT_EQ(offer(), -115);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(conversions, 0u);
    EXPECT_TRUE(display->transferred);
    EXPECT_TRUE(display->planeConversionUses(0));
    EXPECT_EQ(display->event.fires, 1u);
    work();
    EXPECT_FALSE(display->transferred);
    EXPECT_FALSE(display->planeConversionUses(0));
    EXPECT_EQ(display->kicks, 1u);
    EXPECT_GE(offer(), 0);
    EXPECT_EQ(display->event.fires, 1u);
}

TEST_F(PlaneConversion, SourceCannotBeEvictedWhileRoomAdmissionRunsOutsideTheGate)
{
    display->busy[0] = false;
    ASSERT_EQ(offer(), -115);
    duringAdmission = [&] {
        EXPECT_TRUE(display->planeSlotBusy(0));
        EXPECT_TRUE(display->transferred);
        EXPECT_EQ(display->kicks, 0u);
        EXPECT_EQ(offer(), -11);
    };
    work();
    EXPECT_FALSE(display->planeSlotBusy(0));
}

TEST_F(PlaneConversion, StopBeforeDispatchReleasesTheQueuedReservationWithoutTouchingHardware)
{
    ASSERT_EQ(offer(), -115);
    display->copyStopping = true;
    work();
    EXPECT_EQ(conversions, 0u);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(display->kicks, 0u);
    EXPECT_FALSE(display->planeConversionQueued);
}

TEST_F(PlaneConversion, FailedProducerDoesNotSpinOrScanConvertedPixels)
{
    convertError = -5;
    ASSERT_EQ(offer(), -115);
    work();
    EXPECT_EQ(display->kicks, 0u);
    EXPECT_EQ(display->lost, 1u);
    EXPECT_EQ(display->taken, 1u);
    EXPECT_FALSE(display->planeNextSet);
    EXPECT_FALSE(display->convertedPlanes[0].valid);
}

TEST_F(PlaneConversion, UnavailableWorkerDoesNotAllocateOrAdmit)
{
    display->copySource = nullptr;
    EXPECT_EQ(offer(), -19);
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(conversions, 0u);
}
}  // namespace
