// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchCompletionFlipDisplay.h's filter writes: the GPU's nonstall filter and host1x's syncpoint filter write window
// A's armed flip before its completion is delivered; the completion then writes nothing and its pass enters the ring
// as before, and a frame end or SC7 that comes between adopts the flip into the ring alone, its swap retired by the
// pass (switch_completion_flip_harness.h has the model).
#include <gtest/gtest.h>

#include <cstdint>
#include <string>

#include "bazel/testing/source_text.h"
#include "switch_completion_flip_harness.h"

namespace completion_flip_test {
namespace {

constexpr int kSlot = Display::kSlot0;
constexpr uint32_t kWinAStart = 0xbc0, kWinAKind = 0xbcb, kWinAHOffset = 0xbc6, kWinAStride = 0xb8a;
const std::vector<uint32_t> kSixStores = {
    kWinAKind, kWinAStride, kWinAHOffset, kWinAStart, Display::kDcStateControl, Display::kDcStateControl};

class CompletionFlipFilter : public testing::Test {
protected:
    Display d;

    void SetUp() override
    {
        gOffer = nullptr;
        gWaiterFn = nullptr;
        gWaitResult = 0;
        gVicStatus = 1;
        gWaits = 0;
        gLog.clear();
        d.completionFlipsProven = true;
        d.startCompletionFlip();
    }

    void frame()
    {
        d.boundary();
        d.tick();
    }
};

TEST_F(CompletionFlipFilter, TheGpuFilterWritesTheFlipAndItsCompletionWritesNothing)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.model.written.clear();
    d.gpuFilter();
    EXPECT_EQ(d.model.written, kSixStores);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kEarly);
    EXPECT_EQ(d.completionFlip.writer, SwitchCompletionFlip::kFromGpuFilter);
    d.model.written.clear();
    d.gpuFilter();
    d.complete(0);
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kWritten);
    d.donePass();
    EXPECT_EQ(d.ring.pending, kSlot);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    frame();
    EXPECT_EQ(d.ring.front, kSlot);
    EXPECT_EQ(d.retired, std::vector<uint64_t>{d.flipRequest});
}

// The boundary takes the filter's flip and the frame end runs before the GPU's work loop delivers it: the ring has it
// for the latch read; the swap waits for its completion's pass, which retires it and enters nothing.
TEST_F(CompletionFlipFilter, AFrameEndBeforeTheCompletionAdoptsTheFlipAndThePassRetiresItsSwap)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.gpuFilter();
    d.boundary();
    const uint64_t stamp = d.feStamp;
    d.tick();
    ASSERT_EQ(d.shown.size(), 1u);
    EXPECT_EQ(d.shown[0], std::make_pair(kSlot, stamp));
    EXPECT_EQ(d.ring.front, kSlot);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kAdopted);
    EXPECT_TRUE(d.retired.empty()) << "its completion is still out";
    EXPECT_TRUE(d.transferred);
    d.model.written.clear();
    d.complete(0);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kAdopted);
    d.donePass();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    EXPECT_EQ(d.retired, std::vector<uint64_t>{d.flipRequest});
    EXPECT_FALSE(d.transferred);
    EXPECT_EQ(d.ring.front, kSlot);
}

// A frame end adopts a copy's flip; the copy's pass finds its buffer no longer the ring's target and flips nothing.
TEST_F(CompletionFlipFilter, AnAdoptedCopyIsNotFlippedAgainByItsPass)
{
    ASSERT_EQ(d.offerCopy(), 0);
    const int target = d.ring.drawing;
    d.armWaiter(7);
    EXPECT_EQ(gWaits, 1u);
    d.vicFilter();
    EXPECT_EQ(d.completionFlip.writer, SwitchCompletionFlip::kFromVicFilter);
    EXPECT_EQ(gWaiterToken.value, 7u);
    frame();
    EXPECT_EQ(d.ring.front, target);
    EXPECT_EQ(d.ring.drawing, Display::kNoBuffer);
    d.complete(0);
    d.model.written.clear();
    d.donePass();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.ring.front, target);
    EXPECT_EQ(d.ring.pending, Display::kNoBuffer);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
}

TEST_F(CompletionFlipFilter, AVicJobNotCertainlyDoneIsLeftToItsCompletion)
{
    ASSERT_EQ(d.offerCopy(), 0);
    gVicStatus = 0;
    d.armWaiter(9);
    d.model.written.clear();
    d.vicFilter();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kArmed);
    d.complete(0);
    EXPECT_EQ(d.model.written, kSixStores);
}

TEST_F(CompletionFlipFilter, AJobAlreadyDoneOrAFlipNotArmedRegistersNoWaiter)
{
    ASSERT_EQ(d.offerCopy(), 0);
    gWaitResult = SWITCH_HOST1X_FILTER_PASSED;
    d.armWaiter(3);
    EXPECT_EQ(gWaiterFn, nullptr);
    d.complete(0);
    d.donePass();
    gWaitResult = 0;
    d.armWaiter(4);
    EXPECT_EQ(gWaits, 1u) << "nothing armed: no waiter";
}

// A waiter left from an earlier offer (a pass that beat its filter) claims nothing of the next offer's flip.
TEST_F(CompletionFlipFilter, AStaleWaiterClaimsNothingOfTheNextOffer)
{
    ASSERT_EQ(d.offerCopy(), 0);
    d.armWaiter(5);
    auto *stale = gWaiterFn;
    void *staleContext = gWaiterContext;
    d.complete(0);
    d.donePass();
    frame();
    ASSERT_EQ(d.offer(kSlot), 0);
    ASSERT_EQ(d.completionFlip.state, SwitchCompletionFlip::kArmed);
    d.model.written.clear();
    stale(staleContext);
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kArmed);
}

// SC7 between the filter's write and its completion: the flip is in the ring before the controller is saved, and the
// completion's deferred pass enters nothing and retires the swap.
TEST_F(CompletionFlipFilter, SuspendAdoptsAnEarlyFlipAndTheDeferredPassRetiresIt)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.gpuFilter();
    d.suspend();
    EXPECT_EQ(d.ring.front, kSlot);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kAdopted);
    EXPECT_NO_THROW(d.complete(0));
    d.donePass();
    EXPECT_TRUE(d.deferred);
    d.resume();
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    EXPECT_EQ(d.retired, std::vector<uint64_t>{d.flipRequest});
    frame();
    EXPECT_EQ(d.ring.front, kSlot);
}

// gpuComplete stores gpuDonePending, then claims: a pass in between finds the filter's flip and enters it.
TEST_F(CompletionFlipFilter, APassBetweenTheCompletionsStoresAndItsClaimEntersTheEarlyFlip)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.gpuFilter();
    d.gpuStatus = 0;
    d.gpuDonePending = true;
    d.donePass();
    EXPECT_EQ(d.ring.pending, kSlot);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    d.model.written.clear();
    d.writeCompletionFlip(0);
    EXPECT_TRUE(d.model.written.empty());
}

// The producers' contract broken: a flip their filter wrote whose completion then failed is shown as done, logged once,
// and no filter writes a flip after it.
TEST_F(CompletionFlipFilter, AFilterFlipWhoseCompletionFailsIsShownAndTurnsTheFiltersOff)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.gpuFilter();
    d.complete(-5);
    EXPECT_EQ(d.gpuStatus, 0);
    ASSERT_EQ(gLog.size(), 1u);
    EXPECT_NE(gLog[0].find("whose completion then failed (-5)"), std::string::npos) << gLog[0];
    d.donePass();
    EXPECT_EQ(d.ring.pending, kSlot);
    frame();
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    d.model.written.clear();
    d.gpuFilter();
    EXPECT_TRUE(d.model.written.empty());
    d.complete(0);
    EXPECT_EQ(d.model.written, kSixStores);
}

TEST_F(CompletionFlipFilter, NoFilterWritesAWithdrawnOrUnarmedFlip)
{
    d.model.written.clear();
    d.gpuFilter();
    gOffer = [](const TegraScanoutCopy *) {
        return -16;
    };
    EXPECT_EQ(d.offer(kSlot), -16);
    d.gpuFilter();
    EXPECT_TRUE(d.model.written.empty());
}

// The display's hook sites for the filters: gpuComplete takes the verdict first, a copy's flip() asks before it takes
// the ring's target, and VIC's copy thread registers its waiter once the job's fence is armed.
TEST(CompletionFlipFilterSource, TheDisplayAsksWhereTheFiltersNeedIt)
{
    using switch_ios::test::Between;
    using switch_ios::test::SourceText;
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    EXPECT_EQ(Between(display, "    void gpuComplete(int status)\n    {\n", ";").find("status = completionFlipVerdict"),
              8u);
    EXPECT_EQ(Between(display, "    void flip()\n    {\n", ")").find("if (flippedByCompletion("), 8u);
    EXPECT_NE(display.find("#include \"../TegraHost1x/SwitchHost1xFence.h\""), std::string::npos);
    const std::string vic = Between(SourceText("../SwitchVICScanoutDisplay.h"), "bool vicCopyWork()", "\n}\n");
    EXPECT_LT(vic.find("switch_host1x_token_fence("), vic.find("armCompletionFlipWaiter(job.output.view.producer);"));
}

// T-v4's panic: offerGpuCopy into the boot framebuffer (entry 0) armed its flip with surface 0's address read under
// completionFlipLock, through IOMemoryMap::getPhysicalAddress and its IORecursiveLock. The arm reads it before the lock
// now, and surface 0's address is a stored word; nothing under the lock may block (the harness's stubs throw).
TEST_F(CompletionFlipFilter, ACopyIntoTheBootFramebufferArmsWithNothingThatBlocks)
{
    for (int i = 0; i < 4 && d.ring.target() != 0; ++i) {
        ASSERT_EQ(d.offerCopy(), 0);
        d.complete(0);
        d.donePass();
        frame();
    }
    ASSERT_EQ(d.ring.target(), 0);
    int offered = -1;
    ASSERT_NO_THROW(offered = d.offerCopy());
    ASSERT_EQ(offered, 0);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kArmed);
    EXPECT_EQ(d.completionFlip.entry, 0);
    EXPECT_EQ(d.completionFlip.start, Display::kBootFramebuffer);
    ASSERT_NO_THROW(d.complete(0));
    EXPECT_EQ(d.model.armed[Display::kWinbufStartAddr], Display::kBootFramebuffer);
    d.donePass();
    frame();
    EXPECT_EQ(d.ring.front, 0);
    EXPECT_EQ(gPreemptionOff, 0);
}

// Each producer's filter writes the boot framebuffer's flip with preemption off and nothing that blocks.
TEST_F(CompletionFlipFilter, EachFilterWritesTheBootFramebufferWithNothingThatBlocks)
{
    for (int i = 0; i < 4 && d.ring.target() != 0; ++i) {
        ASSERT_EQ(d.offerCopy(), 0);
        d.complete(0);
        d.donePass();
        frame();
    }
    ASSERT_EQ(d.ring.target(), 0);
    ASSERT_EQ(d.offerCopy(), 0);
    d.armWaiter(11);
    d.model.written.clear();
    ASSERT_NO_THROW(d.vicFilter());
    EXPECT_EQ(d.model.written, kSixStores);
    EXPECT_EQ(d.model.assembly[Display::kWinbufStartAddr], Display::kBootFramebuffer);
    d.complete(0);
    d.donePass();
    frame();
    ASSERT_EQ(d.offer(kSlot), 0);
    d.model.written.clear();
    ASSERT_NO_THROW(d.gpuFilter());
    EXPECT_EQ(d.model.written, kSixStores);
    EXPECT_EQ(gPreemptionOff, 0);
}

// What the harness's stubs stand for panics on the device with preemption off: each throws here, and not without it.
TEST(CompletionFlipHarness, CatchesWhatPanicsWithPreemptionOff)
{
    Display d;
    IOSimpleLock lock;
    const IOInterruptState state = IOSimpleLockLockDisableInterrupt(&lock);
    EXPECT_THROW(d.bootMapping->getPhysicalAddress(), std::runtime_error);
    uint64_t length = 0;
    EXPECT_THROW(d.bootMapping->getPhysicalSegment(0, &length), std::runtime_error);
    IOLock mutex;
    EXPECT_THROW(IOLockLock(&mutex), std::runtime_error);
    IORecursiveLock recursive;
    EXPECT_THROW(IORecursiveLockLock(&recursive), std::runtime_error);
    EXPECT_THROW(IOMalloc(16), std::runtime_error);
    IOCommandGate gate;
    EXPECT_THROW(gate.runAction(nullptr), std::runtime_error);
    EXPECT_THROW(IOLog("SwitchDisplay: a line"), std::runtime_error);
    IOSimpleLockUnlockEnableInterrupt(&lock, state);
    EXPECT_NO_THROW(d.bootMapping->getPhysicalAddress());
    gLog.clear();
}

}  // namespace
}  // namespace completion_flip_test
