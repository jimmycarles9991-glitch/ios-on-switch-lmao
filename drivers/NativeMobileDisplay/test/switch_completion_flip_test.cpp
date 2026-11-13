// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchCompletionFlipDisplay.h: a direct frame's completion writes window A's flip itself, through window A's own
// aperture and nothing the gate's reads switch, and the gate's ring, latch check and SC7 path see that flip at the
// boundary that took it, whatever order the completion, its own pass, the frame boundaries and the frame-end action
// run in (switch_completion_flip_harness.h has the model). Window A's aperture is taken as proven here, as it is
// after the first completion flip latched as written; switch_completion_flip_proof_test has the unit that refuses.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>

#include "bazel/testing/source_text.h"
#include "switch_completion_flip_harness.h"
#include "switch_completion_flip_walk.h"

namespace completion_flip_test {
namespace {

using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

constexpr int kSlot = Display::kSlot0;
constexpr uint32_t kWinAStart = 0xbc0, kWinAKind = 0xbcb, kWinAHOffset = 0xbc6, kWinAStride = 0xb8a;

class CompletionFlip : public testing::Test {
protected:
    Display d;

    void SetUp() override
    {
        gOffer = nullptr;
        gLog.clear();
        d.completionFlipsProven = true;
    }

    void TearDown() override
    {
        gOffer = nullptr;
    }

    void frame()
    {
        d.boundary();
        d.tick();
    }
};

TEST_F(CompletionFlip, TheCompletionWritesWindowAThroughItsOwnApertureOnly)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kArmed);
    const unsigned selectors = d.model.selectorWrites;
    d.model.written.clear();
    d.complete(0);
    const std::vector<uint32_t> want = {
        kWinAKind, kWinAStride, kWinAHOffset, kWinAStart, Display::kDcStateControl, Display::kDcStateControl};
    EXPECT_EQ(d.model.written, want);
    EXPECT_EQ(d.model.selectorWrites, selectors);
    EXPECT_EQ(d.model.assembly[Display::kWinbufStartAddr], d.surfaceAddress(kSlot));
    EXPECT_EQ(d.model.armed[Display::kWinbufStartAddr], d.surfaceAddress(kSlot));
    EXPECT_EQ(d.model.armed[Display::kWinLineStride], 2944u);
    EXPECT_EQ(d.model.requested, Display::kGeneralActReq | Display::kWinAActReq);
    EXPECT_EQ(d.completionFlip.requestedAt, d.frameEnds);
    EXPECT_EQ(d.ring.pending, Display::kNoBuffer);

    d.model.written.clear();
    d.donePass();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.ring.pending, kSlot);
    EXPECT_EQ(d.flipRequested, d.completionFlip.requestedAt);
    EXPECT_EQ(d.marks[kSlot].ready, d.completionFlip.writtenAt);
    EXPECT_EQ(d.entrySeq[kSlot], d.flipRequest);
    EXPECT_EQ(d.deadlineArms, 1u);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    EXPECT_FALSE(d.transferred);

    frame();
    ASSERT_EQ(d.shown.size(), 1u);
    EXPECT_EQ(d.shown[0].first, kSlot);
    EXPECT_EQ(d.ring.front, kSlot);
}

TEST_F(CompletionFlip, AFrameEndBeforeTheCompletionsPassRunsThePassFirst)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.complete(0);
    d.boundary();
    EXPECT_EQ(d.activeEntry(), kSlot);
    const uint64_t stamp = d.feStamp;
    d.tick();
    ASSERT_EQ(d.shown.size(), 1u);
    EXPECT_EQ(d.shown[0], std::make_pair(kSlot, stamp));
    EXPECT_EQ(d.ring.front, kSlot);
    EXPECT_FALSE(d.transferred);
    d.model.written.clear();
    d.donePass();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.ring.front, kSlot);
}

// The completion lands between the frame-end action's first step and its latch read, and a boundary takes it before
// the read: the read sees the flip the ring has.
TEST_F(CompletionFlip, ACompletionInsideTheFrameEndActionIsInTheRingBeforeTheLatchRead)
{
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    d.complete(0);
    d.donePass();
    ASSERT_EQ(d.ring.pending, kSlot + 1);
    ASSERT_EQ(d.offer(kSlot), 0);
    d.midTick = [this] {
        d.complete(0);
        d.boundary();
        d.midTick = nullptr;
    };
    d.boundary();
    d.tick();
    EXPECT_EQ(d.activeEntry(), kSlot);
    EXPECT_EQ(d.ring.front, kSlot);
    ASSERT_FALSE(d.shown.empty());
    EXPECT_EQ(d.shown.back().first, kSlot);
}

// A GPU or VIC copy into a scan buffer: its completion writes window A to the buffer it copied into, and the pass's
// flip() finds that buffer as the ring's target and puts it in the ring.
TEST_F(CompletionFlip, ACopyIntoAScanBufferIsFlippedByItsCompletion)
{
    ASSERT_EQ(d.offerCopy(), 0);
    const int target = d.ring.drawing;
    ASSERT_NE(target, Display::kNoBuffer);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kArmed);
    d.complete(0);
    EXPECT_EQ(d.model.armed[Display::kWinbufStartAddr], d.surfaceAddress(target));
    EXPECT_EQ(d.model.armed[Display::kWinbufSurfaceKind], Display::kBlockSurfaceKind);
    d.donePass();
    EXPECT_EQ(d.ring.pending, target);
    EXPECT_EQ(d.ring.drawing, Display::kNoBuffer);
    frame();
    EXPECT_EQ(d.ring.front, target);
}

TEST_F(CompletionFlip, AFailedFrameWritesNothingAndFlipsNothing)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.model.written.clear();
    d.complete(-5);
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    d.donePass();
    EXPECT_EQ(d.ring.pending, Display::kNoBuffer);
    EXPECT_FALSE(d.transferred);
}

TEST_F(CompletionFlip, ARefusedOfferWithdrawsItsFlip)
{
    gOffer = [](const TegraScanoutCopy *) {
        return -16;
    };
    EXPECT_EQ(d.offer(kSlot), -16);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    d.model.written.clear();
    d.complete(0);
    EXPECT_TRUE(d.model.written.empty());
}

// The completion may run on another CPU inside the offer call, before the offer's own bookkeeping.
TEST_F(CompletionFlip, ACompletionBeforeTheOfferReturnsStillFlips)
{
    gOffer = [this](const TegraScanoutCopy *) {
        d.complete(0);
        return 0;
    };
    ASSERT_EQ(d.offer(kSlot), 0);
    EXPECT_EQ(d.model.armed[Display::kWinbufStartAddr], d.surfaceAddress(kSlot));
    d.donePass();
    EXPECT_EQ(d.ring.pending, kSlot);
    EXPECT_EQ(d.entrySeq[kSlot], d.flipRequest);
    frame();
    EXPECT_EQ(d.ring.front, kSlot);
}

TEST_F(CompletionFlip, AGateWriteOfWindowAWithdrawsAnArmedFlip)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.withdrawCompletionFlip();
    d.model.written.clear();
    d.complete(0);
    EXPECT_TRUE(d.model.written.empty());
    d.donePass();
    EXPECT_EQ(d.ring.pending, kSlot);
    EXPECT_EQ(d.model.assembly[Display::kWinbufStartAddr], d.surfaceAddress(kSlot));
    EXPECT_GT(d.model.selectorWrites, 0u);
}

// The gate's own flip takes window A from an armed completion before it writes a register: a completion landing
// at the gate's first write writes nothing.
TEST_F(CompletionFlip, AGateFlipWithdrawsAnArmedFlipBeforeItsFirstWrite)
{
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    size_t completionWrites = 1;
    d.model.beforeWrite = [this, &completionWrites] {
        const size_t before = d.model.written.size();
        d.complete(0);
        completionWrites = d.model.written.size() - before;
    };
    d.writeFlip(kSlot);
    EXPECT_EQ(completionWrites, 0u);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    EXPECT_EQ(d.model.assembly[Display::kWinbufStartAddr], d.surfaceAddress(kSlot));
}

TEST_F(CompletionFlip, NotArmedWhenTheFrameWouldBeHeldOrCarriesWindowB)
{
    ASSERT_EQ(d.offer(kSlot + 3), 0);
    d.complete(0);
    d.donePass();
    ASSERT_EQ(d.ring.pending, kSlot + 3);
    ASSERT_EQ(d.offer(kSlot), 0);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle) << "a pitch frame replacing a tiled one";
    d.complete(0);
    d.donePass();
    EXPECT_EQ(d.ring.held, kSlot);
    frame();
    EXPECT_EQ(d.ring.pending, kSlot);
    frame();
    EXPECT_EQ(d.ring.front, kSlot);

    d.planeNextSet = true;
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
}

// SC7: a written flip goes in the ring before the controller is saved, an armed one is withdrawn, and nothing writes
// the unclocked controller.
TEST_F(CompletionFlip, SuspendPutsAWrittenFlipInTheRingAndWithdrawsAnArmedOne)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    d.complete(0);
    d.suspend();
    EXPECT_EQ(d.ring.front, kSlot);
    EXPECT_FALSE(d.transferred);
    d.resume();

    ASSERT_EQ(d.offer(kSlot + 1), 0);
    d.suspend();
    EXPECT_EQ(d.completionFlip.state, SwitchCompletionFlip::kIdle);
    EXPECT_NO_THROW(d.complete(0));
    d.donePass();
    EXPECT_TRUE(d.deferred);
    d.resume();
    EXPECT_EQ(d.ring.pending, kSlot + 1);
    frame();
    EXPECT_EQ(d.ring.front, kSlot + 1);
}

// quiesceForSuspend reads what SC7 will take as shown before planesQuiesce and ring.settle, and retires through it once
// hwSuspended holds any kick it makes for the resume.
TEST(CompletionFlipSource, SuspendRetiresWhatItsSettlesTakeAsShown)
{
    const CodeText quiesce = Block(SourceText("../NativeLegacyDisplay.cpp"), "void quiesceForSuspend()");
    EXPECT_TRUE(quiesce.Before("const uint64_t shown = suspendShownSeq();", "planesQuiesce();"));
    EXPECT_TRUE(quiesce.Before("ring.settle();", "hwSuspended = true;"));
    EXPECT_TRUE(quiesce.Before("hwSuspended = true;", "retireSuspendShown(shown);"));
}

// SC7 takes window A's pending flip as shown: its swap completes at the settle, with nothing presented after the
// resume, and a present after it completes nothing of it again.
TEST_F(CompletionFlip, AFlipSettledAtSC7CompletesThereWithNothingPresentedAfter)
{
    ASSERT_EQ(d.offer(kSlot), 0);
    const uint64_t mine = d.flipRequest;
    d.complete(0);
    d.donePass();
    ASSERT_EQ(d.ring.pending, kSlot);
    d.suspend();
    EXPECT_EQ(d.retired, std::vector<uint64_t>{mine});
    d.resume();
    frame();
    frame();
    EXPECT_EQ(d.retired, std::vector<uint64_t>{mine});
    ASSERT_EQ(d.offer(kSlot + 1), 0);
    d.complete(0);
    d.donePass();
    frame();
    EXPECT_EQ(std::count(d.retired.begin(), d.retired.end(), mine), 1);
    EXPECT_EQ(d.retired.back(), d.flipRequest);
}

// The ring sees each latch at the first frame end after the boundary that took it, whichever of a producer's filter
// and its completion wrote the flip.
TEST_F(CompletionFlip, RandomOrdersNeverLoseOrDelayALatch)
{
    Walk walk;
    randomWalk(d, 200000, walk);
    EXPECT_GT(walk.completed, 8000u) << walk.completed;
    EXPECT_GT(walk.fromCompletions, 2000u) << walk.fromCompletions;
    EXPECT_GT(walk.fromFilters, 2000u) << walk.fromFilters;
    EXPECT_GT(d.shown.size(), 5000u) << d.shown.size();
    EXPECT_FALSE(d.completionFilterOff);
    EXPECT_TRUE(gLog.empty());
}

// The display's own hook sites: the completion writes after gpuDonePending (so a gate that sees the flip written can
// run the pass), the frame-end action and SC7 take a written flip first, the latch read is the locked one, the offer
// is armed before the GPU driver sees it, and FRAME_END stays routed while a flip is out.
TEST(CompletionFlipSource, TheDisplayCallsItWhereTheOrderingNeedsIt)
{
    const std::string display = SourceText("../NativeLegacyDisplay.cpp");
    const CodeText complete = Block(display, "void gpuComplete(int status)");
    EXPECT_TRUE(
        complete.Before("__atomic_store_n(&gpuDonePending, true, __ATOMIC_RELEASE);", "writeCompletionFlip(status);"));
    EXPECT_TRUE(complete.Before("writeCompletionFlip(status);", "gpuDoneSource->interruptOccurred("));
    EXPECT_TRUE(Block(display, "void vblankTick()").StartsWith("takeCompletionFlip();"));
    EXPECT_TRUE(Block(display, "void quiesceForSuspend()").StartsWith("suspendCompletionFlip();"));
    EXPECT_TRUE(Block(display, "void stopRotation()").StartsWith("withdrawCompletionFlip();"));
    EXPECT_TRUE(Block(display, "void settleStopped()").StartsWith("flipUnverifiedFromGate();"));
    const CodeText check = Block(display, "void checkFlip(bool atFrameEnd)");
    EXPECT_TRUE(check.Has("const auto [control, start] = latchState();"));
    EXPECT_FALSE(check.Has("windowActive("));
    EXPECT_TRUE(Block(display, "bool frameEndNeeded()").Has("completionFlipOut()"));
    EXPECT_TRUE(Block(display, "bool offerDirect(").Has("offerWithCompletionFlip(copy, entry)"));
    EXPECT_TRUE(Block(display, "bool offerGpuCopy(").Has("offerWithCompletionFlip(copy, target)"));
    const CodeText vic = Block(SourceText("../SwitchVICScanoutDisplay.h"), "bool offerVicCopy(");
    EXPECT_TRUE(vic.Before("ring.claim(target);", "armCompletionFlip(target);"));
    EXPECT_TRUE(vic.Before("armCompletionFlip(target);", "copySource->interruptOccurred("));
    const std::string start = SourceText("../SwitchScanoutStart.h");
    EXPECT_TRUE(Block(start, "void presentEntry(int i)").StartsWith("if (presentedByCompletion(i))"));
    EXPECT_TRUE(Block(SourceText("../SwitchOverlayRegisters.h"), "void planeBlendA(").Has("withdrawCompletionFlip();"));
    EXPECT_TRUE(
        Block(SourceText("../SwitchPaletteDisplay.h"), "void paletteFrameEnd(").Has("withdrawCompletionFlip();"));
}

// Window A's other writers on the gate (the column scan, the window probes, the landscape change and the turned scan
// on window A) withdraw an armed flip before they write its assembly copy, and none arms while they own window A.
TEST(CompletionFlipSource, WindowAsOtherWritersWithdrawFirst)
{
    const std::string scan = SourceText("../NativeColumnScanDisplay.h");
    const std::string probe = SourceText("../DisplayWindowProbeDisplay.h");
    const std::string landscape = SourceText("../SwitchLandscapeDisplay.h");
    const auto withdrawsFirst = [](const std::string &source, const char *function) {
        return Block(source, function).StartsWith("withdrawCompletionFlip();");
    };
    EXPECT_TRUE(withdrawsFirst(scan, "void columnScanShow("));
    EXPECT_TRUE(withdrawsFirst(scan, "void columnScanRestoreWindow("));
    EXPECT_TRUE(withdrawsFirst(probe, "void windowProbeShow("));
    EXPECT_TRUE(withdrawsFirst(probe, "void windowProbeRestoreWindows("));
    EXPECT_TRUE(withdrawsFirst(landscape, "void landscapeMoveUi("));
    EXPECT_TRUE(Block(landscape, "void landscapeLeaveNow(").Before("withdrawCompletionFlip();", "landscapeMoveUi("));
    EXPECT_TRUE(Block(SourceText("../SwitchOverlayRegisters.h"), "void planeWriteWindowRegisters(")
                    .Has("if (w == switch_landscape::kA) withdrawCompletionFlip();"));
    const std::string flips = SourceText("../SwitchCompletionFlipDisplay.h");
    const CodeText may = Block(flips, "bool completionFlipMay(");
    for (const char *owner : {"columnScanBusy()", "windowProbeBusy()", "landscapeOn()", "landscapeHoldsKicks()"})
        EXPECT_TRUE(may.Has(owner));
    EXPECT_FALSE(Block(flips, "LatchState latchState()").Has("windowActive("));
}

}  // namespace
}  // namespace completion_flip_test
