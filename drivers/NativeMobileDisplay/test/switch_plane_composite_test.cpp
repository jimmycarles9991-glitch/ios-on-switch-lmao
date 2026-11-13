// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>
#include <array>
#include <cstdlib>
#include <stdexcept>
#include <vector>
#include "../SwitchDisplayMemoryEnvelope.h"
#include "../SwitchPlaneComposite.h"
#include "../../TegraVIC/SwitchNativeVICImage.h"
#include "../SwitchScanoutBuffer.h"
#include "../SwitchLandscapeWindows.h"
#define IONewZero(type, count)         static_cast<type *>(std::calloc(count, sizeof(type)))
#define IODelete(pointer, type, count) std::free(pointer)

struct switch_gpu_backing {
    uint64_t id, content, bytes;
    unsigned refs;
};

struct switch_gpu_cpu_lease {};

class OSObject {};

using thread_call_param_t = void *;
using thread_call_func_t = void (*)(thread_call_param_t, thread_call_param_t);

struct thread_call {
    thread_call_func_t func;
    thread_call_param_t param;
    bool pending;
};

using thread_call_t = thread_call *;
constexpr int THREAD_CALL_PRIORITY_KERNEL = 2, THREAD_CALL_OPTIONS_ONCE = 1;
constexpr int kIOReturnSuccess = 0, kIOReturnAborted = -14;
using SwitchMMIOWords = uint32_t *;

namespace {
uint64_t reserved, reserveLimit = UINT64_MAX;
unsigned composites, exports, kicks, retains;
bool vicReady = true;
int composeError;
std::vector<SwitchVICComposeSource> lastSources;
std::vector<uint64_t> lastIds;
uint64_t lastDestination;
TegraVICImage lastLayout;
TegraVICRect lastArea;
switch_gpu_cpu_lease lease;
uint64_t pages[4096];
TegraVICTraits traits = kTegraVICTraits;

int switch_emc_display_memory_ready()
{
    return 1;
}

int switch_emc_display_memory_reserve(const switch_display_memory_head *request)
{
    reserved = request->peak_bytes_per_second;
    return request->peak_bytes_per_second > reserveLimit ? -1 : 0;
}

void IOLog(const char *, ...)
{}

[[noreturn]] void panic(const char *, ...)
{
    throw std::runtime_error("panic");
}

thread_call_t thread_call_allocate_with_options(thread_call_func_t func, thread_call_param_t param, int, int)
{
    return new thread_call{func, param, false};
}

bool thread_call_enter(thread_call_t call)
{
    const bool was = call->pending;
    call->pending = true;
    return was;
}

bool thread_call_cancel_wait(thread_call_t call)
{
    const bool was = call->pending;
    call->pending = false;
    return was;
}

bool thread_call_free(thread_call_t call)
{
    delete call;
    return true;
}
}  // namespace

extern "C" bool tegra_vic_ready()
{
    return vicReady;
}

extern "C" const TegraVICTraits *tegra_vic_traits()
{
    return &traits;
}

switch_gpu_backing *switch_display_scanout_buffer(size_t bytes)
{
    return new switch_gpu_backing{900 + composites, 0, bytes, 1};
}

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *backing)
{
    ++backing->refs;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *backing)
{
    if (!--backing->refs)
        delete backing;
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
    ++exports;
    *out = &lease;
    *producer = {backing->content + (uint64_t(2) << 32), 1, SWITCH_GPU_ENGINE_VIC, 11};
    return 0;
}

void switch_native_scanout_release(SwitchNativeScanout *scanout)
{
    if (scanout->view.native)
        switch_gpu_backing_release(scanout->view.native);
    *scanout = {};
}

int switch_vic_scanout_compose(const SwitchVICComposeSource *sources, uint32_t count, switch_gpu_backing *destination,
                               const TegraVICImage &layout, const TegraVICRect &area, SwitchNativeScanout *output)
{
    ++composites;
    lastSources.assign(sources, sources + count);
    lastIds.clear();
    for (uint32_t i = 0; i < count; ++i)
        lastIds.push_back(sources[i].scanout->view.native->id);
    lastDestination = destination->id;
    lastLayout = layout;
    lastArea = area;
    *output = {};
    if (composeError)
        return composeError;
    ++destination->content;
    switch_gpu_backing_export_read(destination, &output->view.producer, &output->lease);
    switch_gpu_backing_retain(destination);
    output->view.native = destination;
    return 0;
}

namespace {
struct Loop {
    int runAction(int (*action)(OSObject *, void *, void *, void *, void *), OSObject *object)
    {
        return action(object, nullptr, nullptr, nullptr, nullptr);
    }
};

struct NativeLegacyDisplay : OSObject {
    bool entryPlaneSet[4] = {};
    using PlaneSet = switch_display::Composition;
    using Window = switch_landscape::Window;
    using Commits = switch_display::Compositions<PlaneSet>;
    static constexpr uint32_t kWinABase = 0xb80, kWinBBase = 0xd80, kWinCBase = 0xf80;
    static constexpr uint32_t kWinAUpdate = 1 << 9, kWinBUpdate = 1 << 10, kWinCUpdate = 1 << 11;
    static constexpr uint32_t kWinAActReq = 2, kWinBActReq = 4, kWinCActReq = 8;
    static constexpr uint32_t kGeneralUpdate = 1 << 8, kGeneralActReq = 1;
    static constexpr uint32_t kWinBActHcntrSel = 1 << 4, kWinCActHcntrSel = 1 << 6;
    static constexpr uint32_t kWinBUnderflowInt = 1 << 9, kWinCUnderflowInt = 1 << 10;
    static constexpr uint32_t kDcRegActControl = 0x43, kDcStateControl = 0x41;
    static constexpr uint32_t kDcIntEnable = 0x39, kDcIntStatus = 0x37;
    static constexpr uint32_t kDcStateAccess = 0x40, kReadMuxActive = 1, kWriteMuxActive = 4;
    static constexpr unsigned kPlaneSlots = 8;
    std::array<uint32_t, 4096> registers = {};
    uint32_t *dc = registers.data();

    struct Slot {
        uint64_t low = 0;
        SwitchNativeScanout *native = nullptr;
    } planeSlots[kPlaneSlots];

    SwitchNativeScanout snapshots[kPlaneSlots];

    struct Record {
        native_planes::Layer layer;
    } planeLayers[native_planes::kLayers];

    PlaneSet sets[Commits::kSlots + 1] = {};
    Commits planeLatch;
    PlaneSet planeTarget = {}, planeHeld = {};
    bool planeHeldSet = false, planeBlending = false, planeFiltersWritten[2] = {};
    bool stats = true, gpuScanout = false, planesOn = true;
    int planeCsc[2][2] = {{-1, -1}, {-1, -1}};
    uint32_t planeBlendSaved[3] = {}, pitchActControl = UINT32_MAX, planeBuildingSlots = 0;
    uint64_t planeRequestedAt = 0, frameEnds = 10;
    unsigned stPlaneFlips = 0, stPlaneDropped = 0, stPlaneComposited = 0, deadlines = 0, withdrawals = 0, releases = 0,
             planeUnderflowRun = 0, busyBacking = 0;
    static constexpr unsigned kLatchFrames = 4;
    Loop loop;

    NativeLegacyDisplay()
    {
        landscapeState = {};
        planeLatch.values = sets;
        for (unsigned i = 0; i < kPlaneSlots; ++i)
            planeSlots[i].native = &snapshots[i];
        switch_display_memory_head mode{};
        mode.pixel_clock_hz = 78000000;
        mode.active_width = 720;
        mode.active_height = 1280;
        mode.total_width = 1000;
        mode.total_height = 1300;
        EXPECT_TRUE(displayMemory.configure(mode, 720, 1280, false));
    }

    ~NativeLegacyDisplay()
    {
        stopPlaneComposite();
        for (auto &snapshot : snapshots)
            switch_native_scanout_release(&snapshot);
    }

    void scanoutMemoryBarrier()
    {}

    void planeReleaseIdle()
    {
        ++releases;
    }

    const void *firstUntaken() const
    {
        return nullptr;
    }

    bool planeWindowIdle() const
    {
        return false;
    }

    void getDisplayDimensions(uint32_t *width, uint32_t *height)
    {
        *width = 720;
        *height = 1280;
    }

    uint32_t planeSlotIova(int slot) const
    {
        return 0x10000000 + uint32_t(slot) * 0x100000;
    }

    const PlaneSet &planeSetOf(int id) const
    {
        return sets[id >= 0 ? id : Commits::kSlots];
    }

    void armFlipDeadline()
    {
        ++deadlines;
    }

    void withdrawCompletionFlip()
    {
        ++withdrawals;
    }

    void mediaFlipFromGate()
    {}

    Loop *getWorkLoop()
    {
        return &loop;
    }

    void kickSwapQueue()
    {
        ++kicks;
    }

    void retain()
    {
        ++retains;
    }

    void release()
    {
        --retains;
    }

    static bool planeRetry(int slot)
    {
        return slot == -11 || slot == -16 || slot == -115;
    }

    bool convertedBusy(switch_gpu_backing *backing) const
    {
        return busyBacking && backing->id == busyBacking;
    }

    void convertedReleaseIdle(switch_gpu_backing *)
    {}

    int planeMapSnapshot(SwitchNativeScanout &snapshot, const native_planes::Image &, int = -1)
    {
        switch_native_scanout_release(&snapshots[6]);
        snapshots[6] = snapshot;
        snapshot = {};
        return 6;
    }

#include "../SwitchLandscapeState.h"

    bool landscapeHolds(PlaneSet &)
    {
        return false;
    }

    void landscapeLeaveNow(const char *)
    {}

    void landscapeAbandonWant()
    {}

#include "../SwitchDisplayBandwidthDisplay.h"
#include "../SwitchOverlayRegisters.h"
#include "../SwitchPlaneAdmissionDrop.h"
#include "../SwitchCompositionRegisters.h"
#include "../SwitchOverlayRecovery.h"
#include "../SwitchPlanesResume.h"
#include "../SwitchPlaneCompositeDisplay.h"

    bool stage(const PlaneSet &p)
    {
        uint32_t update = 0, request = 0;
        return planeStage(p, update, request);
    }

    void latch()
    {
        ASSERT_TRUE(planeLatch.latched(false, planeScannedSet()));
        planeRetireBandwidth();
    }
};

NativeLegacyDisplay::LandscapeState NativeLegacyDisplay::landscapeState;

using Display = NativeLegacyDisplay;

// A 4K NV12 video (pitch, in a capture of its own) shown at 1/6 on both axes, wanted on window B or C.
native_planes::Layer video(int32_t x, int32_t y)
{
    native_planes::Layer l;
    l.kind = native_planes::kKindNv12Video;
    l.surface.format = native_planes::fourcc('4', '2', '0', 'v');
    l.surface.planes = 2;
    l.surface.width = 3840;
    l.surface.height = 2160;
    l.surface.stride[0] = l.surface.stride[1] = 3840;
    l.surface.planeHeight[0] = 2160;
    l.surface.planeHeight[1] = 1080;
    l.surface.offset[1] = 3840ull * 2160;
    l.surface.allocation = l.surface.descriptorBytes = 3840ull * 2160 * 3 / 2;
    l.surface.colorMatrix = 2;
    l.src = {0, 0, 3840, 2160};
    l.dst = {x, y, 640, 360};
    return l;
}

void capture(Display &display, unsigned slot, uint64_t id, const native_planes::Layer &layer)
{
    auto &s = display.snapshots[slot];
    s.view.native = new switch_gpu_backing{id, 3, layer.surface.descriptorBytes, 1};
    s.lease = &lease;
    switch_gpu_backing_describe(s.view.native, &s.allocation);
}

switch_display::Overlay overlay(unsigned slot, unsigned depth, const native_planes::Layer &layer)
{
    switch_display::Overlay o;
    o.on = true;
    o.slot = int(slot);
    o.depth = depth;
    EXPECT_TRUE(native_planes::imageOf(layer, 720, 1280, o.image));
    return o;
}

class PlaneComposite : public testing::Test {
protected:
    Display *display = nullptr;
    switch_display::Overlay wanted[2];
    const int selected[2] = {0, 2};

    void SetUp() override
    {
        reserved = composites = exports = kicks = retains = 0;
        reserveLimit = UINT64_MAX;
        composeError = 0;
        vicReady = true;
        traits = kTegraVICTraits;
        display = new Display;
        display->planeLayers[0].layer = video(40, 100);
        display->planeLayers[2].layer = video(40, 600);
        capture(*display, 1, 71, display->planeLayers[0].layer);
        capture(*display, 3, 72, display->planeLayers[2].layer);
        wanted[0] = overlay(1, 1, display->planeLayers[0].layer);
        wanted[1] = overlay(3, 2, display->planeLayers[2].layer);
    }

    void TearDown() override
    {
        delete display;
        EXPECT_EQ(retains, 0u);
    }

    uint64_t twoVideos()
    {
        Display probe;
        switch_display::Overlay copy[2] = {wanted[0], wanted[1]};
        PlaneSet both = {};
        both.window[0] = copy[0];
        both.window[1] = copy[1];
        EXPECT_TRUE(probe.planeReserveBandwidth(both));
        return reserved;
    }

    void runWorker()
    {
        ASSERT_TRUE(display->planeComposite && display->planeComposite->call->pending);
        auto *call = display->planeComposite->call;
        call->pending = false;
        call->func(call->param, nullptr);
    }

    using PlaneSet = Display::PlaneSet;
};

TEST(PlaneCompositeGeometry, AClippedLayerKeepsTheSourceThatLandsOnTheScreen)
{
    native_planes::Layer l = video(-320, 1100);
    switch_display::CompositePart part;
    ASSERT_TRUE(switch_display::compositePart(l, 720, 1280, part));
    EXPECT_EQ(part.screen.x, 0u);
    EXPECT_EQ(part.screen.w, 320u);
    EXPECT_EQ(part.screen.y, 1100u);
    EXPECT_EQ(part.screen.h, 180u);
    EXPECT_EQ(part.srcX, uint64_t(1920) << 16);
    EXPECT_EQ(part.srcW, uint64_t(1920) << 16);
    EXPECT_EQ(part.srcY, 0u);
    EXPECT_EQ(part.srcH, uint64_t(1080) << 16);
    l.dst = {800, 0, 640, 360};
    EXPECT_FALSE(switch_display::compositePart(l, 720, 1280, part));
}

TEST(PlaneCompositeGeometry, TheSurfaceIsTheUnionInWholeGobsAndBlocks)
{
    switch_display::CompositePart parts[2] = {{{40, 100, 640, 360}, 0, 0, 0, 0}, {{0, 600, 720, 405}, 0, 0, 0, 0}};
    const auto u = switch_display::compositeUnion(parts, 2);
    EXPECT_EQ(u.x, 0u);
    EXPECT_EQ(u.y, 100u);
    EXPECT_EQ(u.w, 720u);
    EXPECT_EQ(u.h, 905u);
    const auto block = switch_display::compositeSurface(u, true);
    EXPECT_EQ(block.pitch, 2880u);
    EXPECT_EQ(block.rows, 1024u);
    const auto pitch = switch_display::compositeSurface({0, 0, 333, 10}, false);
    EXPECT_EQ(pitch.pitch, 1344u);
    EXPECT_EQ(pitch.rows, 10u);
    native_planes::Image image;
    const tgm_layout layout = switch_display::compositeLayout(block);
    ASSERT_TRUE(native_planes::imageOf(switch_display::compositeLayer(block, u), 720, 1280, image, &layout));
    EXPECT_EQ(image.position, (100u << 16) | 0u);
    EXPECT_EQ(image.size, (905u << 16) | 720u);
    EXPECT_EQ(image.dda, (4096u << 16) | 4096u);
    EXPECT_EQ(image.surfaceKind,
              native_planes::kSurfaceKindBlock | (4u << native_planes::kSurfaceKindBlockHeightShift));
    EXPECT_EQ(image.lineStride, 2880u);
    EXPECT_FALSE(image.yuv);
    EXPECT_LE(image.readHigh, block.bytes);
}

TEST_F(PlaneComposite, OverlaysAdmissionTakesAreLeftAsTheyAre)
{
    EXPECT_FALSE(display->planeComposeWaits(wanted, selected, 720, 1280));
    EXPECT_EQ(wanted[0].slot, 1);
    EXPECT_EQ(wanted[1].slot, 3);
    EXPECT_EQ(composites, 0u);
}

// Two 4K videos at 1/6 over the budget: one composite of both goes to window B at destination size, admitted, and
// planeStage drops nothing.
TEST_F(PlaneComposite, RefusedOverlaysGoOutAsOneCompositeAndNothingIsDropped)
{
    reserveLimit = twoVideos() - 1;
    EXPECT_TRUE(display->planeComposeWaits(wanted, selected, 720, 1280));
    EXPECT_TRUE(display->planeCompositeUses(1));
    EXPECT_TRUE(display->planeCompositeUses(3));
    EXPECT_EQ(retains, 1u);
    runWorker();
    EXPECT_EQ(retains, 0u);
    ASSERT_EQ(composites, 1u);
    EXPECT_EQ(kicks, 1u);
    EXPECT_FALSE(display->planeCompositeUses(1));
    ASSERT_EQ(lastSources.size(), 2u);
    ASSERT_EQ(lastIds.size(), 2u);
    EXPECT_EQ(lastIds[0], 72u);  // the lower layer is the bottom slot
    EXPECT_EQ(lastIds[1], 71u);
    EXPECT_EQ(lastSources[0].slot.dstY, 500u);
    EXPECT_EQ(lastSources[1].slot.dstY, 0u);
    EXPECT_EQ(lastSources[0].slot.blend, uint32_t(TEGRA_VIC_BLEND_OPAQUE));
    EXPECT_EQ(lastSources[0].image.standard, uint32_t(TEGRA_VIC_YCC_709));
    EXPECT_EQ(lastLayout.width, 640u);
    EXPECT_EQ(lastLayout.height, 860u);
    EXPECT_TRUE(lastLayout.blockLinear);
    EXPECT_EQ(lastArea.width, 640u);
    EXPECT_EQ(display->stPlaneComposited, 1u);

    EXPECT_FALSE(display->planeComposeWaits(wanted, selected, 720, 1280));
    EXPECT_EQ(composites, 1u);
    ASSERT_TRUE(wanted[0].on);
    EXPECT_FALSE(wanted[1].on);
    EXPECT_EQ(wanted[0].slot, 6);
    EXPECT_FALSE(wanted[0].image.yuv);
    EXPECT_EQ(wanted[0].image.position, (100u << 16) | 40u);
    EXPECT_EQ(wanted[0].image.size, (860u << 16) | 640u);
    EXPECT_EQ(display->snapshots[6].view.producer.engine, uint32_t(SWITCH_GPU_ENGINE_VIC));

    PlaneSet set = {};
    set.window[0] = wanted[0];
    set.seq = 1;
    ASSERT_TRUE(display->stage(set));
    EXPECT_EQ(display->stPlaneDropped, 0u);
    const auto &armed = display->planeSetOf(display->planeLatch.armed);
    EXPECT_TRUE(armed.uses(6));
    EXPECT_LE(reserved, reserveLimit);
}

TEST_F(PlaneComposite, TheSameFramesAreComposedOnceAndANewFrameAgain)
{
    reserveLimit = twoVideos() - 1;
    switch_display::Overlay first[2] = {wanted[0], wanted[1]};
    ASSERT_TRUE(display->planeComposeWaits(first, selected, 720, 1280));
    switch_display::Overlay again[2] = {wanted[0], wanted[1]};
    EXPECT_TRUE(display->planeComposeWaits(again, selected, 720, 1280));
    EXPECT_EQ(retains, 1u);
    runWorker();
    switch_display::Overlay cached[2] = {wanted[0], wanted[1]};
    EXPECT_FALSE(display->planeComposeWaits(cached, selected, 720, 1280));
    ++display->snapshots[1].allocation.content_generation;
    switch_display::Overlay next[2] = {wanted[0], wanted[1]};
    EXPECT_TRUE(display->planeComposeWaits(next, selected, 720, 1280));
    runWorker();
    EXPECT_EQ(composites, 2u);
}

TEST_F(PlaneComposite, AJobThatCannotBeSubmittedLetsTheSetGoWithoutIt)
{
    reserveLimit = twoVideos() - 1;
    composeError = -12;
    ASSERT_TRUE(display->planeComposeWaits(wanted, selected, 720, 1280));
    runWorker();
    EXPECT_EQ(display->stPlaneComposited, 0u);
    EXPECT_FALSE(display->planeComposeWaits(wanted, selected, 720, 1280));
    EXPECT_TRUE(wanted[1].on);
    EXPECT_EQ(composites, 1u);
}

TEST_F(PlaneComposite, AWorkerRefusalIsThatPicturesAloneAndTheNextFrameComposesAgain)
{
    reserveLimit = twoVideos() - 1;
    composeError = -95;
    ASSERT_TRUE(display->planeComposeWaits(wanted, selected, 720, 1280));
    runWorker();
    switch_display::Overlay same[2] = {wanted[0], wanted[1]};
    EXPECT_FALSE(display->planeComposeWaits(same, selected, 720, 1280));
    EXPECT_TRUE(same[1].on);
    composeError = 0;
    ++display->snapshots[1].allocation.content_generation;
    EXPECT_TRUE(display->planeComposeWaits(wanted, selected, 720, 1280));
    runWorker();
    EXPECT_EQ(composites, 2u);
    EXPECT_EQ(display->stPlaneComposited, 1u);
}

// A refusal beside another overlay set still on window B may pass once that set retires, so it is not remembered;
// one beside nothing but the route's own composite is the overlays' own.
TEST_F(PlaneComposite, ARefusalIsRememberedOnlyWhenNothingElseIsOnTheWindows)
{
    PlaneSet other = {};
    other.window[0] = wanted[0];
    other.seq = 1;
    ASSERT_TRUE(display->stage(other));
    display->latch();
    reserveLimit = 0;
    switch_display::Overlay first[2] = {wanted[0], wanted[1]};
    EXPECT_TRUE(display->planeComposeWaits(first, selected, 720, 1280));
    EXPECT_FALSE(display->planeComposite->refusedSet);
    runWorker();
    switch_display::Overlay cached[2] = {wanted[0], wanted[1]};
    ASSERT_FALSE(display->planeComposeWaits(cached, selected, 720, 1280));
    reserveLimit = UINT64_MAX;
    PlaneSet composite = {};
    composite.window[0] = cached[0];
    composite.seq = 2;
    ASSERT_TRUE(display->stage(composite));
    display->latch();
    reserveLimit = twoVideos() - 1;
    switch_display::Overlay again[2] = {wanted[0], wanted[1]};
    EXPECT_FALSE(display->planeComposeWaits(again, selected, 720, 1280));
    EXPECT_TRUE(display->planeComposite->refusedSet);
}

// NVDEC's picture in its own 2-GOB blocks, cropped: VIC reads it where it lies, from the crop's corner.
TEST_F(PlaneComposite, ANativePictureIsReadBlockLinearFromItsCrop)
{
    auto &native = display->snapshots[1];
    auto &layout = native.view.layout;
    layout.format = TEGRA_VIC_FOURCC('4', '2', '0', 'v');
    layout.plane_count = 2;
    layout.color_matrix = 2;
    layout.planes[0] = {0, 3904ull * 2176, 3872, 2176, 3904, 1, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
    layout.planes[1] = {3904ull * 2176, 3904ull * 1088, 1936, 1088, 3904, 2, SWITCH_GPU_BACKING_BLOCK_LINEAR, 1};
    native.view.crop = {16, 8, 3840, 2160};
    native.allocation.page_count = 4096;
    reserveLimit = twoVideos() - 1;
    ASSERT_TRUE(display->planeComposeWaits(wanted, selected, 720, 1280));
    runWorker();
    ASSERT_EQ(lastSources.size(), 2u);
    const auto &top = lastSources[1];
    EXPECT_TRUE(top.image.blockLinear);
    EXPECT_EQ(top.image.blockHeightLog2, 1u);
    EXPECT_EQ(top.image.width, 3872u);
    EXPECT_EQ(top.slot.srcX, uint64_t(16) << 16);
    EXPECT_EQ(top.slot.srcY, uint64_t(8) << 16);
    EXPECT_EQ(top.slot.srcW, uint64_t(3840) << 16);
    EXPECT_FALSE(lastSources[0].image.blockLinear);
}

// HW Test display.planes_composite: armed, an overlay admission takes goes as the composite; the report counts it and
// disarms.
TEST_F(PlaneComposite, TheArmedProbeComposesWhatAdmissionTakesAndTheReportDisarms)
{
    EXPECT_EQ(display->planeCompositeProbe(true), 0u);
    switch_display::Overlay one[2] = {wanted[0], {}};
    ASSERT_TRUE(display->planeComposeWaits(one, selected, 720, 1280));
    runWorker();
    ASSERT_FALSE(display->planeComposeWaits(one, selected, 720, 1280));
    EXPECT_EQ(one[0].slot, 6);
    EXPECT_EQ(lastIds.size(), 1u);
    EXPECT_EQ(display->planeCompositeProbe(false), 1u);
    switch_display::Overlay after[2] = {wanted[0], {}};
    EXPECT_FALSE(display->planeComposeWaits(after, selected, 720, 1280));
    EXPECT_EQ(after[0].slot, 1);
    EXPECT_EQ(composites, 1u);
}

TEST_F(PlaneComposite, WithoutMultiSlotTwoOverlaysAreNotComposed)
{
    reserveLimit = twoVideos() - 1;
    traits.multiSlot = 0;
    EXPECT_FALSE(display->planeComposeWaits(wanted, selected, 720, 1280));
    EXPECT_EQ(composites, 0u);
    EXPECT_EQ(retains, 0u);
}

TEST_F(PlaneComposite, AStopWithAJobQueuedGivesEverythingBack)
{
    reserveLimit = twoVideos() - 1;
    ASSERT_TRUE(display->planeComposeWaits(wanted, selected, 720, 1280));
    display->stopPlaneComposite();
    EXPECT_EQ(retains, 0u);
    EXPECT_EQ(composites, 0u);
    EXPECT_EQ(display->planeComposite, nullptr);
}

TEST_F(PlaneComposite, EveryBufferScannedWaitsForALatch)
{
    reserveLimit = twoVideos() - 1;
    ASSERT_TRUE(display->planeComposeWaits(wanted, selected, 720, 1280));
    runWorker();
    display->busyBacking = 900;
    ++display->snapshots[1].allocation.content_generation;
    for (unsigned i = 0; i < 2; ++i) {
        switch_display::Overlay next[2] = {wanted[0], wanted[1]};
        ASSERT_TRUE(display->planeComposeWaits(next, selected, 720, 1280));
        runWorker();
        EXPECT_NE(lastDestination, 900u);
        ++display->snapshots[1].allocation.content_generation;
    }
    EXPECT_EQ(composites, 3u);
}
}  // namespace
