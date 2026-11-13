// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include "../../TegraVIC/SwitchVICScanoutConvert.h"
#include "bazel/testing/source_text.h"

#define SWITCH_FRAME_TIMELINE          0
#define IONew(type, count)             static_cast<type *>(std::calloc(count, sizeof(type)))
#define IONewZero(type, count)         static_cast<type *>(std::calloc(count, sizeof(type)))
#define IODelete(pointer, type, count) std::free(pointer)
#define OSSafeReleaseNULL(p) \
    do {                     \
        if (p)               \
            (p)->release();  \
        (p) = nullptr;       \
    } while (0)

struct switch_gpu_backing {
    uint64_t first;
    uint32_t pages;
    unsigned refs;
};

struct switch_gpu_cpu_lease {};

struct OriginalLegacySwapRequest {
    int id;
};

class OSObject {
public:
    int refs = 1;

    void retain()
    {
        ++refs;
    }

    void release()
    {
        --refs;
    }
};

using IOByteCount = uint64_t;
constexpr unsigned kIOMemoryMapperNone = 0x800;

class IOMemoryDescriptor {
public:
    uint64_t pa;

    uint64_t getPhysicalSegment(uint64_t offset, IOByteCount *length, unsigned)
    {
        *length = 4096 - (offset & 4095);
        return pa + offset;
    }
};

constexpr int kIOReturnAborted = -14;
constexpr int TEGRA_SCANOUT_SOURCE_LOST = -5;
constexpr uint32_t kNativeBlockWidth = 720, kNativeBlockHeight = 1280, kScanBlockLog2 = 4;
constexpr uint64_t kNativeBlockStride = 2880, kNativeBlockBytes = 0x384000;

namespace {
TegraVICTraits traits = kTegraVICTraits;
IOMemoryDescriptor descriptor = {0x90000000};
uint64_t sourcePages[2048];
bool vicUp = true;
unsigned refusals, fences, placed, swapStages, snapshots;
int placeError, fenceError, completedStatus = 1;
switch_gpu_cpu_lease sourceLease, targetLease;
switch_gpu_backing sourceBacking = {0x90000000, 2048, 1}, windowB = {0xa0000000, 256, 1};
TegraVICImage placedImage, placedLayout;
TegraVICOp placedOperation;
SwitchVICPlaceReads placedAlso;
switch_gpu_backing *placedTarget;
switch_gpu_backing_token fenced;
volatile uint64_t native_legacy_early_retires;

uint64_t mach_absolute_time()
{
    return 7;
}

void native_legacy_swap_stage(unsigned)
{
    ++swapStages;
}

void IOLog(const char *, ...)
{}

[[noreturn]] void panic(const char *, ...)
{
    throw std::runtime_error("panic");
}

void switch_refusal(const char *, const char *, const char *, ...)
{
    ++refusals;
}

bool tegra_vic_ready()
{
    return vicUp;
}

const TegraVICTraits *tegra_vic_traits()
{
    return &traits;
}

IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *)
{
    return &descriptor;
}

void native_legacy_gpu_complete(void *, int status)
{
    completedStatus = status;
}
}  // namespace

extern "C" switch_gpu_backing *switch_gpu_backing_register(const uint64_t *pages, uint32_t count, uint32_t, void *,
                                                           void (*)(void *))
{
    for (uint32_t i = 0; i < count; ++i)
        EXPECT_EQ(pages[i], pages[0] + (uint64_t(i) << 12));
    return new switch_gpu_backing{pages[0], count, 1};
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

int switch_native_scanout_pitch_wait(OSObject *, SwitchNativeScanout *scanout)
{
    ++snapshots;
    *scanout = {};
    scanout->lease = &sourceLease;
    scanout->view.native = &sourceBacking;
    scanout->view.producer = {41, 1, SWITCH_GPU_ENGINE_GM20B, 3};
    scanout->allocation.page_count = sourceBacking.pages;
    scanout->allocation.pages = sourcePages;
    return 0;
}

void switch_native_scanout_release(SwitchNativeScanout *scanout)
{
    *scanout = {};
}

int switch_vic_scanout_place(const SwitchNativeScanout *source, const TegraVICImage *image,
                             switch_gpu_backing *destination, const TegraVICImage *layout, const TegraVICOp *operation,
                             const SwitchVICPlaceReads &also, SwitchNativeScanout *output)
{
    EXPECT_EQ(source->view.native, &sourceBacking);
    ++placed;
    placedImage = *image;
    placedLayout = *layout;
    placedOperation = *operation;
    placedAlso = also;
    placedTarget = destination;
    if (placeError)
        return placeError;
    output->lease = &targetLease;
    output->view.producer = {(uint64_t(5) << 32) + 77, 1, SWITCH_GPU_ENGINE_VIC, 161};
    return 0;
}

extern "C" int switch_host1x_token_fence(const switch_gpu_backing_token *tokens, uint32_t count,
                                         void (*completed)(void *, int), void *)
{
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(completed, native_legacy_gpu_complete);
    ++fences;
    fenced = tokens[0];
    return fenceError;
}

namespace {
struct Display {
    static constexpr unsigned kScanBuffers = 3;
    static constexpr int kNoBuffer = -1;
    static constexpr unsigned kGpuCopyDeadlineMS = 1000, kGpuCopyDeadlines = 10;

    struct Ring {
        int free = 1, claimed = kNoBuffer, released = 0;

        int target() const
        {
            return claimed == kNoBuffer ? free : kNoBuffer;
        }

        void claim(int i)
        {
            claimed = i;
        }

        void release()
        {
            claimed = kNoBuffer;
            ++released;
        }
    } ring;

    struct Timer {
        unsigned armed = 0, cancelled = 0;

        void setTimeoutMS(unsigned)
        {
            ++armed;
        }

        void cancelTimeout()
        {
            ++cancelled;
        }
    } timer;

    struct Source {
        unsigned fired = 0;

        void interruptOccurred(void *, void *, int)
        {
            ++fired;
        }
    } source;

    struct Overlay {
        bool on;
        int slot;
    };

    struct PlaneSet {
        Overlay window[2];
    };

    struct Slot {
        SwitchNativeScanout *native;
    };

    bool primary = true, hwRotate = true, doubleBuffer = true, gpuCopy = true, copyStopping = false;
    bool kickDeferred = false, transferred = false, latchRetire = true, gpuRetired = false, rekick = false;
    bool hwSuspended = false, gpuPresentDeferred = false, gpuDonePending = false, gpuScanout = false;
    bool planeNextSet = false;
    uint64_t gpuSequence = 0, gpuOfferedAt = 0, gpuLength = 0;
    const OriginalLegacySwapRequest *gpuRequest = nullptr, *gpuPresented = nullptr, *pending = nullptr;
    int gpuPresentedEntry = 0;
    PlaneSet planeNext = {};
    Slot planeSlots[8] = {};
    Timer *gpuDeadline = &timer;
    Source *copySource = &source;
    unsigned presented = 0, retiredUnshown = 0, retired = 0, kicks = 0, lost = 0, taken = 0, finished = 0;
    int armedFor = -1;
    switch_gpu_backing_token waiterFor = {};
    unsigned waiters = 0;

    uint32_t surfaceAddress(int i) const
    {
        return 0xf5a00000u + uint32_t(i) * 0x400000u;
    }

    void noteTaken(const OriginalLegacySwapRequest *, uint64_t)
    {
        ++taken;
    }

    bool retireSwap(const OriginalLegacySwapRequest *, bool)
    {
        ++retired;
        return true;
    }

    void presentGpuCopy()
    {
        ++presented;
    }

    void dropLostFrame(const OriginalLegacySwapRequest *, int)
    {
        ++lost;
    }

    void releaseIdleDirectReads()
    {}

    void retireUnshown(uint64_t)
    {
        ++retiredUnshown;
    }

    uint64_t seqOf(const OriginalLegacySwapRequest *) const
    {
        return 3;
    }

    const OriginalLegacySwapRequest *pendingSwapRequest() const
    {
        return pending;
    }

    void finishTransfer(const OriginalLegacySwapRequest *, bool)
    {
        ++finished;
    }

    void kickSwapQueue()
    {
        ++kicks;
    }

    bool armCompletionFlip(int entry)
    {
        armedFor = entry;
        return true;
    }

    void armCompletionFlipWaiter(const switch_gpu_backing_token &producer)
    {
        waiterFor = producer;
        ++waiters;
    }

#include "../SwitchVICScanoutDisplay.h"

    ~Display()
    {
        stopVicCopy();
    }
};

class VicScanout : public testing::Test {
protected:
    Display display;
    OSObject surface;
    OriginalLegacySwapRequest request = {1};

    void SetUp() override
    {
        display.startVicCopy();
        traits = kTegraVICTraits;
        vicUp = true;
        refusals = fences = placed = swapStages = snapshots = 0;
        placeError = fenceError = 0;
        completedStatus = 1;
        descriptor.pa = 0x90000000;
        for (uint32_t i = 0; i < 2048; ++i)
            sourcePages[i] = 0x90000000 + (uint64_t(i) << 12);
    }

    bool offer(uint64_t stride, int blockLog2 = -1)
    {
        return display.offerVicCopy(&request, &surface, stride, stride * 1280, 720, 1280, blockLog2, 9);
    }

    // The copy thread's turn, then the completion it armed coming back to the gate.
    void run()
    {
        ASSERT_TRUE(display.vicCopyWork());
        if (!fenceError && !placeError)
            completedStatus = 0;
        ASSERT_TRUE(display.finishVicCopy(completedStatus));
    }
};

TEST_F(VicScanout, APitchFrameIsPlacedInWindowABlocksAndFlipsFromVicsSyncpoint)
{
    ASSERT_TRUE(offer(3072));
    EXPECT_TRUE(display.gpuScanout);
    EXPECT_TRUE(display.transferred);
    EXPECT_EQ(display.ring.claimed, 1);
    EXPECT_EQ(display.armedFor, 1) << "VIC's completion writes window A's flip to the buffer it copies into";
    EXPECT_EQ(display.source.fired, 1u);
    EXPECT_EQ(display.timer.armed, 1u);
    ASSERT_TRUE(display.vicCopyWork());
    EXPECT_EQ(placedTarget->first, 0xf5a00000u + 0x400000u);
    EXPECT_EQ(placedTarget->pages, uint32_t(kNativeBlockBytes >> 12));
    EXPECT_EQ(placedImage.pitch[0], 3072u);
    EXPECT_EQ(placedImage.blockLinear, 0u);
    EXPECT_EQ(placedImage.span[0], 3072u * 1280);
    EXPECT_EQ(placedLayout.blockLinear, 1u);
    EXPECT_EQ(placedLayout.blockHeightLog2, 4u);
    EXPECT_EQ(placedLayout.pitch[0], 2880u);
    EXPECT_EQ(placedLayout.rows[0], 1280u);
    EXPECT_EQ(placedLayout.span[0], kNativeBlockBytes);
    EXPECT_EQ(placedOperation.dstW, 720u);
    EXPECT_EQ(placedOperation.nearest, 1u);
    EXPECT_EQ(fences, 1u);
    EXPECT_EQ(fenced.engine, uint32_t(SWITCH_GPU_ENGINE_VIC));
    EXPECT_EQ(display.waiters, 1u) << "host1x's syncpoint filter may write the flip once the fence is armed";
    EXPECT_EQ(display.waiterFor.value, fenced.value);
    EXPECT_EQ(display.presented, 0u);
    ASSERT_TRUE(display.finishVicCopy(0));
    EXPECT_EQ(display.presented, 1u);
    EXPECT_FALSE(display.gpuScanout);
    EXPECT_EQ(display.gpuPresented, &request);
    EXPECT_EQ(surface.refs, 1);
}

TEST_F(VicScanout, ABlockLinearFrameInWindowABlocksIsCopiedAsItsBytes)
{
    ASSERT_TRUE(offer(2880, 4));
    run();
    EXPECT_EQ(placedLayout.blockLinear, 0u);
    EXPECT_EQ(placedLayout.pitch[0], 4096u);
    EXPECT_EQ(placedLayout.rows[0], 900u);
    EXPECT_EQ(placedImage.pitch[0], 4096u);
    EXPECT_EQ(placedImage.span[0], kNativeBlockBytes);
    EXPECT_EQ(placedOperation.srcW, uint64_t(1024) << 16);
    EXPECT_EQ(display.presented, 1u);
}

TEST_F(VicScanout, RowsVicCannotReadOrOtherBlocksOrNoVicAreDeclined)
{
    EXPECT_FALSE(offer(2880));
    EXPECT_EQ(refusals, 1u);
    EXPECT_FALSE(offer(2880, 3));
    traits.blockLinearOut = 0;
    EXPECT_FALSE(offer(3072));
    traits = kTegraVICTraits;
    vicUp = false;
    EXPECT_FALSE(offer(3072));
    EXPECT_FALSE(display.gpuScanout);
    EXPECT_EQ(display.source.fired, 0u);
}

TEST_F(VicScanout, EverySurfaceHeldDefersToTheFrameEndThatFreesOne)
{
    display.ring.free = Display::kNoBuffer;
    EXPECT_TRUE(offer(3072));
    EXPECT_TRUE(display.kickDeferred);
    EXPECT_FALSE(display.gpuScanout);
}

TEST_F(VicScanout, TheWindowBSurfacesTheFlipShowsAreWaitedForByTheJob)
{
    SwitchNativeScanout shown = {};
    shown.lease = &sourceLease;
    shown.view.native = &windowB;
    shown.view.producer = {12, 1, SWITCH_GPU_ENGINE_GM20B, 4};
    display.planeSlots[2].native = &shown;
    display.planeNext.window[0] = {true, 2};
    display.planeNextSet = true;
    ASSERT_TRUE(offer(3072));
    EXPECT_EQ(windowB.refs, 2u);
    run();
    ASSERT_EQ(placedAlso.count, 1u);
    EXPECT_EQ(placedAlso.backings[0], &windowB);
    EXPECT_EQ(windowB.refs, 1u);
}

TEST_F(VicScanout, AFailedJobIsNeverFlippedToAndItsSwapRetires)
{
    placeError = -95;
    ASSERT_TRUE(offer(3072));
    ASSERT_TRUE(display.vicCopyWork());
    EXPECT_EQ(completedStatus, -95);
    ASSERT_TRUE(display.finishVicCopy(completedStatus));
    EXPECT_EQ(display.presented, 0u);
    EXPECT_EQ(display.ring.released, 1);
    EXPECT_EQ(display.retiredUnshown, 1u);
    EXPECT_EQ(display.kicks, 1u);
    EXPECT_FALSE(display.transferred);
    EXPECT_EQ(refusals, 1u);
    EXPECT_EQ(surface.refs, 1);
}

TEST_F(VicScanout, ASnapshotOfOtherPagesThanTheSwapsIsNotCopied)
{
    descriptor.pa = 0x98000000;
    ASSERT_TRUE(offer(3072));
    ASSERT_TRUE(display.vicCopyWork());
    EXPECT_EQ(placed, 0u);
    EXPECT_EQ(completedStatus, -22);
    ASSERT_TRUE(display.finishVicCopy(completedStatus));
    EXPECT_EQ(display.presented, 0u);
    EXPECT_EQ(refusals, 2u);
}

TEST_F(VicScanout, ALostSourceIsTheGpuDriversLostFrame)
{
    ASSERT_TRUE(offer(3072));
    ASSERT_TRUE(display.vicCopyWork());
    ASSERT_TRUE(display.finishVicCopy(SWITCH_GPU_BACKING_DEPENDENCY_FAILED));
    EXPECT_EQ(display.lost, 1u);
}

TEST_F(VicScanout, AJobVicNeverEndsPanicsAfterTenDeadlines)
{
    ASSERT_TRUE(offer(3072));
    for (unsigned i = 1; i < Display::kGpuCopyDeadlines; ++i)
        EXPECT_TRUE(display.vicCopyLate());
    EXPECT_THROW(display.vicCopyLate(), std::runtime_error);
}

TEST_F(VicScanout, TheCpuCopiesOnlyWhileVicIsNotUp)
{
    vicUp = false;
    EXPECT_FALSE(display.cpuCopyRefused(&request, 9));
    vicUp = true;
    EXPECT_TRUE(display.cpuCopyRefused(&request, 9));
    EXPECT_EQ(display.retiredUnshown, 1u);
    EXPECT_TRUE(display.rekick);
    EXPECT_EQ(refusals, 1u);
}

TEST_F(VicScanout, AQueuedJobTheCopyThreadNeverTookGoesAtTheStop)
{
    ASSERT_TRUE(offer(3072));
    display.stopVicCopy();
    EXPECT_FALSE(display.gpuScanout);
    EXPECT_FALSE(display.transferred);
    EXPECT_EQ(display.ring.released, 1);
    EXPECT_EQ(surface.refs, 1);
    EXPECT_FALSE(display.vicCopyWork());
}

TEST_F(VicScanout, TheKickTriesVicAfterTheGpuCopyAndBeforeTheCpu)
{
    const std::string source = switch_ios::test::SourceText("../NativeLegacyDisplay.cpp");
    const std::string kick = switch_ios::test::Between(source, "    void kickOnce()", "\n    }\n");
    const auto gpu = kick.find("offerGpuCopy(request, memory");
    const auto vic = kick.find("if (offerVicCopy(request, surface, sourceStride, allocation, width, height, blockLog2, "
                               "kickAt))");
    const auto refused = kick.find("if (cpuCopyRefused(request, kickAt))");
    const auto cpu = kick.find("sourceMapping(memory, ownedSource)");
    ASSERT_NE(vic, std::string::npos);
    ASSERT_NE(refused, std::string::npos);
    EXPECT_LT(gpu, vic);
    EXPECT_LT(vic, refused);
    EXPECT_LT(refused, cpu);
    EXPECT_THAT(source,
                testing::HasSubstr("    void copyWork()\n    {\n        if (vicCopyWork())\n            return;"));
    EXPECT_THAT(source,
                testing::HasSubstr("    void finishGpuSwap(int status)\n    {\n        if (finishVicCopy(status))"));
    EXPECT_THAT(source, testing::HasSubstr("        if (self->vicCopyLate())\n            return;"));
    EXPECT_THAT(source, testing::HasSubstr("        startGpuCopy();\n        startVicCopy();"));
    EXPECT_THAT(source, testing::HasSubstr("                self->stopVicCopy();"));
}
}  // namespace
