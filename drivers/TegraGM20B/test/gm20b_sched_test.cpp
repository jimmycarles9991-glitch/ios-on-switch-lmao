// SPDX-License-Identifier: MIT
// TegraGM20BSchedPolicy.h (what a compositor kick preempts, the interleaved runlist, the deadline's holds) and
// TegraGM20BRecover.h's
// tgm_stuck (when a context-switch timeout tick or a late wait may kill a channel) and tgm_hang_blame (which channel a
// late wait kills), over the registers and channels of deploy-2045's hang: GR stuck switching from backboardd's channel 3
// to Maps' 26, PBDMA0 on 26.
#include <algorithm>
#include <cstdint>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BRecover.h"
#include "drivers/TegraGM20B/TegraGM20BSchedPolicy.h"

namespace
{
constexpr uint64_t kMs = 1000 * 1000;
constexpr uint32_t kHangEng0 = 0x801ae003;  // busy, ch 3 -> ch 26, ctx_status 7 (switch)
constexpr uint32_t kCtxValid = 1u << 13;

uint32_t resident(uint32_t chid)
{
	return PFIFO_ENG_BUSY | kCtxValid | chid;
}

TEST(KickVictim, GrMidSwitchIsLeftToTheHost)
{
	EXPECT_EQ(tgm_kick_victim(kHangEng0, 0, 1, 26, 0), TGM_VICTIM_SWITCHING);
}

TEST(KickVictim, AppOnGrIsPreempted)
{
	EXPECT_EQ(tgm_kick_victim(resident(26), 0, 1, 26, 0), 26);
}

TEST(KickVictim, AppHoldingThePbdmaWhileGrRunsTheCompositor)
{
	EXPECT_EQ(tgm_kick_victim(resident(3), 1, 1, 26, 0), 26);
}

TEST(KickVictim, NothingWhenOnlyHighChannelsRun)
{
	EXPECT_EQ(tgm_kick_victim(resident(3), 1, 1, 5, 1), TGM_VICTIM_NONE);
	EXPECT_EQ(tgm_kick_victim(0, 0, 0, 0, 0), TGM_VICTIM_NONE);
}

TEST(KickVictim, IdleGrWithAContextLoadedIsNoVictim)
{
	EXPECT_EQ(tgm_kick_victim(kCtxValid | 26, 0, 0, 26, 0), TGM_VICTIM_NONE);
}

TEST(KickVictim, ATsgIdNamesNoChannel)
{
	EXPECT_EQ(tgm_kick_victim(resident(7) | PFIFO_ENG_ID_TSG, 0, 0, 0, 0), TGM_VICTIM_NONE);
}

std::vector<uint16_t> plan(const std::vector<uint8_t> &level, const std::vector<uint16_t> &entries,
			   unsigned max)
{
	std::vector<uint16_t> order(4096);
	order.resize(tgm_interleave(level.data(), entries.data(), unsigned(level.size()), max,
				    order.data(), unsigned(order.size())));
	return order;
}

unsigned entriesOf(const std::vector<uint16_t> &order, const std::vector<uint16_t> &entries)
{
	unsigned n = 0;
	for (uint16_t u : order)
		n += entries[u];
	return n;
}

TEST(Interleave, EveryHighUnitFollowsEachLowOne)
{
	const std::vector<uint8_t> high = {0, 1, 0, 0, 1};
	const std::vector<uint16_t> entries(5, 1);
	EXPECT_EQ(plan(high, entries, 64), (std::vector<uint16_t>{0, 1, 4, 2, 1, 4, 3, 1, 4}));
}

TEST(Interleave, OnlyHighOrOnlyLowListsEachOnce)
{
	EXPECT_EQ(plan({1, 1}, {1, 1}, 64), (std::vector<uint16_t>{0, 1}));
	EXPECT_EQ(plan({0, 0, 0}, {1, 1, 1}, 64), (std::vector<uint16_t>{0, 1, 2}));
	EXPECT_TRUE(plan({}, {}, 64).empty());
}

TEST(Interleave, AFullRunlistSpacesTheHighUnitsOutAndKeepsEveryUnit)
{
	std::vector<uint8_t> high(100, 0);
	std::vector<uint16_t> entries(100, 1);
	high[0] = high[50] = 1;
	entries[50] = 3;  // a group: header and two channels
	const unsigned max = 140;
	const auto order = plan(high, entries, max);
	EXPECT_LE(entriesOf(order, entries), max);
	for (uint16_t u = 0; u < 100; u++)
		EXPECT_NE(std::find(order.begin(), order.end(), u), order.end()) << "unit " << u;
	EXPECT_GT(std::count(order.begin(), order.end(), 0), 1);
}

TEST(Interleave, RoomForOneRepeatListsTheHighOnesOnceAfterTheLowOnes)
{
	const std::vector<uint8_t> high = {0, 1, 0};
	const std::vector<uint16_t> entries = {1, 1, 1};
	EXPECT_EQ(plan(high, entries, 3), (std::vector<uint16_t>{0, 2, 1}));
}

TEST(Interleave, NoRoomForEvenThatListsEachOnce)
{
	const std::vector<uint8_t> high = {0, 1, 0};
	const std::vector<uint16_t> entries = {1, 1, 1};
	EXPECT_EQ(plan(high, entries, 2), (std::vector<uint16_t>{0, 1, 2}));
}

constexpr uint8_t kBg = TGM_LEVEL_BACKGROUND, kFg = TGM_LEVEL_FOREGROUND, kHigh = TGM_LEVEL_HIGH;

// nvgpu's three levels: the compositor after every other unit, every foreground app between two background ones
TEST(Interleave, ThreeLevelsTheNvgpuWay)
{
	const std::vector<uint8_t> level = {kBg, kFg, kHigh, kFg, kBg};
	const std::vector<uint16_t> entries(5, 1);
	EXPECT_EQ(plan(level, entries, 64),
		  (std::vector<uint16_t>{0, 2, 1, 2, 3, 2, 4, 2, 1, 2, 3, 2}));
}

TEST(Interleave, WhicheverUnitTheHostLeavesAHigherLevelIsNext)
{
	const std::vector<uint8_t> level = {kFg, kBg, kHigh, kBg, kFg, kHigh, kBg};
	const std::vector<uint16_t> entries(level.size(), 1);
	const auto order = plan(level, entries, 256);
	for (size_t i = 0; i < order.size(); i++) {
		const uint8_t here = level[order[i]], next = level[order[(i + 1) % order.size()]];
		if (here != kHigh)
			EXPECT_EQ(next, kHigh) << "after position " << i;
	}
	for (uint16_t u = 0; u < level.size(); u++)
		EXPECT_NE(std::find(order.begin(), order.end(), u), order.end()) << "unit " << u;
}

TEST(Interleave, TwoLevelsOfAnyKindInterleaveAsHighAndLow)
{
	EXPECT_EQ(plan({kBg, kHigh, kBg}, {1, 1, 1}, 64), (std::vector<uint16_t>{0, 1, 2, 1}));
	EXPECT_EQ(plan({kBg, kFg, kBg}, {1, 1, 1}, 64), (std::vector<uint16_t>{0, 1, 2, 1}));
}

TEST(Interleave, ThreeLevelsTooBigForTheRunlistKeepTheCompositorAfterEveryApp)
{
	// 3 background, 3 foreground, 1 high: the full interleave needs 3 + 3 x 7 = 24 entries, more than 13
	const std::vector<uint8_t> level = {kBg, kFg, kBg, kFg, kBg, kFg, kHigh};
	const std::vector<uint16_t> entries(level.size(), 1);
	const auto order = plan(level, entries, 13);
	EXPECT_LE(order.size(), 13u);
	EXPECT_EQ(order, (std::vector<uint16_t>{0, 6, 1, 6, 2, 6, 3, 6, 4, 6, 5, 6}));
}

constexpr uint64_t kInterval = 16666667;  // 60 Hz

// Metal's GPU priorities as Core Animation sets them (CA::OGL::new_metal_context): its window server renderer 0, the
// asynchronous dispatcher 1, other contexts 2 and 4, background contexts' background priority 2
TEST(Priority, OnlyTheTopMetalPriorityIsTheCompositors)
{
	EXPECT_EQ(tgm_priority_level(0, 4, 0), TGM_LEVEL_HIGH);
	EXPECT_EQ(tgm_priority_level(1, 4, 0), TGM_LEVEL_FOREGROUND);
	EXPECT_EQ(tgm_priority_level(2, 4, 0), TGM_LEVEL_FOREGROUND);
	EXPECT_EQ(tgm_priority_level(4, 4, 0), TGM_LEVEL_BACKGROUND);
}

TEST(Priority, ABackgroundRoleRunsAtTheBackgroundPriority)
{
	EXPECT_EQ(tgm_priority_level(2, 4, 1), TGM_LEVEL_BACKGROUND);
	EXPECT_EQ(tgm_priority_level(2, 2, 1), TGM_LEVEL_FOREGROUND);
	// the compositor's level is never a background process's
	EXPECT_EQ(tgm_priority_level(0, 0, 1), TGM_LEVEL_FOREGROUND);
}

TEST(Deadline, AChannelIsHeldItsPreemptionLatencyBeforeTheCompositorsLatestStart)
{
	const uint64_t vblank = 1000 * kMs, expected = 6 * kMs, latency = 2 * kMs;
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, vblank - 20 * kMs, expected, latency, 1),
		  vblank - 8 * kMs);
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, vblank - 8 * kMs, expected, latency, 1),
		  vblank - 8 * kMs);
	// a channel with a longer latency is held earlier for the same frame
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, vblank - 30 * kMs, expected, 18 * kMs, 1),
		  vblank - 24 * kMs);
}

TEST(Deadline, AHopelessHoldWaitsForTheNextVblankOnlyWithSlackToIt)
{
	const uint64_t vblank = 1000 * kMs, expected = 6 * kMs, latency = 2 * kMs;
	// 3 ms before the vblank: cannot make it; 19.7 ms to the next, more than the 8 needed: held 8 ms before that one
	uint64_t now = vblank - 3 * kMs;
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, now, expected, latency, 1),
		  vblank + kInterval - 8 * kMs);
	// the 18 ms hog: 18 + 6 = 24 ms needed, 19.7 to the next vblank: no slack, held now (never slower for the compositor)
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, now, expected, 18 * kMs, 1), now);
	// past the vblank already
	now = vblank + 2 * kMs;
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, now, expected, latency, 1),
		  vblank + kInterval - 8 * kMs);
}

TEST(Deadline, AChannelNotOnTheGpuOnlyNeedsDisablingWhenItsHoldIsLate)
{
	const uint64_t vblank = 1000 * kMs, expected = 6 * kMs, latency = 2 * kMs;
	// 7 ms before the vblank, past its hold time (8 ms before): off the GPU it is disabled now and the frame makes it
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, vblank - 7 * kMs, expected, latency, 0),
		  vblank - 7 * kMs);
	// on the GPU it needs its 2 ms to leave: hopeless for this vblank, held 8 ms before the next
	EXPECT_EQ(tgm_hold_time(vblank, kInterval, vblank - 7 * kMs, expected, latency, 1),
		  vblank + kInterval - 8 * kMs);
}

TEST(Deadline, TheCompositorsFrameIsItsGpuTimeWithoutOverlap)
{
	// two submissions queued back to back: the second's start stamp is before the first's end, so it counts from there
	const uint64_t first = tgm_gpu_span(1008 * kMs, 1000 * kMs, 0);
	const uint64_t second = tgm_gpu_span(1012 * kMs, 1003 * kMs, 1008 * kMs);
	const uint64_t present = tgm_gpu_span(1013 * kMs, 1009 * kMs, 1012 * kMs);
	EXPECT_EQ(first + second + present, 13 * kMs);
	// an app ran between them: the gap is not the compositor's
	EXPECT_EQ(tgm_gpu_span(1030 * kMs, 1025 * kMs, 1012 * kMs), 5 * kMs);
	EXPECT_EQ(tgm_gpu_span(1000 * kMs, 1005 * kMs, 0), 0u);
}

// deploy-next3's stats line: PTIMER x1.627 (ns per unit)
TEST(Deadline, PtimerSpansInNanoseconds)
{
	const uint64_t rate = tgm_pt_rate(1627 * kMs, 1000 * kMs);
	EXPECT_NEAR(double(tgm_pt_ns(10 * kMs, rate)), 16270 * 1000.0,
		    100.0);                              // 16.16: within 0.001%
	EXPECT_EQ(tgm_pt_rate(50 * kMs, 30 * kMs), 0u);  // too short a span to tell
	EXPECT_EQ(tgm_pt_ns(10 * kMs, 0), 0u);
}

TEST(Deadline, TheHogGetsATurnEveryOtherWindow)
{
	const uint64_t latency = 18 * kMs;
	// held in the last window, which closed 3 ms ago: its turn
	EXPECT_TRUE(tgm_owed_turn(1, 3 * kMs, latency));
	// the turn taken, it is held again in the window after
	EXPECT_FALSE(tgm_owed_turn(0, 3 * kMs, latency));
	// an app that leaves the GPU in 300 us is held every window
	EXPECT_FALSE(tgm_owed_turn(1, 3 * kMs, 300 * 1000));
}

TEST(Deadline, ErroredIsolatedOrRecoveringChannelsAreNeverHeldOrReleased)
{
	EXPECT_TRUE(tgm_may_release(0, 0, 0));
	EXPECT_FALSE(tgm_may_release(1, 0, 0));
	EXPECT_FALSE(tgm_may_release(0, 1, 0));
	EXPECT_FALSE(tgm_may_release(0, 0, 1));
	EXPECT_FALSE(tgm_may_release(0, 0, 2));
	EXPECT_TRUE(tgm_may_hold(kFg, 1, 0, 0, 0, 0, 0));
	EXPECT_TRUE(tgm_may_hold(kBg, 1, 0, 0, 0, 0, 0));
	EXPECT_FALSE(tgm_may_hold(kHigh, 1, 0, 0, 0, 0, 0));
	EXPECT_FALSE(tgm_may_hold(kFg, 0, 0, 0, 0, 0, 0));  // not an app channel gpu.c registered
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 1, 0, 0, 0, 0));  // the present in flight waits for it
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 0, 1, 0, 0, 0));
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 0, 0, 1, 0, 0));
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 0, 0, 0, 1, 0));
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 0, 0, 0, 0,
				  1));  // nvkm blocks it itself (a teardown, a context bind)
}

// r30-fences' implicit fences: a compositor submission that acquires on an app's pending surface write needs that app
// on the GPU (tgm_hold_need), as the present does for the channels it waits for
TEST(Deadline, AnAppTheCompositorWaitsOnIsNeverHeld)
{
	EXPECT_FALSE(tgm_may_hold(kFg, 1, 1, 0, 0, 0, 0));
	EXPECT_FALSE(tgm_may_hold(kBg, 1, 1, 0, 0, 0, 0));
}

TEST(Deadline, AnAppTheCompositorWaitsOnIsNeverPreemptedByItsKick)
{
	EXPECT_TRUE(tgm_kick_may_preempt(26, 0));
	EXPECT_FALSE(tgm_kick_may_preempt(26, 1));
	EXPECT_FALSE(tgm_kick_may_preempt(TGM_VICTIM_NONE, 0));
	EXPECT_FALSE(tgm_kick_may_preempt(TGM_VICTIM_SWITCHING, 0));
}

TEST(Deadline, CompositorTimeRisesAtOnceAndFallsSlowly)
{
	uint64_t e = tgm_expect(0, 16 * kMs);
	EXPECT_EQ(e, 8 * kMs);
	e = tgm_expect(e, 16 * kMs);
	EXPECT_EQ(e, 12 * kMs);
	for (int i = 0; i < 30; i++)
		e = tgm_expect(e, 16 * kMs);
	EXPECT_EQ(e, 16 * kMs);
	e = tgm_expect(e, 8 * kMs);
	EXPECT_EQ(e, 15 * kMs);
	for (int i = 0; i < 300; i++)
		e = tgm_expect(e, 8 * kMs);
	EXPECT_LE(e, 8 * kMs + 8);
}

TEST(Deadline, PreemptionLatencyTakesALongerSampleAtOnce)
{
	const uint64_t max = 100 * kMs;
	uint64_t p = tgm_preempt_sampled(TGM_PREEMPT_MIN_NS, 18 * kMs, max);
	EXPECT_EQ(p, 18 * kMs);
	p = tgm_preempt_sampled(p, 2 * kMs, max);
	EXPECT_EQ(p, 16 * kMs);
	EXPECT_EQ(tgm_preempt_sampled(TGM_PREEMPT_MIN_NS, 0, max), TGM_PREEMPT_MIN_NS);
	EXPECT_EQ(tgm_preempt_sampled(TGM_PREEMPT_MIN_NS, 200 * kMs, max), max);
	EXPECT_EQ(tgm_preempt_left_early(16 * kMs), 14 * kMs);
	EXPECT_EQ(tgm_preempt_left_early(TGM_PREEMPT_MIN_NS), TGM_PREEMPT_MIN_NS);
}

TEST(Deadline, WindowsOnlyWhileTheCompositorDraws)
{
	const uint64_t now = 1000 * kMs;
	// CA turned vsync on just now: the first frame after an idle is held for
	EXPECT_TRUE(tgm_compositor_drawing(now, kInterval, now, now, 0));
	// vsync on for a while, the compositor kicked last frame
	EXPECT_TRUE(tgm_compositor_drawing(now, kInterval, now, now - 500 * kMs, now - 10 * kMs));
	// its idle countdown: vsync still on, nothing kicked for three frames
	EXPECT_FALSE(tgm_compositor_drawing(now, kInterval, now, now - 500 * kMs, now - 50 * kMs));
	// vsync off (a detached app, the display asleep, static UI)
	EXPECT_FALSE(tgm_compositor_drawing(now, kInterval, now - 30 * kMs, now - 500 * kMs,
					    now - 30 * kMs));
	EXPECT_FALSE(tgm_compositor_drawing(now, kInterval, 0, 0, now));
}

TEST(Deadline, HeldTimeNeverCountsTowardAStall)
{
	// 2.5 s without progress, 1 s of it held by the windows: 1.5 s stalled
	EXPECT_EQ(tgm_unheld_ns(3500 * kMs, 1000 * kMs, 4000 * kMs, 3000 * kMs), 1500 * kMs);
	EXPECT_EQ(tgm_unheld_ns(1000 * kMs, 900 * kMs, 500 * kMs, 300 * kMs), 0u);
	EXPECT_EQ(tgm_unheld_ns(900 * kMs, 1000 * kMs, 0, 0), 0u);  // progress after now was read
}

constexpr uint64_t kTick = 250 * kMs;  // recover.c's CTXSW_TIMEOUT_US

TEST(Stuck, ALongDrainOfTheOutgoingChannelIsWaitedOut)
{
	// backboardd's m2v pass of 144 ms, two a frame, while an app waits: its last completion 20 ms before the switch
	// request, the first tick 250 ms after it, the pass done at 288 ms, the second tick at 500 ms
	const uint64_t request = 1000 * kMs, lastDone = request - 20 * kMs;
	EXPECT_FALSE(tgm_stuck(0, 90, 92, request + kTick, lastDone));
	EXPECT_FALSE(tgm_stuck(0, 91, 92, request + 2 * kTick, request + 288 * kMs));
	EXPECT_LT(2 * 144 * kMs, TGM_STUCK_NS);
}

TEST(Stuck, Deploy2045sSwitchThatNeverEndsIsKilled)
{
	// ENG0 801ae003 at both ticks; backboardd's fence 90/93, nothing completed since 20 ms before the request
	EXPECT_EQ(tgm_kick_victim(kHangEng0, 1, 1, 26, 0), TGM_VICTIM_SWITCHING);
	const uint64_t request = 1000 * kMs, lastDone = request - 20 * kMs;
	EXPECT_FALSE(tgm_stuck(0, 90, 93, request + kTick, lastDone));
	EXPECT_TRUE(tgm_stuck(0, 90, 93, request + 2 * kTick, lastDone));
}

TEST(Stuck, IdleOrDeadChannelsNeverAre)
{
	EXPECT_FALSE(tgm_stuck(0, 93, 93, 10000 * kMs, 0));
	EXPECT_FALSE(tgm_stuck(1, 90, 93, 10000 * kMs, 0));
	EXPECT_TRUE(
		tgm_stuck(0, 0xfffffffe, 2, 10000 * kMs, 0));  // a fence word wrapping past zero
}

struct Candidates {
	std::vector<tgm_hang_candidate> c;
	int late_dead = 0, late_kernel = 0, progress = 0;

	tgm_hang verdict(int &blame) const
	{
		return tgm_hang_blame(c.data(), int(c.size()), late_dead, late_kernel, progress,
				      &blame);
	}
};

constexpr tgm_hang_candidate kStuckClient = {1, 1, 0}, kBusyClient = {1, 0, 0}, kNone = {0, 0, 0},
			     kStuckKernel = {1, 1, 1}, kBusyKernel = {1, 0, 1};

TEST(HangBlame, Deploy2045KillsTheContextGrCannotSwitch)
{
	// the present waited for backboardd; GR's switch blames Maps (restoring 26), PBDMA0 is on 26 too
	Candidates k{{kStuckClient, kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 0);
}

TEST(HangBlame, TheCompositorIsRecoveredWhenItIsTheOneStuck)
{
	// GR stuck saving backboardd's context: suspects 3 (backboardd) then 26 (Maps), late is backboardd
	Candidates k{{kStuckClient, kStuckClient, kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 0);
}

TEST(HangBlame, AChannelOnAnEndlessAcquireBlamesItself)
{
	// GR idle, the PBDMA switched away from it (acquire switch): no suspect, the late channel is stuck
	Candidates k{{kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 1);
}

TEST(HangBlame, TheTaskChannelItWaitsOnComesBeforeIt)
{
	Candidates k{{kBusyClient, kStuckClient, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, 1);
}

TEST(HangBlame, SlowWorkIsNotKilled)
{
	Candidates k{{kBusyClient, kNone, kBusyClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
	EXPECT_EQ(blame, -1);
}

TEST(HangBlame, AStuckKernelChannelIsFatal)
{
	Candidates k{{kStuckKernel, kNone, kStuckClient}};
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_STUCK);
}

TEST(HangBlame, LateKernelWorkPanicsOnlyWhenNothingProgresses)
{
	Candidates k{{kNone, kBusyKernel}};
	k.late_kernel = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_STUCK);
	k.progress = 1;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_PROGRESSING);
}

TEST(HangBlame, ALateChannelAlreadyKilledIsRecovering)
{
	Candidates k{{kNone, {1, 0, 0}}};
	k.late_dead = 1;
	int blame;
	EXPECT_EQ(k.verdict(blame), TGM_HANG_RECOVERING);
	EXPECT_EQ(blame, -1);
}
}  // namespace
