// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchPlaneConversionDisplay.h in a display stand-in, with VIC, the GPU backings and the copy thread modelled, and
// the PlaneConversion fixture: one copy for switch_plane_conversion_test and switch_plane_conversion_size_test.
#pragma once
#include <gtest/gtest.h>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <stdexcept>
#include "../NativePlanes.h"
#include "../../TegraDisplayFlip/TegraScanoutCopy.h"
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

struct OriginalLegacySwapRequest {
    uint32_t swap = 0;
};

class OSObject {};

using IOReturn = int;
constexpr int kIOReturnSuccess = 0, kIOReturnNotReady = -10, kIOReturnAborted = -14;

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

void IOLog(const char *, ...)
{}

[[noreturn]] void panic(const char *, ...)
{
    throw std::runtime_error("panic");
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

// The source slot's producer did not fail: a failure is the conversion's own.
extern "C" int switch_gpu_backing_tokens_failure(const switch_gpu_backing_token *, uint32_t)
{
    return 0;
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

// A pitch surface, as the GPU's layout query answers for anything it did not render block-linear.
int layoutAnswer;  // 1: the source's first page is a GPU render target in tiledLayout
tgm_layout tiledLayout;

extern "C" int tegra_gm20b_surface_layout(uint64_t, tgm_layout *layout)
{
    if (layoutAnswer == 1)
        *layout = tiledLayout;
    return layoutAnswer;
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
    unsigned kicks = 0, unshown = 0, taken = 0;
    static constexpr int kNoBuffer = -1;
    static constexpr size_t swapId = offsetof(OriginalLegacySwapRequest, swap);
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

    const OriginalLegacySwapRequest *pendingSwapRequest() const
    {
        return request;
    }

    static const OriginalLegacySwapRequest *queuedAfter(const OriginalLegacySwapRequest *)
    {
        return nullptr;
    }

    void kickSwapQueue()
    {
        ++kicks;
    }

    void noteTaken(const OriginalLegacySwapRequest *, uint64_t)
    {
        ++taken;
    }

    void retireSwapUnshown(const OriginalLegacySwapRequest *, int)
    {
        ++unshown;
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

#include "../SwitchUnshownSource.h"
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
}  // namespace
