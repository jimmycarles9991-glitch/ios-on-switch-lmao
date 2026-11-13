// SPDX-License-Identifier: MIT
// The order of an engine reset and the PBDMA's ENG_RESET, over a model of the two threads that meet there: nvkm's
// runlist work (or recover.c's) resetting GR, and the interrupt pass. The model takes recover_engine.c's steps
// with TegraGM20BRecover.h's hold and PBDMA decisions; gm20b_source_test checks those steps' order in the source.
#include <cstdint>
#include <string>
#include <vector>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BRecover.h"

namespace
{
enum class Phase {
	Running,
	InReset,
	Initialising,
	Back
};

// The PBDMA stalls on ENG_RESET until it is cleared, then sends its method on into the engine, unless the host was
// asked to preempt the channel (then it saves the channel out and the scheduler loads its context again later).
struct Model {
	bool held =
		true;  // recover_engine.c's protocol; false: the interrupt pass acks whenever it runs (before)
	bool alive = true;  // the PBDMA's channel
	Phase phase = Phase::Running;
	uint32_t holds = 0;
	uint32_t en = TGM_PBDMA_ENG_RESET, intr = 0;
	bool preempted = false, nop = false, resumed = false;
	Phase resumedAt = Phase::Running;

	void ack()
	{
		if (!alive)
			nop = true;
		intr &= ~TGM_PBDMA_ENG_RESET;
		resumed = true;
		resumedAt = phase;
	}

	void hold()
	{
		if (held && tgm_reset_hold_enter(&holds)) {
			en &= ~TGM_PBDMA_ENG_RESET;
			preempted = tgm_reset_pbdma(PBDMA_BUSY, alive) == TGM_RESET_PBDMA_LIVE;
		}
	}

	void release()
	{
		if (held && tgm_reset_hold_leave(&holds)) {
			if (intr & TGM_PBDMA_ENG_RESET)
				ack();
			en |= TGM_PBDMA_ENG_RESET;
		}
	}

	void interruptPass()
	{
		if (intr & en & TGM_PBDMA_ENG_RESET)
			ack();
	}

	// what reaches GR: the PBDMA's method for a channel it kept, or a load the scheduler makes for a preempted one
	bool methodBeforeEngineBack() const
	{
		return resumed && !preempted && !nop && resumedAt != Phase::Back;
	}
};

using Step = void (*)(Model &);

const std::vector<Step> resetSteps = {
	[](Model &m) {
		m.hold();
	},
	[](Model &m) {
		m.phase = Phase::
			InReset;  // NV_PMC_ENABLE off: ENG_RESET raised on the PBDMA's channel
		m.intr |= TGM_PBDMA_ENG_RESET;
	},
	[](Model &m) {
		m.phase = Phase::Initialising;
	},  // out of reset, FECS being loaded
	[](Model &m) {
		m.phase = Phase::Back;
	},
	[](Model &m) {
		m.release();
	},
};

// every point the interrupt pass can run at, between the reset's steps
std::vector<Model> interleavings(bool held, bool alive)
{
	std::vector<Model> runs;
	for (size_t at = 0; at <= resetSteps.size(); at++) {
		Model m;
		m.held = held;
		m.alive = alive;
		for (size_t i = 0; i < resetSteps.size(); i++) {
			if (i == at)
				m.interruptPass();
			resetSteps[i](m);
		}
		if (at == resetSteps.size())
			m.interruptPass();
		m.interruptPass();
		runs.push_back(m);
	}
	return runs;
}

TEST(EngResetOrder, BeforeTheHoldTheInterruptPassLetThePbdmaIntoAResettingEngine)
{
	int early = 0;
	for (const Model &m : interleavings(false, true))
		early += m.methodBeforeEngineBack();
	EXPECT_GT(early, 0) << "an ack between NV_PMC_ENABLE and the end of GR's init";
}

TEST(EngResetOrder, NoInterleavingAcksBeforeTheEngineIsBack)
{
	for (bool alive : {true, false}) {
		for (const Model &m : interleavings(true, alive)) {
			EXPECT_TRUE(m.resumed) << alive;
			EXPECT_EQ(m.resumedAt, Phase::Back) << alive;
			EXPECT_FALSE(m.methodBeforeEngineBack()) << alive;
			EXPECT_EQ(m.en & TGM_PBDMA_ENG_RESET, TGM_PBDMA_ENG_RESET)
				<< "unmasked after";
			EXPECT_EQ(m.holds, 0u);
		}
	}
}

TEST(EngResetOrder, TheLiveChannelIsPreemptedNotKilledAndKeepsItsMethod)
{
	for (const Model &m : interleavings(true, true)) {
		EXPECT_TRUE(m.preempted);
		EXPECT_FALSE(m.nop);
	}
}

TEST(EngResetOrder, AKilledChannelsMethodIsDroppedAtTheRelease)
{
	for (const Model &m : interleavings(true, false)) {
		EXPECT_FALSE(m.preempted);
		EXPECT_TRUE(m.nop);
	}
}

// recover.c's reset of GR goes through GR's own (clkgate.c gated_reset): only the outermost release acks
TEST(EngResetOrder, ANestedResetAcksOnlyAtTheOutermostRelease)
{
	Model m;
	m.hold();
	m.hold();
	m.phase = Phase::InReset;
	m.intr |= TGM_PBDMA_ENG_RESET;
	m.phase = Phase::Back;
	m.release();
	m.interruptPass();
	EXPECT_FALSE(m.resumed);
	m.release();
	EXPECT_TRUE(m.resumed);
	EXPECT_EQ(m.resumedAt, Phase::Back);
}
}  // namespace
