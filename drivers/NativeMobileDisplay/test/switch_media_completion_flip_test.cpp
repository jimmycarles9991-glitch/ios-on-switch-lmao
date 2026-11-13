// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchMediaCompletionFlipDisplay.h: a media plane set is decided and staged at its offer, the producers' completion
// writes its surface addresses and request, and the latch sees it at the frame end after the boundary that took it,
// in any order of completion, pass, boundary, frame end, gate write and SC7; a staged set is never taken before its
// request, and no boundary ever scans a surface whose producer has not finished
// (switch_media_completion_flip_harness.h has the model).
#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>

#include "bazel/testing/source_text.h"
#include "switch_media_completion_flip_harness.h"

namespace media_flip_test {
namespace {

constexpr uint32_t kStartB = Display::kWinbufBBase + native_planes::kStartAddr;
constexpr uint32_t kStartUB = Display::kWinbufBBase + native_planes::kStartAddrU;

class MediaFlip : public testing::Test {
protected:
    Display d;
    uint64_t seq = 0;

    void SetUp() override
    {
        gLog.clear();
        gFenceResult = 0;
        gFences = 0;
    }

    void frame()
    {
        d.model.boundary();
        d.frameEnd();
    }

    uint32_t address(int slot, unsigned plane = 0) const
    {
        const Display::PlaneSet p = video(slot);
        return d.windowAddress(p.window[0], plane);
    }

    // A set out through the gate (a new layout is never armed), its swap retired.
    void shownByGate(const Display::PlaneSet &p)
    {
        ASSERT_TRUE(d.offer(p, ++seq));
        ASSERT_TRUE(d.mediaFlipIdle());
        d.complete(0);
        d.donePass();
        frame();
        ASSERT_EQ(d.shown.back(), seq);
    }

    bool armed(int slot)
    {
        return d.offer(video(slot), ++seq) && d.mediaFlip.state == SwitchCompletionFlip::kArmed;
    }
};

TEST_F(MediaFlip, TheCompletionWritesTheNewAddressesAndTheRequestOnly)
{
    shownByGate(video(1));
    ASSERT_TRUE(armed(2));
    EXPECT_FALSE(d.planeLatch.armedRequested);
    EXPECT_FALSE(d.planeLatch.requested());
    EXPECT_EQ(d.model.assembly[kStartB], address(1)) << "the staged copy keeps the scanned surface";
    EXPECT_EQ(d.model.assembly[kStartUB], address(1, 1));
    EXPECT_EQ(d.model.requested, 0u);

    d.model.written.clear();
    d.complete(0);
    const std::vector<uint32_t> want = {kStartB, kStartUB, Display::kDcStateControl, Display::kDcStateControl};
    EXPECT_EQ(d.model.written, want);
    EXPECT_EQ(d.model.requested, Display::kGeneralActReq | Display::kWinBActReq | Display::kWinCActReq);

    d.model.written.clear();
    d.donePass();
    EXPECT_TRUE(d.model.written.empty());
    EXPECT_TRUE(d.planeLatch.armedRequested);
    EXPECT_EQ(d.planeRequestedAt, d.mediaFlip.requestedAt);
    frame();
    EXPECT_EQ(d.shown.back(), seq);
    EXPECT_EQ(d.model.active[kStartB], address(2));
    EXPECT_TRUE(gLog.empty());
}

// The same surface again matches the scanned registers word for word: still not taken before its request.
TEST_F(MediaFlip, AStagedSetIsNeverTakenBeforeItsRequest)
{
    shownByGate(video(1));
    for (const int slot : {2, 2}) {
        const size_t shown = d.shown.size();
        const int active = d.planeLatch.active;
        ASSERT_TRUE(armed(slot));
        for (int i = 0; i < 6; ++i)
            ASSERT_NO_THROW(frame());
        EXPECT_EQ(d.shown.size(), shown);
        EXPECT_EQ(d.planeLatch.active, active);
        EXPECT_FALSE(d.planeLatch.armedRequested);
        d.complete(0);
        d.donePass();
        frame();
        EXPECT_EQ(d.shown.back(), seq);
    }
}

TEST_F(MediaFlip, AFrameEndBeforeThePassTakesTheWrittenSetFirst)
{
    shownByGate(video(1));
    ASSERT_TRUE(armed(2));
    d.complete(0);
    frame();
    EXPECT_EQ(d.shown.back(), seq);
    EXPECT_FALSE(d.gpuDonePending);
    d.model.written.clear();
    d.donePass();
    EXPECT_TRUE(d.model.written.empty());
}

TEST_F(MediaFlip, ACompletionInsideTheFrameEndActionIsInTheLatchBeforeItsRead)
{
    shownByGate(video(1));
    ASSERT_TRUE(armed(2));
    d.midFrameEnd = [this] {
        d.complete(0);
        d.model.boundary();
        d.midFrameEnd = nullptr;
    };
    d.frameEnd();
    EXPECT_EQ(d.shown.back(), seq);
}

TEST_F(MediaFlip, AFailedProducerLeavesTheLatchAsItWas)
{
    shownByGate(video(1));
    const int active = d.planeLatch.active;
    ASSERT_TRUE(armed(2));
    d.model.written.clear();
    d.complete(-5);
    EXPECT_TRUE(d.model.written.empty());
    d.donePass();
    EXPECT_EQ(d.planeLatch.armed, Display::PlaneCommits::kNone);
    EXPECT_EQ(d.planeLatch.active, active);
    EXPECT_TRUE(d.planeLatch.armedRequested);
    EXPECT_EQ(d.notShown, 1u);
    EXPECT_EQ(d.model.assembly[kStartB], address(1));
}

TEST_F(MediaFlip, ARefusedFenceWithdrawsItsSet)
{
    shownByGate(video(1));
    gFenceResult = -16;
    EXPECT_FALSE(d.offer(video(2), ++seq));
    EXPECT_TRUE(d.mediaFlipIdle());
    EXPECT_EQ(d.planeLatch.armed, Display::PlaneCommits::kNone);
    EXPECT_TRUE(d.planeLatch.armedRequested);
}

// Any other gate write of windows B and C withdraws an armed set first; that set goes out from the gate after it.
TEST_F(MediaFlip, AGateWriteOfWindowsBAndCWithdrawsAnArmedSet)
{
    shownByGate(video(1));
    ASSERT_TRUE(armed(2));
    const uint64_t armedSeq = seq;
    Display::PlaneSet other = video(3);
    other.seq = armedSeq - 1;
    d.planeFlipOnly(other);
    EXPECT_TRUE(d.mediaFlipIdle());
    d.model.written.clear();
    d.complete(0);
    EXPECT_TRUE(d.model.written.empty());
    d.donePass();
    for (int i = 0; i < 3; ++i)
        ASSERT_NO_THROW(frame());
    EXPECT_EQ(d.shown.back(), armedSeq);
    EXPECT_EQ(d.model.active[kStartB], address(2));
}

TEST_F(MediaFlip, TheRecoveryTakesAWrittenSetIntoTheLatchFirst)
{
    shownByGate(video(1));
    ASSERT_TRUE(armed(2));
    d.complete(0);
    d.beginPlaneRecovery("test");
    EXPECT_FALSE(d.gpuDonePending);
    EXPECT_EQ(d.planeRecoveryTarget.seq, seq);
}

TEST_F(MediaFlip, TheRecoveryWithdrawsAnArmedSetWhichItThenHolds)
{
    shownByGate(video(1));
    const uint64_t first = seq;
    ASSERT_TRUE(armed(2));
    d.beginPlaneRecovery("test");
    EXPECT_TRUE(d.mediaFlipIdle());
    EXPECT_EQ(d.planeRecoveryTarget.seq, first);
    d.complete(0);
    d.donePass();
    EXPECT_EQ(d.planeRecoveryTarget.seq, seq);
}

TEST_F(MediaFlip, SuspendWithdrawsAnArmedSetWhichShowsAfterTheResume)
{
    shownByGate(video(1));
    ASSERT_TRUE(armed(2));
    d.suspend();
    EXPECT_TRUE(d.mediaFlipIdle());
    d.complete(0);
    d.donePass();
    EXPECT_TRUE(d.planeHeldSet);
    d.resume();
    frame();
    EXPECT_EQ(d.model.active[kStartB], address(2));
    EXPECT_EQ(d.shown.back(), seq);
}

TEST_F(MediaFlip, SuspendTakesAWrittenSetIntoTheLatchFirst)
{
    shownByGate(video(1));
    ASSERT_TRUE(armed(2));
    d.complete(0);
    d.suspend();
    EXPECT_FALSE(d.gpuDonePending);
    EXPECT_EQ(d.planeSetOf(d.planeLatch.active).seq, seq);
    d.resume();
    frame();
    EXPECT_EQ(d.model.active[kStartB], address(2));
}

TEST_F(MediaFlip, NotArmedForANewLayout)
{
    shownByGate(video(1));
    shownByGate(video(2, (360u << 16) | 640));
    EXPECT_EQ(d.model.active[Display::kWinBBase + native_planes::kSize], (360u << 16) | 640);
    Display::PlaneSet off = {};
    shownByGate(off);
}

// A completion whose address stores never reach the assembly copy: one line, these flips off, the set from the gate.
TEST_F(MediaFlip, ACompletionThatDoesNotLatchIsWrittenFromTheGate)
{
    shownByGate(video(1));
    ASSERT_TRUE(armed(2));
    d.model.completionDropsAddresses = true;
    d.complete(0);
    d.donePass();
    ASSERT_NO_THROW(frame());
    ASSERT_EQ(gLog.size(), 1u);
    EXPECT_NE(gLog[0].find("SwitchDisplay: windows B and C's completion flip did not latch"), std::string::npos);
    EXPECT_NE(gLog[0].find("they flip from the display's work loop from here on"), std::string::npos);
    EXPECT_TRUE(d.mediaFlipsRefused);
    EXPECT_EQ(d.model.assembly[kStartB], address(2));
    ASSERT_NO_THROW(frame());
    EXPECT_EQ(d.shown.back(), seq);
    EXPECT_EQ(d.model.active[kStartB], address(2));
    EXPECT_FALSE(armed(3));
    EXPECT_EQ(gLog.size(), 1u);
}

// A controller whose every UPDATE and ACT_REQ takes every window: a window A flip promotes the staged copy early, and
// windows B and C still scan the frame already on screen.
TEST_F(MediaFlip, AnEarlyPromotionShowsTheFrameAlreadyOnScreen)
{
    d.model.promote = DcModel::kEveryWindow;
    shownByGate(video(1));
    ASSERT_TRUE(armed(2));
    d.dc[Display::kDcStateControl] = Display::kGeneralUpdate | Display::kWinAUpdate;
    d.dc[Display::kDcStateControl] = Display::kGeneralActReq | Display::kWinAActReq;
    d.model.boundary();
    EXPECT_EQ(d.model.active[kStartB], address(1));
    d.frameEnd();
    d.complete(0);
    d.donePass();
    frame();
    EXPECT_EQ(d.model.active[kStartB], address(2));
    EXPECT_EQ(d.shown.back(), seq);
}

uint64_t gRng = 0x2545f4914f6cdd1dull;

unsigned roll(unsigned n)
{
    gRng ^= gRng << 13;
    gRng ^= gRng >> 7;
    gRng ^= gRng << 17;
    return static_cast<unsigned>(gRng % n);
}

// Offers, host1x filter writes and completions (some failed, some inside the frame-end action), passes, boundaries,
// frame ends, window A flips and SC7 in random order: no latch is ever overdue, every boundary scans a surface whose
// producer finished, and most sets go out by their completion or earlier by the filter, which writes only a set whose
// producers are done and whose completion is a success.
struct Walk {
    unsigned fromCompletions = 0, fromFilters = 0;
};

void randomWalk(Display &d, unsigned steps, Walk &walk)
{
    std::set<uint32_t> finished = {0};
    uint64_t seq = 0;
    bool offered = false, done = false, fails = false, boundarySince = false, secondDone = true;
    Display::PlaneSet pending = {};
    const auto finish = [&] {
        for (unsigned plane = 0; plane != 2; ++plane)
            finished.insert(d.windowAddress(pending.window[0], plane));
    };
    // With two producers the filter may run while the second is still writing: then it must write nothing.
    const auto filter = [&] {
        if (!offered || done || fails)
            return;
        secondDone = secondDone || roll(2);
        gUncertain.clear();
        if (!secondDone)
            gUncertain.insert(seq + Display::kSecond);
        if (secondDone)
            finish();
        d.model.written.clear();
        d.filter();
        walk.fromFilters += !d.model.written.empty();
        if (!secondDone && !d.model.written.empty())
            ADD_FAILURE() << "a set written while its second producer was running";
    };
    const auto complete = [&] {
        if (!offered || done)
            return;
        const int status = fails ? -5 : 0;
        d.model.written.clear();
        d.complete(status);
        walk.fromCompletions += !d.model.written.empty();
        if (!status)
            finish();
        done = true;
    };
    const auto boundary = [&] {
        d.model.boundary();
        boundarySince = true;
        if (d.model.active[Display::kWinBBase + native_planes::kOptions] & native_planes::kWinEnable)
            ASSERT_TRUE(finished.count(d.model.active[kStartB])) << "a surface whose producer is still writing it";
    };
    for (unsigned step = 0; step < steps; ++step) {
        switch (roll(10)) {
        case 0:
        case 1:
            if (!d.transferred && !d.hwSuspended) {
                pending = roll(24) ? video(1 + static_cast<int>(roll(4))) : video(5, (360u << 16) | 640);
                const unsigned producers = roll(4) ? 1 : 2;
                offered = d.offer(pending, ++seq, producers);
                done = false;
                fails = roll(16) == 0;
                secondDone = producers == 1;
            }
            break;
        case 2:
            complete();
            break;
        case 3:
            ASSERT_NO_THROW(d.donePass()) << "step " << step;
            break;
        case 4:
            if (!d.hwSuspended)
                boundary();
            break;
        case 5:
        case 6:
            if (!boundarySince || d.hwSuspended)
                break;
            if (roll(4) == 0)
                d.midFrameEnd = [&] {
                    if (roll(2))
                        filter();
                    else
                        complete();
                    if (roll(2))
                        boundary();
                };
            boundarySince = false;
            ASSERT_NO_THROW(d.frameEnd()) << "step " << step;
            d.midFrameEnd = nullptr;
            break;
        case 7:
            if (!d.hwSuspended) {
                d.dc[Display::kDcStateControl] = Display::kGeneralUpdate | Display::kWinAUpdate;
                d.dc[Display::kDcStateControl] = Display::kGeneralActReq | Display::kWinAActReq;
            }
            break;
        case 8:
            if (d.hwSuspended ? roll(4) == 0 : roll(256) == 0) {
                if (d.hwSuspended)
                    ASSERT_NO_THROW(d.resume()) << "step " << step;
                else
                    ASSERT_NO_THROW(d.suspend()) << "step " << step;
            }
            break;
        case 9:
            filter();
            break;
        }
        if (offered && done && !d.transferred)
            offered = false;
    }
}

class MediaFlipWalk : public testing::TestWithParam<DcModel::Promote> {};

TEST_P(MediaFlipWalk, RandomOrdersNeverScanAnUnfinishedSurface)
{
    gLog.clear();
    gFenceResult = 0;
    Display d;
    d.model.promote = GetParam();
    gWaiters.clear();
    gUncertain.clear();
    Walk walk;
    randomWalk(d, 200000, walk);
    EXPECT_GT(walk.fromCompletions, 1500u) << walk.fromCompletions;
    EXPECT_GT(walk.fromFilters, 1500u) << walk.fromFilters;
    EXPECT_GT(walk.fromCompletions + walk.fromFilters, 4000u);
    EXPECT_GT(d.shown.size(), 6000u) << d.shown.size();
    EXPECT_LT(d.swaps.size(), 4u) << "swaps never completed: " << d.swaps.size() << ", the first " << d.swaps.front();
    EXPECT_TRUE(gLog.empty()) << gLog.front();
}

INSTANTIATE_TEST_SUITE_P(Promote, MediaFlipWalk, testing::Values(DcModel::kPerWindow, DcModel::kEveryWindow));

// The display's own hook sites.
TEST(MediaFlipSource, TheDisplayCallsItWhereTheOrderingNeedsIt)
{
    using switch_ios::test::Between;
    using switch_ios::test::SourceText;
    const std::string fence = SourceText("../SwitchMediaPlaneFence.h");
    EXPECT_NE(fence.find("planeFenceWithMediaFlip(p, producers, count)"), std::string::npos);
    EXPECT_EQ(fence.find("switch_host1x_token_fence(producers"), std::string::npos);
    const std::string flip = SourceText("../SwitchCompletionFlipDisplay.h");
    const std::string write = Between(flip, "void writeCompletionFlip(int status)", "\n}\n");
    EXPECT_LT(write.find("IOSimpleLockLockDisableInterrupt(lock)"), write.find("writeMediaFlipLocked(status);"));
    EXPECT_LT(write.find("writeMediaFlipLocked(status);"), write.find("IOSimpleLockUnlockEnableInterrupt(lock"));
    EXPECT_NE(Between(flip, "void takeCompletionFlip()", "\n}\n").find("takeMediaFlip();"), std::string::npos);
    EXPECT_NE(Between(flip, "void suspendCompletionFlip()", "\n}\n").find("mediaFlipFromGate();"), std::string::npos);
    EXPECT_NE(Between(flip, "bool completionFlipOut() const", "\n}\n").find("!mediaFlipIdle()"), std::string::npos);
    EXPECT_EQ(Between(SourceText("../NativePlaneFenceDisplay.h"), "void finishPlaneFence(int status)\n{\n", ";")
                  .find("mediaFlipPass()"),
              4u);
    const std::string planes = SourceText("../NativePlanesDisplay.h");
    EXPECT_EQ(Between(planes, "void planeOut(const PlaneSet &p)\n{\n", ")").find("if (presentedByMediaFlip(p"), 4u);
    EXPECT_EQ(Between(planes, "void stopPlanes()\n{\n", ";").find("mediaFlipFromGate()"), 4u);
    const std::string frameEnd = Between(planes, "void planeFrameEnd()", "\n}\n");
    EXPECT_NE(frameEnd.find("if (planeLatchRequested()) {\n        if (planeLatchedAtFrameEnd()) {"),
              std::string::npos);
    EXPECT_EQ(frameEnd.find("planeScannedSet()"), std::string::npos);
    EXPECT_EQ(Between(SourceText("../SwitchOverlayRegisters.h"),
                      "bool planeStage(PlaneSet p, uint32_t &update, "
                      "uint32_t &request)\n{\n",
                      ";")
                  .find("mediaFlipFromGate()"),
              4u);
    EXPECT_EQ(Between(SourceText("../SwitchOverlayRecovery.h"), "void beginPlaneRecovery(const char *why)\n{\n", ";")
                  .find("mediaFlipFromGate()"),
              4u);
    EXPECT_NE(SourceText("../NativeLegacyDisplay.cpp")
                  .find("#include \"SwitchMediaPlaneFence.h\"\n#include \"SwitchMediaCompletionFlipDisplay.h\""),
              std::string::npos);
}

// The device line's other writers of windows B and C (the landscape change, the window probes) take a written set and
// withdraw an armed one first, and none arms in landscape or during a window probe.
TEST(MediaFlipSource, TheLandscapeAndWindowProbeWritersTakeItFirst)
{
    using switch_ios::test::Between;
    using switch_ios::test::SourceText;
    const std::string landscape = SourceText("../SwitchLandscapeDisplay.h");
    for (const char *function : {"void landscapeEnter(PlaneSet p)\n{\n", "void landscapeLeave(PlaneSet p)\n{\n",
                                 "void landscapeLeaveNow(const char *why)\n{\n"})
        EXPECT_EQ(Between(landscape, function, ";").find("mediaFlipFromGate()"), 4u) << function;
    EXPECT_NE(Between(landscape, "const char *landscapeBusy()", "\n}\n").find("!mediaFlipIdle()"), std::string::npos);
    const std::string probe = SourceText("../DisplayWindowProbeDisplay.h");
    for (const char *function : {"void windowProbeShow(", "void windowProbeRestoreWindows("}) {
        const std::string body = Between(probe, function, "\n}\n");
        EXPECT_LT(body.find("mediaFlipFromGate();"), body.find("dc[")) << function;
    }
    const std::string post = Between(probe, "int windowProbePost(", "\n}\n");
    EXPECT_LT(post.find("mediaFlipFromGate();"), post.find("planeReserveBandwidth("));
    const std::string may = Between(SourceText("../SwitchMediaCompletionFlipDisplay.h"), "bool mediaFlipMay(", "\n}\n");
    for (const char *owner : {"p.landscape", "landscapeOn()", "landscapeHoldsKicks()", "windowProbeBusy()"})
        EXPECT_NE(may.find(owner), std::string::npos) << owner;
}

}  // namespace
}  // namespace media_flip_test
