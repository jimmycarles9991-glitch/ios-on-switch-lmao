// SPDX-License-Identifier: BSD-2-Clause-Patent
// One display period of a scroll or a scale frame as the display runs it (they are the same to the display: one swap, one
// frame end), counted: the DC A frame-end filter (with the colour unit's), the frame-end action (the hand-off, the
// colour unit's enable and palette, the latch), the kick, and either a direct flip from the kick or a fence-only offer
// whose completion writes window A. Every lock taken, atomic read-modify-write, hand-off (a work-loop signal, a timer's
// arm or cancel) and work-loop pass, by the real code: SwitchFrameEndDisplay.h, SwitchCompletionFlipDisplay.h,
// SwitchGpuDoneDisplay.h, SwitchFlipDeadlineDisplay.h and TegraDisplayColor.cpp, on the completion harness's DC A.
// Boot 21's split: about half the frames flip from the kick, half through the completion.
#include <gtest/gtest.h>

#include <IOKit/IOLib.h>

#include <cstdint>
#include <cstring>

#include "../TegraDisplayColor.h"
#include "switch_completion_flip_harness.h"

namespace {
uint32_t gColourDc[0x1000];
unsigned gHolds;
}  // namespace

extern "C" void native_legacy_frame_end_hold(void)
{
    ++gHolds;
}

extern "C" void native_legacy_frame_end_release(void)
{
    --gHolds;
}

extern "C" uint32_t switch_mmio_model_read32(uintptr_t address)
{
    uint32_t value = 0;
    memcpy(&value, reinterpret_cast<const void *>(address), sizeof(value));
    return value;
}

extern "C" void switch_mmio_model_write32(uintptr_t address, uint32_t value)
{
    memcpy(reinterpret_cast<void *>(address), &value, sizeof(value));
}

extern "C" uint16_t switch_mmio_model_read16(uintptr_t)
{
    return 0;
}

extern "C" void switch_mmio_model_write16(uintptr_t, uint16_t)
{}

extern "C" uint8_t switch_mmio_model_read8(uintptr_t)
{
    return 0;
}

extern "C" void switch_mmio_model_write8(uintptr_t, uint8_t)
{}

namespace completion_flip_test {
namespace {

constexpr int kSlot = Display::kSlot0;

struct Counts {
    unsigned colourLocks, intMaskLocks, flipLocks, rmws, signals, wakeOccurred, timerArms, timerCancels, passes;
};

class DisplayPeriod : public testing::Test {
protected:
    Display d;
    TegraDisplayColor colour;
    unsigned passes = 0;

    void SetUp() override
    {
        gOffer = nullptr;
        gLog.clear();
        memset(gColourDc, 0, sizeof(gColourDc));
        memset(&colour, 0, sizeof(colour));
        ASSERT_TRUE(colour.init());
        colour.attach(SwitchMMIOWords(reinterpret_cast<volatile uint8_t *>(gColourDc)));
        d.color = &colour;
        d.startCompletionFlip();
        for (int i = 0; i < 4; ++i)
            directPeriod();
        for (int i = 0; i < 4; ++i)
            completionPeriod();
    }

    void TearDown() override
    {
        colour.detach();
        colour.free();
        gOffer = nullptr;
    }

    Counts now() const
    {
        return {::gSimpleLockTakes.load(),
                d.intMaskLock->takes,
                gHarnessSimpleLocks,
                gRmws,
                d.vblankSource.signals,
                d.gpuDoneStore.signals,
                d.flipTimerStore.arms + d.gpuDeadlineStore.arms,
                d.flipTimerStore.cancels + d.gpuDeadlineStore.cancels,
                passes};
    }

    static Counts since(const Counts &a, const Counts &b)
    {
        return {b.colourLocks - a.colourLocks,
                b.intMaskLocks - a.intMaskLocks,
                b.flipLocks - a.flipLocks,
                b.rmws - a.rmws,
                b.signals - a.signals,
                b.wakeOccurred - a.wakeOccurred,
                b.timerArms - a.timerArms,
                b.timerCancels - a.timerCancels,
                b.passes - a.passes};
    }

    // vblankFired: the work-loop pass the filter signalled, in NativeLegacyDisplay's order for these steps.
    void frameEndAction()
    {
        ++passes;
        d.tick();
        colour.frameEndAction(d.ring.pending == Display::kNoBuffer);
        (void)colour.windowOptions();
    }

    void frameEnd()
    {
        d.model.boundary();
        d.feStamp = mach_absolute_time();
        d.frameEndInterrupt(false);
        frameEndAction();
    }

    // kickSwapQueue's steps before kickOnce, with no swap queued behind an offer out.
    void kick()
    {
        d.kickUnmaskFrameEnd();
        d.kickCompletionPass(false);
    }

    // A swap whose producer's writes are in DRAM: the kick flips window A to its slot.
    void directPeriod()
    {
        kick();
        d.flipRequest = ++d.nextSwap;
        d.swaps.push_back({d.flipRequest});
        d.presentEntry(kSlot + static_cast<int>(d.nextSwap % 2));
        frameEnd();
    }

    // A swap whose producer still runs: a fence-only offer whose completion writes window A.
    void completionPeriod()
    {
        kick();
        ASSERT_EQ(d.offer(kSlot + static_cast<int>(d.nextSwap % 2)), 0);
        d.complete(0);
        if (d.gpuDoneStore.signals != d.donePasses)
            ++passes;
        d.donePass();
        frameEnd();
    }
};

// What a period costs, by lock: none is taken (the colour unit's in the DC A filter least of all), no read-modify-write
// on the direct path and one claim on the completion's, one signal from the filter to the frame-end action and nothing
// else handed off, one work-loop pass.
TEST_F(DisplayPeriod, ADirectFlipPeriod)
{
    const Counts before = now();
    directPeriod();
    const Counts c = since(before, now());
    EXPECT_EQ(c.colourLocks, 0u) << "colour unit: filter, action, windowOptions";
    EXPECT_EQ(c.intMaskLocks, 0u) << "the kick's INT_MASK";
    EXPECT_EQ(c.flipLocks, 0u);
    EXPECT_EQ(c.rmws, 0u) << "the filter's hand-off";
    EXPECT_EQ(c.signals, 1u);
    EXPECT_EQ(c.wakeOccurred, 0u);
    EXPECT_EQ(c.timerArms, 0u) << "a deadline per flip";
    EXPECT_EQ(c.timerCancels, 0u) << "a deadline cancelled per latch";
    EXPECT_EQ(c.passes, 1u);
}

TEST_F(DisplayPeriod, AFenceOnlyCompletionPeriod)
{
    const Counts before = now();
    completionPeriod();
    const Counts c = since(before, now());
    EXPECT_EQ(c.colourLocks, 0u) << "colour unit: filter, action, windowOptions";
    EXPECT_EQ(c.intMaskLocks, 0u) << "the kick's INT_MASK";
    EXPECT_EQ(c.flipLocks, 0u) << "completionFlipLock: arm, write, pass";

    EXPECT_EQ(c.rmws, 1u) << "the hand-off, the completion's pass; one claim stays";
    EXPECT_EQ(c.signals, 1u);
    EXPECT_EQ(c.wakeOccurred, 0u) << "the completion's own work-loop pass";
    EXPECT_EQ(c.timerArms, 0u) << "the GPU deadline per offer, the flip deadline per flip";
    EXPECT_EQ(c.timerCancels, 0u);
    EXPECT_EQ(c.passes, 1u) << "the completion's pass folds into the frame end";
}

// Both periods keep the screen moving: each latch retires its swap at the frame end that shows it.
TEST_F(DisplayPeriod, EachSwapRetiresAtTheFrameEndThatShowsIt)
{
    for (int i = 0; i < 6; ++i) {
        const size_t retired = d.retired.size();
        if (i % 2)
            completionPeriod();
        else
            directPeriod();
        EXPECT_EQ(d.retired.size(), retired + 1) << "period " << i;
        EXPECT_EQ(d.ring.pending, Display::kNoBuffer) << "period " << i;
    }
}

TEST_F(DisplayPeriod, ConsoleTapeRearmsAfterStartupFramesWereMaskedAtIdle)
{
    gFrameEndHolds = 0;
    d.tapeAttached = false;
    frameEnd();
    frameEnd();
    d.maskIdleFrameEnd();
    const unsigned signals = d.vblankSource.signals;
    const uint64_t frames = d.tapeFrames;
    d.model.boundary();
    d.model.general[Display::kDcIntStatus] |= Display::kFrameEndInt;
    ASSERT_EQ(d.model.general[Display::kDcIntMask] & Display::kFrameEndInt, 0u);
    EXPECT_EQ(d.vblankSource.signals, signals);
    EXPECT_EQ(d.tapeFrames, frames);

    d.tapeAttached = true;
    d.unmaskFrameEnd();
    ASSERT_NE(d.model.general[Display::kDcIntMask] & Display::kFrameEndInt, 0u);
    EXPECT_EQ(d.model.general[Display::kDcIntStatus] & Display::kFrameEndInt, 0u);
    EXPECT_EQ(d.tapeFrames, frames);
    frameEnd();
    EXPECT_EQ(d.vblankSource.signals, signals + 1);
    EXPECT_EQ(d.tapeFrames, frames + 1);
}

// FRAME_END stays routed while a hold is out: the kick skips intMaskLock only while the bit it last saw set is the gate's
// to clear, the idle mask leaves it for a hold, and a mask the gate wrote sends the next kick through the lock again.
TEST_F(DisplayPeriod, TheKicksMaskShadowFollowsEveryWriter)
{
    const auto frameEndUnmasked = [&] {
        return (d.model.general[Display::kDcIntMask] & Display::kFrameEndInt) != 0u;
    };
    ASSERT_TRUE(frameEndUnmasked());
    const unsigned takes = d.intMaskLock->takes;
    kick();
    EXPECT_EQ(d.intMaskLock->takes, takes);
    gFrameEndHolds = 1;
    d.unmaskFrameEnd();
    d.maskIdleFrameEnd();
    EXPECT_TRUE(frameEndUnmasked()) << "a hold is out";
    gFrameEndHolds = 0;
    d.maskIdleFrameEnd();
    EXPECT_FALSE(frameEndUnmasked());
    const unsigned masked = d.intMaskLock->takes;
    kick();
    EXPECT_TRUE(frameEndUnmasked());
    EXPECT_EQ(d.intMaskLock->takes, masked + 1) << "the masked->unmasked edge under the lock";
    d.writeIntMask(Display::kFrameEndInt, 0);
    kick();
    EXPECT_TRUE(frameEndUnmasked());
    d.scanoutStopped = true;
    d.writeIntMask(Display::kFrameEndInt, 0);
    kick();
    EXPECT_FALSE(frameEndUnmasked()) << "a stopped scanout gives no frame end";
    d.scanoutStopped = false;
    d.writeIntMask(0, Display::kFrameEndInt);
    const unsigned live = d.intMaskLock->takes;
    kick();
    EXPECT_EQ(d.intMaskLock->takes, live);
    d.intMaskDead();
    kick();
    EXPECT_EQ(d.intMaskLock->takes, live + 2) << "after the stop every kick asks again";
}

// Each cause the filter takes reaches the action once; two frame ends before it are one status and one fold, a stale
// V_BLANK the gate drops is not taken, and nothing the filter counts is lost to the action's read. DC A's other
// causes (V_PULSE2, INT_STATUS bit 5) are none of its: it neither takes nor clears them.
TEST_F(DisplayPeriod, TheFiltersCountsReachTheActionOnce)
{
    d.stats = true;
    d.model.general[DcModel::kIntStatus] |= Display::kVblankInt;
    d.frameEndInterrupt(false);
    d.frameEndInterrupt(false);
    EXPECT_TRUE(d.frameEndPending(Display::kFrameEndInt));
    d.dropFrameEndStatus(Display::kVblankInt);
    const uint32_t folded = d.stFeFolded;
    EXPECT_EQ(d.takeFrameEndStatus(), Display::kFrameEndInt);
    EXPECT_EQ(d.stFeFolded, folded + 1);
    EXPECT_FALSE(d.frameEndPending(Display::kFrameEndInt | Display::kVblankInt));
    EXPECT_EQ(d.takeFrameEndStatus(), 0u);
    constexpr uint32_t kVpulse2 = 1u << 5;
    d.model.general[DcModel::kIntStatus] |= kVpulse2;
    d.frameEndInterrupt(false);
    EXPECT_EQ(d.takeFrameEndStatus(), Display::kFrameEndInt);
    EXPECT_EQ(d.model.general[DcModel::kIntStatus] & kVpulse2, kVpulse2) << "left as DC A raised it";
    EXPECT_EQ(d.vblankSource.signals, d.feTaken) << "one signal per filter run";
}

// A written flip's pass waits for the frame end, which runs it before it reads the latch: the swap retires at that
// latch. The gate wants the pass now when a swap waits behind the offer, before or after the completion wrote.
TEST_F(DisplayPeriod, AWrittenFlipsPassRunsAtTheFrameEndUnlessTheGateWantsItNow)
{
    kick();
    ASSERT_EQ(d.offer(kSlot), 0);
    d.complete(0);
    EXPECT_EQ(d.gpuDoneStore.signals, d.donePasses) << "no work-loop pass of its own";
    const size_t retired = d.retired.size();
    frameEnd();
    EXPECT_EQ(d.retired.size(), retired + 1) << "retired at the latch that showed it";

    kick();
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    d.kickCompletionPass(true);
    d.complete(0);
    EXPECT_NE(d.gpuDoneStore.signals, d.donePasses) << "a swap waits behind it: signalled";
    d.donePass();
    frameEnd();

    kick();
    ASSERT_EQ(d.offer(kSlot), 0);
    d.complete(0);
    ASSERT_EQ(d.gpuDoneStore.signals, d.donePasses);
    d.kickCompletionPass(true);
    EXPECT_NE(d.gpuDoneStore.signals, d.donePasses) << "the gate saw the flip written and signalled the pass itself";
    d.donePass();
    frameEnd();

    d.kickCompletionPass(false);
    d.scanoutStopped = true;
    d.publishCompletionPass();
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    d.complete(0);
    EXPECT_NE(d.gpuDoneStore.signals, d.donePasses) << "no frame end comes to a stopped scanout";
    d.donePass();
}

// One deadline while anything is out: armed once, re-armed at its fire while frame ends come, and a panic when one
// fires with a flip out and no frame end since (DC A's interrupt lost), the completion's flip included.
TEST_F(DisplayPeriod, OneDeadlineThatStillCatchesALostFrameEnd)
{
    kick();
    ASSERT_EQ(d.offer(kSlot), 0);
    d.complete(0);
    ASSERT_TRUE(d.flipDeadlineArmed);
    frameEnd();
    const unsigned arms = d.flipTimerStore.arms;
    d.checkFlipDeadline();
    EXPECT_EQ(d.flipTimerStore.arms, arms) << "nothing out: not armed again";
    kick();
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    d.complete(0);
    EXPECT_EQ(d.flipTimerStore.arms, arms + 1) << "armed at the offer";
    d.frameEndInterrupt(false);
    frameEndAction();
    ASSERT_NE(d.ring.pending, Display::kNoBuffer) << "a frame end whose boundary did not take the flip";
    d.checkFlipDeadline();
    EXPECT_EQ(d.flipTimerStore.arms, arms + 2) << "frame ends came: armed again";
    EXPECT_THROW(d.checkFlipDeadline(), std::runtime_error) << "a flip out and no frame end since the last look";
}

// The frame end's stamp, which Core Animation's frame clock is phased from, is the interrupt's time: TegraPanel's DSI
// step and the colour unit's writes come after it.
TEST_F(DisplayPeriod, TheFrameEndIsStampedBeforeThePanelStep)
{
    d.model.boundary();
    const uint64_t before = gClock;
    gPanelStepTicks = 9600;
    d.frameEndInterrupt(false);
    gPanelStepTicks = 0;
    EXPECT_LT(d.feStamp - before, 9600u);
    EXPECT_GE(gClock - before, 9600u);
}

// Core Animation's render thread wakes on its own timer at the frame end, before the action may run: the frame
// timeline's stand-in vsync is the filter's stamp, published by the filter, while no vsync client is armed.
TEST_F(DisplayPeriod, TheFilterPublishesTheTimelineVsync)
{
    d.stats = true;
    d.timelineVsyncs.clear();
    d.frameEndInterrupt(false);
    ASSERT_EQ(d.timelineVsyncs.size(), 1u) << "published before the frame-end action";
    EXPECT_EQ(d.timelineVsyncs[0].first, d.feStamp);
    EXPECT_EQ(d.timelineVsyncs[0].second, d.refreshTicks());
    d.vsyncArmed = true;
    d.frameEndInterrupt(false);
    EXPECT_EQ(d.timelineVsyncs.size(), 1u) << "an armed vsync's V_BLANK is the timeline's";
    d.vsyncArmed = false;
}

// The DC A filter runs the colour unit's and TegraPanel's steps with interrupts off: no simple lock, mutex, sleep, log or
// allocation, with a change of the colour unit's tables pending or not.
TEST_F(DisplayPeriod, TheFrameEndFilterTakesNoLock)
{
    EXPECT_NO_THROW(d.frameEndInterrupt());
    frameEndAction();
    uint32_t contrast = 0x3fc00000;
    ASSERT_EQ(colour.setContrast(&contrast), kIOReturnSuccess);
    for (int i = 0; i < 4; ++i) {
        d.model.boundary();
        EXPECT_NO_THROW(d.frameEndInterrupt()) << "frame end " << i;
        frameEndAction();
    }
}

TEST_F(DisplayPeriod, RepeatedEdrFactorTakesNoLock)
{
    colour.setEdrFactor(1.0f);
    const unsigned takesBefore = colour.lockTakes();
    for (int i = 0; i < 8; ++i)
        colour.setEdrFactor(1.0f);
    EXPECT_EQ(colour.lockTakes(), takesBefore);
    colour.setEdrFactor(0.5f);
    EXPECT_EQ(colour.lockTakes(), takesBefore + 1);
    for (int i = 0; i < 8; ++i)
        colour.setEdrFactor(0.5f);
    EXPECT_EQ(colour.lockTakes(), takesBefore + 1);
}

TEST_F(DisplayPeriod, RepeatedColorSettersDeduplicateWithoutRebuildsOrHolds)
{
    const unsigned holdsStart = gHolds;
    const uint32_t genStart = colour.generation();

    ASSERT_EQ(colour.setBrightnessCorrection(0x8000u), kIOReturnSuccess);
    EXPECT_EQ(colour.generation(), genStart + 1);
    const unsigned takesAfterCorr = colour.lockTakes();
    const unsigned holdsAfterCorr = gHolds;
    for (int i = 0; i < 8; ++i)
        ASSERT_EQ(colour.setBrightnessCorrection(0x8000u), kIOReturnSuccess);
    EXPECT_EQ(colour.lockTakes(), takesAfterCorr);
    EXPECT_EQ(gHolds, holdsAfterCorr);
    EXPECT_EQ(colour.generation(), genStart + 1);

    ASSERT_EQ(colour.setAccessibilityScale(0xc000u), kIOReturnSuccess);
    const unsigned takesAfterScale = colour.lockTakes();
    const unsigned holdsAfterScale = gHolds;
    for (int i = 0; i < 8; ++i)
        ASSERT_EQ(colour.setAccessibilityScale(0xc000u), kIOReturnSuccess);
    EXPECT_EQ(colour.lockTakes(), takesAfterScale);
    EXPECT_EQ(gHolds, holdsAfterScale);

    switch_color::Matrix warm = switch_color::kIdentity;
    warm.m[2][2] = switch_color::kOne / 2;
    ASSERT_EQ(colour.setMatrix(9, &warm), kIOReturnSuccess);
    const uint32_t genAfterMat = colour.generation();
    const unsigned holdsAfterMat = gHolds;
    for (int i = 0; i < 4; ++i)
        ASSERT_EQ(colour.setMatrix(9, &warm), kIOReturnSuccess);
    EXPECT_EQ(colour.generation(), genAfterMat);
    EXPECT_EQ(gHolds, holdsAfterMat);

    switch_color::Q32 swapMat[15] = {};
    memcpy(&swapMat[3], &warm, sizeof(warm));
    ASSERT_EQ(colour.swapSetColorMatrix(swapMat, 2, 0), kIOReturnSuccess);
    const uint32_t genAfterSwapMat = colour.generation();
    const unsigned holdsAfterSwapMat = gHolds;
    for (int i = 0; i < 4; ++i)
        ASSERT_EQ(colour.swapSetColorMatrix(swapMat, 2, 0), kIOReturnSuccess);
    EXPECT_EQ(colour.generation(), genAfterSwapMat);
    EXPECT_EQ(gHolds, holdsAfterSwapMat);

    for (int i = 0; i < 8; ++i)
        frameEnd();
    EXPECT_EQ(gHolds, holdsStart);
}

}  // namespace
}  // namespace completion_flip_test
