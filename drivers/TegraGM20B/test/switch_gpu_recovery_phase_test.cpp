// SPDX-License-Identifier: MIT
#include "switch_gpu_recovery_phase_fixture.h"
#include "drivers/TegraGM20B/test/switch_gpu_recovery_phase_source.h"

namespace
{
constexpr unsigned failed = 2, live = 3, otherFailed = 4;

u32 status(unsigned state, u32 id, u32 next = 0)
{
	return state << 13 | id | next << 16;
}

int cxid()
{
	return -1;
}

const nvkm_engn_func engineFunctions{cxid};

class RecoveryPhase : public testing::Test
{
protected:
	switch_recovery_phase::Hardware hw;
	nvkm_device device{};
	nvkm_fifo fifo{};
	nvkm_engine graphics{};
	nvkm_runq queue{0};
	nvkm_engn engine{&engineFunctions, nullptr, 0, &graphics};
	nvkm_runl runl{};
	std::array<nvkm_cgrp, 16> groups{};
	std::array<nvkm_chan, 16> channels{};
	u32 from = 0;

	void SetUp() override
	{
		switch_recovery_phase::hardware = &hw;
		hw.runl = &runl;
		device.fifo = &fifo;
		fifo.engine.subdev.device = &device;
		graphics.subdev.device = &device;
		runl.fifo = &fifo;
		runl.runq[0] = &queue;
		runl.runq_nr = 1;
		runl.engns = &engine;
		runl.mutex = true;
		for (unsigned i = 0; i < groups.size(); i++) {
			groups[i].runl = &runl;
			channels[i].cgrp = &groups[i];
			runl.channels[i] = &channels[i];
			runl.groups[i] = &groups[i];
		}
		groups[failed].rc = groups[otherFailed].rc = NVKM_CGRP_RC_RUNNING;
		channels[failed].errored = channels[otherFailed].errored = 1;
	}

	void TearDown() override
	{
		EXPECT_EQ(hw.locks, 0);
		EXPECT_EQ(hw.lookups, 0);
		switch_recovery_phase::hardware = nullptr;
	}

	bool pending()
	{
		return switch_gpu_recover_pending(&runl, &from);
	}
};

TEST_F(RecoveryPhase, AnotherPreemptInFlightIsWaitedForBeforeTheFailedGroupIsAskedFor)
{
	hw.pbdma = status(1, failed);
	hw.preempt = PFIFO_PREEMPT_PENDING | live;
	EXPECT_TRUE(pending());
	EXPECT_TRUE(hw.writes.empty());
	EXPECT_EQ(from, 0u);
	hw.preempt = live;
	EXPECT_TRUE(pending());
	ASSERT_EQ(hw.writes.size(), 1u);
	EXPECT_EQ(hw.writes[0], failed);
	EXPECT_EQ(from, failed + 1);
	EXPECT_EQ(hw.unblocks, 1);
}

// A preempt is done once PREEMPT_PENDING clears (nvgpu gk20a_fifo_is_preempt_pending): a status still naming its target
// afterwards neither keeps the wait going nor asks for the same preempt again.
TEST_F(RecoveryPhase, ACompletedPreemptEndsTheWaitThoughThePbdmaStillNamesItsTarget)
{
	hw.pbdma = status(1, failed);
	ASSERT_TRUE(pending());
	hw.preempt = failed;
	EXPECT_FALSE(pending());
	EXPECT_EQ(hw.writes, std::vector<u32>{failed});
	EXPECT_EQ(hw.unblocks, 1);
}

TEST_F(RecoveryPhase, ACompletedPreemptEndsTheWaitThoughAnEngineStillNamesItsTarget)
{
	hw.pbdma = status(1, live);
	hw.engines[0] = PFIFO_ENG_BUSY | status(1, failed);
	ASSERT_TRUE(pending());
	hw.preempt = failed;
	EXPECT_FALSE(pending());
	EXPECT_EQ(hw.writes, std::vector<u32>{failed});
}

// nvkm_runl_rc's loop, with the hardware completing every preempt at once and every status frozen: each failed group is
// asked for once and the wait ends well inside nvkm_msec's iterations.
TEST_F(RecoveryPhase, FrozenStatusesEndTheWaitAfterOnePreemptPerFailedGroup)
{
	hw.pbdma = status(7, otherFailed, failed);
	hw.engines[0] = PFIFO_ENG_BUSY | status(1, failed);
	int calls = 0;
	while (calls < 16 && pending()) {
		hw.preempt &= ~u32{PFIFO_PREEMPT_PENDING};
		calls++;
	}
	EXPECT_LT(calls, 16);
	EXPECT_EQ(hw.writes, (std::vector<u32>{failed, otherFailed}));
}

TEST_F(RecoveryPhase, AFailedGroupsPreemptAnotherWriterAskedForIsWaitedFor)
{
	hw.pbdma = status(1, failed);
	hw.preempt = PFIFO_PREEMPT_PENDING | failed;
	EXPECT_TRUE(pending());
	EXPECT_TRUE(hw.writes.empty());
	hw.preempt = failed;
	hw.pbdma = 0;
	EXPECT_FALSE(pending());
	EXPECT_TRUE(hw.writes.empty());
}

TEST_F(RecoveryPhase, UnrelatedLiveWorkMayRescheduleAfterFailedTargetDisappears)
{
	hw.pbdma = status(1, failed);
	ASSERT_TRUE(pending());
	hw.preempt = failed;
	hw.pbdma = status(1, live);
	hw.engines[0] = PFIFO_ENG_BUSY | status(1, live);
	EXPECT_FALSE(pending());
	EXPECT_EQ(hw.writes.size(), 1u);
	EXPECT_EQ(groups[live].rc, NVKM_CGRP_RC_NONE);
	EXPECT_EQ(channels[live].errored, 0);
}

TEST_F(RecoveryPhase, CurrentAndIncomingContextFieldsAreDecodedForEveryActiveState)
{
	EXPECT_TRUE(switch_gpu_recover_status_holds(status(1, failed, live), failed, false));
	EXPECT_TRUE(switch_gpu_recover_status_holds(status(6, failed, live), failed, false));
	EXPECT_FALSE(switch_gpu_recover_status_holds(status(5, live, failed), live, false));
	EXPECT_TRUE(switch_gpu_recover_status_holds(status(5, live, failed), failed, false));
	EXPECT_TRUE(switch_gpu_recover_status_holds(status(7, failed, otherFailed), failed, false));
	EXPECT_TRUE(switch_gpu_recover_status_holds(status(7, failed, otherFailed), otherFailed,
						    false));
	EXPECT_FALSE(switch_gpu_recover_status_holds(status(0, failed, failed), failed, false));
	EXPECT_FALSE(switch_gpu_recover_status_holds(status(1, failed), failed, true));
	EXPECT_TRUE(
		switch_gpu_recover_status_holds(PFIFO_ENG_BUSY | status(1, failed), failed, true));
}

TEST_F(RecoveryPhase, IncomingLoadAndChannelGroupTypeSelectTheMatchingPreemptTarget)
{
	const u32 groupTarget = PFIFO_PREEMPT_TSG | failed;
	hw.pbdma = status(5, live, 0x1000 | failed);
	ASSERT_TRUE(pending());
	ASSERT_EQ(hw.writes.size(), 1u);
	EXPECT_EQ(hw.writes[0], groupTarget);
	EXPECT_EQ(from, groupTarget + 1);
	hw.preempt = groupTarget;
	EXPECT_FALSE(pending());
	EXPECT_EQ(hw.writes.size(), 1u);
}

TEST_F(RecoveryPhase, SemaphoreAcquireWaiterIsStillEvicted)
{
	hw.pbdma = status(1, failed);
	hw.method = 0x8000001c;
	hw.data = 1;
	ASSERT_TRUE(pending());
	ASSERT_EQ(hw.writes.size(), 1u);
	EXPECT_EQ(hw.writes[0], failed);
}

TEST_F(RecoveryPhase, DistinctFailedSwitchSidesAreEachEvictedOnce)
{
	hw.pbdma = status(7, otherFailed, failed);
	ASSERT_TRUE(pending());
	ASSERT_EQ(hw.writes.size(), 1u);
	EXPECT_EQ(hw.writes[0], failed);
	hw.preempt = failed;
	hw.pbdma = status(5, live, otherFailed);
	EXPECT_TRUE(pending());
	ASSERT_EQ(hw.writes.size(), 2u);
	EXPECT_EQ(hw.writes[1], otherFailed);
	hw.preempt = otherFailed;
	hw.pbdma = status(1, live);
	EXPECT_FALSE(pending());
	EXPECT_EQ(hw.writes.size(), 2u);
}

TEST_F(RecoveryPhase, IdleEngineLoadUsesIncomingFailedContext)
{
	hw.engines[0] = status(5, live, failed);
	ASSERT_TRUE(pending());
	ASSERT_EQ(hw.writes.size(), 1u);
	EXPECT_EQ(hw.writes[0], failed);
}

TEST_F(RecoveryPhase, SoftwareEngineCannotSupplyAFailedResidentContext)
{
	const nvkm_engn_func softwareFunctions{nullptr};
	nvkm_engn software{&softwareFunctions, nullptr, 1, &graphics};
	engine.next = &software;
	hw.engines[1] = PFIFO_ENG_BUSY | status(1, failed);
	hw.engineUnreadable[1] = true;
	EXPECT_FALSE(pending());
	EXPECT_EQ(from, 0u);
	EXPECT_TRUE(hw.writes.empty());
	EXPECT_EQ(hw.engineReads[1], 0u);
}

TEST_F(RecoveryPhase, NewerPendingRecoveryIsLeftForItsExistingOwner)
{
	groups[failed].rc = NVKM_CGRP_RC_PENDING;
	runl.rc_pending = 1;
	hw.pbdma = status(1, failed);
	hw.preempt = PFIFO_PREEMPT_PENDING | failed;
	EXPECT_TRUE(pending());
	hw.preempt = failed;
	EXPECT_FALSE(pending());
	EXPECT_TRUE(hw.writes.empty());
	EXPECT_EQ(groups[failed].rc, NVKM_CGRP_RC_PENDING);
	EXPECT_EQ(channels[failed].errored, 1);
	EXPECT_EQ(runl.rc_pending, 1);
}

TEST_F(RecoveryPhase, NewerPendingRecoveryDoesNotAdmitANewEviction)
{
	groups[failed].rc = NVKM_CGRP_RC_PENDING;
	runl.rc_pending = 1;
	hw.pbdma = status(1, failed);
	EXPECT_FALSE(pending());
	EXPECT_EQ(from, 0u);
	EXPECT_TRUE(hw.writes.empty());
	EXPECT_EQ(groups[failed].rc, NVKM_CGRP_RC_PENDING);
	EXPECT_EQ(channels[failed].errored, 1);
	EXPECT_EQ(runl.rc_pending, 1);
}

TEST_F(RecoveryPhase, SoftwareEngineIsNotReadAfterAPreemptCompleted)
{
	const nvkm_engn_func softwareFunctions{nullptr};
	nvkm_engn software{&softwareFunctions, nullptr, 1, &graphics};
	engine.next = &software;
	hw.engines[1] = PFIFO_ENG_BUSY | status(1, failed);
	hw.engineUnreadable[1] = true;
	from = failed + 1;
	hw.preempt = failed;
	EXPECT_FALSE(pending());
	EXPECT_TRUE(hw.writes.empty());
	EXPECT_EQ(hw.engineReads[1], 0u);
}

TEST_F(RecoveryPhase, UnblockResetsFailedIdleLoadIncomingContext)
{
	hw.preempt = PFIFO_PREEMPT_PENDING | failed;
	hw.engines[0] = status(5, live, failed);
	tgm_recover_preempt_unblock(&runl);
	ASSERT_EQ(hw.resets.size(), 1u);
	EXPECT_EQ(hw.resets[0].engine, 0);
	EXPECT_EQ(hw.resets[0].channel, static_cast<int>(failed));
	EXPECT_EQ(hw.engines[0], 0u);
	EXPECT_EQ(groups[live].rc, NVKM_CGRP_RC_NONE);
	EXPECT_EQ(channels[live].errored, 0);
}

TEST_F(RecoveryPhase, UnblockPreservesLiveIncomingLoadEvenWhenPriorContextFailed)
{
	hw.preempt = PFIFO_PREEMPT_PENDING | failed;
	const u32 loading = status(5, failed, live);
	hw.engines[0] = loading;
	tgm_recover_preempt_unblock(&runl);
	EXPECT_TRUE(hw.resets.empty());
	EXPECT_EQ(hw.engines[0], loading);
}

TEST_F(RecoveryPhase, UnblockResetsFailedPendingIncomingGroupAfterReleasingLookup)
{
	constexpr unsigned groupId = 6;
	groups[groupId].rc = NVKM_CGRP_RC_PENDING;
	runl.rc_pending = 1;
	hw.preempt = PFIFO_PREEMPT_PENDING | PFIFO_PREEMPT_TSG | groupId;
	hw.engines[0] = status(5, live, 0x1000 | groupId);
	tgm_recover_preempt_unblock(&runl);
	ASSERT_EQ(hw.resets.size(), 1u);
	EXPECT_EQ(hw.resets[0].channel, static_cast<int>(groupId));
	EXPECT_EQ(hw.engines[0], 0u);
	EXPECT_EQ(groups[groupId].rc, NVKM_CGRP_RC_PENDING);
	EXPECT_EQ(runl.rc_pending, 1);
}

TEST_F(RecoveryPhase, UnblockPreservesLiveIncomingGroup)
{
	hw.preempt = PFIFO_PREEMPT_PENDING | failed;
	const u32 loading = status(5, failed, 0x1000 | live);
	hw.engines[0] = loading;
	tgm_recover_preempt_unblock(&runl);
	EXPECT_TRUE(hw.resets.empty());
	EXPECT_EQ(hw.engines[0], loading);
}

TEST_F(RecoveryPhase, UnblockStillResetsFailedSavingContext)
{
	hw.preempt = PFIFO_PREEMPT_PENDING | failed;
	hw.engines[0] = PFIFO_ENG_BUSY | status(6, failed, live);
	tgm_recover_preempt_unblock(&runl);
	ASSERT_EQ(hw.resets.size(), 1u);
	EXPECT_EQ(hw.resets[0].channel, static_cast<int>(failed));
}

TEST_F(RecoveryPhase, UnblockStillResetsFailedSwitchSavingContext)
{
	hw.preempt = PFIFO_PREEMPT_PENDING | failed;
	hw.engines[0] = PFIFO_ENG_BUSY | status(7, failed, live);
	graphics.loading = false;
	tgm_recover_preempt_unblock(&runl);
	ASSERT_EQ(hw.resets.size(), 1u);
	EXPECT_EQ(hw.resets[0].channel, static_cast<int>(failed));
}

TEST_F(RecoveryPhase, UnblockDoesNotResetSwitchOnceLiveContextIsLoading)
{
	hw.preempt = PFIFO_PREEMPT_PENDING | failed;
	const u32 loading = PFIFO_ENG_BUSY | status(7, failed, live);
	hw.engines[0] = loading;
	graphics.loading = true;
	tgm_recover_preempt_unblock(&runl);
	EXPECT_TRUE(hw.resets.empty());
	EXPECT_EQ(hw.engines[0], loading);
}

TEST_F(RecoveryPhase, UnblockPreservesExistingUnownedSavingContextRecovery)
{
	hw.preempt = PFIFO_PREEMPT_PENDING | live;
	hw.engines[0] = PFIFO_ENG_BUSY | status(6, live, failed);
	hw.unowned[live] = true;
	tgm_recover_preempt_unblock(&runl);
	ASSERT_EQ(hw.resets.size(), 1u);
	EXPECT_EQ(hw.resets[0].channel, static_cast<int>(live));
}

TEST_F(RecoveryPhase, UnblockDoesNothingWithoutAnOutstandingPreempt)
{
	hw.engines[0] = status(5, live, failed);
	tgm_recover_preempt_unblock(&runl);
	EXPECT_TRUE(hw.resets.empty());
}
}
