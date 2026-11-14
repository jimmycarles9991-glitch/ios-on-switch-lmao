// SPDX-License-Identifier: MIT
// One client's channel killed while other clients have work on runlist 0 (gm20b_bystander_model.h): every other
// channel's jobs complete, none is killed and none reopens. Boot 21's 14:11:59.611 reset is the first scenario.
#include <string>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/test/gm20b_bystander_model.h"

namespace
{
using bystander::Gpu;
using bystander::Job;
using bystander::kCe;
using bystander::kGr;
using bystander::kPresent;
using bystander::Rule;

constexpr int kKilled = 10, kBystander = 21, kOther = 24, kKick = 13;

// The killed channel's job faulted on GR, which holds its context: it cannot be saved until GR is reset
void faultedOnGr(Gpu &gpu)
{
	gpu.submit(kKilled, {kGr, 1, true});
	gpu.eng[kGr].ctx = kKilled;
	gpu.eng[kGr].stuck = true;
}

// The bystander on PBDMA0 with a GR method pending, GR switching to it from the faulted context (FECS not loading
// yet, so engn_cxid names the faulted channel and nvkm resets GR), the compositor's kick preempt of the faulted
// channel in flight
void switchingToTheBystander(Gpu &gpu)
{
	faultedOnGr(gpu);
	gpu.submit(kBystander, {kGr, 3});
	gpu.submit(kBystander, {kCe, 2});
	gpu.pbdma = kBystander;
	gpu.eng[kGr].switching = true;
	gpu.eng[kGr].next = kBystander;
	gpu.ask(kKilled);
}

void presentQueued(Gpu &gpu)
{
	gpu.submit(kPresent, {kCe, 1});
}

// Boot 21, 14:11:59.611: ENG 000a200a (the copy engine idle on killed channel 10's context), PBDMA 00152015 (live
// channel 21), PGRAPH 00000081 (GR busy); the hold preempted 21, ENG_RESET was not raised, and the bystander's 257 runs
// had no error. Holds at 3006711cb.
TEST(Bystander, Boot21sResetUnderAChannelPreemptedMidJobOnGrLosesNothing)
{
	for (Rule rule : {Rule::Before, Rule::Shipped}) {
		Gpu gpu(rule);
		gpu.submit(kKilled, {kCe, 1});
		gpu.eng[kCe].ctx = kKilled;
		gpu.submit(kBystander, {kGr, 4});
		gpu.submit(kBystander, {kGr, 2});
		gpu.eng[kGr].ctx = kBystander;
		gpu.eng[kGr].busy = 4;
		gpu.pbdma = kBystander;
		presentQueued(gpu);
		gpu.recover(kKilled);
		gpu.run(3000);
		EXPECT_EQ(gpu.engineResets[kCe], 1);
		EXPECT_EQ(gpu.engineResets[kGr], 0);
		EXPECT_EQ(gpu.raised, 0);
		EXPECT_TRUE(gpu.done(kBystander));
		EXPECT_TRUE(gpu.done(kPresent));
		EXPECT_EQ(gpu.killed, std::vector<int>{kKilled});
		EXPECT_EQ(gpu.panics, 0);
	}
}

// The bystander mid-copy on the copy engine when the killed channel's GR context is reset. Holds at 3006711cb.
TEST(Bystander, AChannelPreemptedMidCopyLosesNothingToGrsReset)
{
	for (Rule rule : {Rule::Before, Rule::Shipped}) {
		Gpu gpu(rule);
		faultedOnGr(gpu);
		gpu.submit(kBystander, {kCe, 5});
		gpu.submit(kBystander, {kGr, 1});
		gpu.eng[kCe].ctx = kBystander;
		gpu.eng[kCe].busy = 5;
		gpu.pbdma = kBystander;
		presentQueued(gpu);
		gpu.recover(kKilled);
		gpu.run(3000);
		EXPECT_EQ(gpu.engineResets[kGr], 1);
		EXPECT_EQ(gpu.engineResets[kCe], 0) << "the copy engine holds a live context";
		EXPECT_EQ(gpu.raised, 0);
		EXPECT_TRUE(gpu.done(kBystander));
		EXPECT_EQ(gpu.killed, std::vector<int>{kKilled});
	}
}

// A preempt of another channel in flight at the hold: at 3006711cb the bystander rides through GR's reset on PBDMA0,
// ENG_RESET is raised on its method, PBDMA0 stays there, the present waits, and its second hang check kills the
// bystander.
TEST(Bystander, Before3006711cbAPreemptInFlightAtTheHoldGetsTheBystanderKilled)
{
	Gpu gpu(Rule::Before);
	switchingToTheBystander(gpu);
	presentQueued(gpu);
	gpu.recover(kKilled);
	EXPECT_EQ(gpu.raised, 1);
	EXPECT_TRUE(gpu.wedged);
	EXPECT_EQ(gpu.rodeThrough, kBystander);
	EXPECT_FALSE(gpu.rodeThroughSettled);
	gpu.run(5000);
	EXPECT_EQ(gpu.killed, (std::vector<int>{kKilled, kBystander}));
	EXPECT_GT(gpu.chans[kBystander].failed, 0u);
	EXPECT_EQ(gpu.chans[kBystander].reopens, 1);
	EXPECT_FALSE(gpu.done(kBystander));
	EXPECT_EQ(gpu.panics, 0);
}

// The owed preempt rests on dev_fifo.ref's teardown rule (a preempt completes once the PBDMA's interrupts are handled),
// as the hold's own preempt and the kernel channel's steps do. Were a preempt of a channel ENG_RESET was raised on never
// to land, nvkm_runl_rc's wait would panic ("BUG ... recovery.c" after a release line reading "landed: preempted at the
// release") where 3006711cb killed the bystander.
TEST(Bystander, TheOwedPreemptRestsOnAPreemptLandingOnceEngResetIsCleared)
{
	for (Rule rule : {Rule::Before, Rule::Shipped}) {
		Gpu gpu(rule);
		gpu.engResetStuck = true;
		switchingToTheBystander(gpu);
		presentQueued(gpu);
		gpu.recover(kKilled);
		gpu.run(5000);
		if (rule == Rule::Before) {
			EXPECT_EQ(gpu.panics, 0);
			EXPECT_EQ(gpu.killed, (std::vector<int>{kKilled, kBystander}));
		} else {
			EXPECT_EQ(gpu.panics, 1);
		}
	}
}

TEST(Bystander, ABystanderOnThePbdmaWithAPendingMethodIsPreemptedAtTheReleaseAndCompletes)
{
	Gpu gpu(Rule::Shipped);
	switchingToTheBystander(gpu);
	presentQueued(gpu);
	gpu.recover(kKilled);
	EXPECT_EQ(gpu.raised, 1) << "the hold could ask for no preempt of its own";
	EXPECT_TRUE(gpu.rodeThroughSettled);
	EXPECT_FALSE(gpu.wedged);
	EXPECT_EQ(gpu.owed, -1);
	gpu.run(3000);
	EXPECT_TRUE(gpu.done(kBystander));
	EXPECT_TRUE(gpu.done(kPresent));
	EXPECT_EQ(gpu.killed, std::vector<int>{kKilled});
	EXPECT_EQ(gpu.panics, 0);
	EXPECT_LT(gpu.now, 100 * bystander::kMs) << "no hang check needed";
}

// The preempt in flight still lands after the release: the owed preempt goes out after nvkm_runl_rc's wait for it
TEST(Bystander, AnOwedPreemptWaitsForThePreemptInFlightAndGoesOutBeforeTheRecoveryEnds)
{
	Gpu gpu(Rule::Shipped);
	switchingToTheBystander(gpu);
	gpu.landIn = 3;
	presentQueued(gpu);
	gpu.recover(kKilled);
	EXPECT_TRUE(gpu.rodeThroughSettled);
	gpu.run(3000);
	EXPECT_TRUE(gpu.done(kBystander));
	EXPECT_TRUE(gpu.done(kPresent));
	EXPECT_EQ(gpu.killed, std::vector<int>{kKilled});
	EXPECT_EQ(gpu.panics, 0);
}

// Its own preempt in flight (a kick of the bystander itself): nothing is owed, and that preempt takes it off
TEST(Bystander, ABystandersOwnPreemptInFlightOwesNothing)
{
	for (Rule rule : {Rule::Before, Rule::Shipped}) {
		Gpu gpu(rule);
		switchingToTheBystander(gpu);
		gpu.ask(kBystander);
		presentQueued(gpu);
		gpu.recover(kKilled);
		EXPECT_EQ(gpu.raised, 0) << "its preempt halted PBDMA0";
		gpu.run(3000);
		EXPECT_TRUE(gpu.done(kBystander));
		EXPECT_EQ(gpu.killed, std::vector<int>{kKilled});
	}
	EXPECT_EQ(tgm_reset_owed(TGM_RESET_PBDMA_LIVE, PFIFO_PREEMPT_PENDING | kBystander,
				 kBystander),
		  -1);
	EXPECT_EQ(tgm_reset_owed(TGM_RESET_PBDMA_LIVE,
				 PFIFO_PREEMPT_PENDING | PFIFO_PREEMPT_TSG | kBystander,
				 kBystander),
		  kBystander)
		<< "a group's preempt is another one";
	EXPECT_EQ(tgm_reset_owed(TGM_RESET_PBDMA_LIVE, 0, kBystander), -1);
	EXPECT_EQ(tgm_reset_owed(TGM_RESET_PBDMA_DEAD, PFIFO_PREEMPT_PENDING | kKick, kBystander),
		  -1);
	EXPECT_EQ(tgm_reset_owed(TGM_RESET_PBDMA_IDLE, PFIFO_PREEMPT_PENDING | kKick, kBystander),
		  -1);
}

// Two bystanders: one on PBDMA0 as above, one whose context the copy engine holds idle with a copy queued after. Only
// GR is reset (the copy engine's context is a live channel's), and both complete.
TEST(Bystander, TwoBystandersBothCompleteAndOnlyTheKilledChannelsEngineIsReset)
{
	for (Rule rule : {Rule::Before, Rule::Shipped}) {
		Gpu gpu(rule);
		switchingToTheBystander(gpu);
		gpu.eng[kCe].ctx = kOther;
		gpu.submit(kOther, {kCe, 2});
		presentQueued(gpu);
		gpu.recover(kKilled);
		gpu.run(5000);
		EXPECT_EQ(gpu.engineResets[kCe], 0) << int(rule);
		EXPECT_TRUE(gpu.done(kOther)) << int(rule);
		if (rule == Rule::Before) {
			EXPECT_FALSE(gpu.done(kBystander));
			continue;
		}
		EXPECT_TRUE(gpu.done(kBystander));
		EXPECT_EQ(gpu.killed, std::vector<int>{kKilled});
	}
}

// Every combination of what PBDMA0, the engines and PFIFO_PREEMPT hold at the kill: no live channel is killed, every
// one completes, and none rode through a reset without, by the end of the recovery, a preempt asked for it or leaving
// PBDMA0 by itself. gr: 0 GR on the faulted context and the bystander's next method a copy, 1 GR switching from it to
// the bystander's method, 2 GR running the bystander's job; ce: 0 idle, 1 the killed channel's context, 2 another
// client's with a copy queued.
TEST(Bystander, NoStateAtTheKillCostsAnotherChannelItsWork)
{
	int before = 0;
	for (Rule rule : {Rule::Before, Rule::Shipped}) {
		for (int pending : {-1, kKilled, kBystander, kKick}) {
			for (int gr = 0; gr < 3; gr++) {
				for (int ce = 0; ce < 3; ce++) {
					for (int land : {0, 3}) {
						Gpu gpu(rule);
						faultedOnGr(gpu);
						gpu.chans[kKick] = {};
						gpu.submit(kBystander, {gr == 2 ? kGr : kCe, 2});
						gpu.submit(kBystander, {kGr, 1});
						gpu.pbdma = kBystander;
						if (gr == 1) {
							gpu.chans[kBystander].jobs.front().engine =
								kGr;
							gpu.eng[kGr].switching = true;
							gpu.eng[kGr].next = kBystander;
						}
						if (gr == 2)
							gpu.eng[kGr] = {kBystander, -1, false,
									false, 2};
						if (ce == 1)
							gpu.eng[kCe].ctx = kKilled;
						if (ce == 2) {
							gpu.eng[kCe].ctx = kOther;
							gpu.submit(kOther, {kCe, 1});
						}
						if (pending >= 0)
							gpu.ask(pending);
						gpu.landIn = land;
						presentQueued(gpu);
						gpu.recover(kKilled);
						const bool settled = gpu.rodeThrough < 0 ||
								     gpu.rodeThroughSettled;
						gpu.run(6000);
						const bool lost =
							!gpu.done(kBystander) ||
							(ce == 2 && !gpu.done(kOther)) ||
							gpu.killed != std::vector<int>{kKilled};
						const std::string at = std::to_string(pending) +
								       " " + std::to_string(gr) +
								       " " + std::to_string(ce) +
								       " " + std::to_string(land);
						if (rule == Rule::Before) {
							EXPECT_EQ(gpu.panics, 0) << at;
							before += lost;
							continue;
						}
						EXPECT_FALSE(lost) << at;
						EXPECT_TRUE(settled) << at;
						EXPECT_TRUE(gpu.done(kPresent)) << at;
						EXPECT_EQ(gpu.panics, 0) << at;
					}
				}
			}
		}
	}
	EXPECT_GT(before, 0) << "3006711cb loses a bystander's work in some of them";
}
}  // namespace
