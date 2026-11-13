// SPDX-License-Identifier: MIT
// TegraGM20BAcquire.h: the RAMFC ACQUIRE word against nvgpu's gk20a_fifo_pbdma_acquire_val, the CCSR states of a channel
// switched out on an acquire, and what RAMFC's semaphore and syncpoint words say a parked channel waits on.
#include <cstdint>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BAcquire.h"

namespace
{
constexpr uint64_t kMs = 1000 * 1000;

TEST(AcquireWord, NouveausResetValueIsAboutThirtySevenMinutes)
{
	EXPECT_NEAR(double(tgm_acquire_timeout(0xfffff902u)) / 1e9, 2199.0, 1.0);
	EXPECT_EQ(0xfffff902u & 0x7ffu, TGM_ACQUIRE_RETRY);
}

TEST(AcquireWord, NvgpusFourSecondsTakeAnExponent)
{
	// ch_wdt_timeout_ms 5000 at 80 %: 4 s = 3906250 units of 1024 ns, 22 bits: exponent 6, mantissa 61035
	const uint32_t word = tgm_acquire_word(4000 * kMs);
	EXPECT_EQ(word, TGM_ACQUIRE_RETRY | 6u << 11 | 61035u << 15 | TGM_ACQUIRE_TIMEOUT_EN);
	EXPECT_LE(tgm_acquire_timeout(word), 4000 * kMs);
	EXPECT_GT(tgm_acquire_timeout(word), 3999 * kMs);
}

TEST(AcquireWord, ARealSecondIsProgrammedInPtimerTime)
{
	const uint64_t ptimer = tgm_acquire_ptimer_ns(1000 * kMs);
	EXPECT_EQ(ptimer, 614400000u);
	EXPECT_NEAR(double(tgm_acquire_timeout(tgm_acquire_word(ptimer))), 614400000.0,
		    16 * 1024.0);
}

TEST(AcquireWord, ZeroTurnsTheTimeoutOffAndShortOnesNeedNoExponent)
{
	EXPECT_EQ(tgm_acquire_word(0), TGM_ACQUIRE_RETRY);
	EXPECT_EQ(tgm_acquire_timeout(TGM_ACQUIRE_RETRY), 0u);
	EXPECT_EQ(tgm_acquire_word(1024 * 1000),
		  TGM_ACQUIRE_RETRY | 1000u << 15 | TGM_ACQUIRE_TIMEOUT_EN);
	EXPECT_EQ(tgm_acquire_word(UINT64_MAX),
		  TGM_ACQUIRE_RETRY | 0x7fff8000u | 0x7800u | TGM_ACQUIRE_TIMEOUT_EN);
}

TEST(Ccsr, OnlyTheAcquireStatesAreParked)
{
	for (uint32_t status = 0; status < 16; status++) {
		const bool parked = status == 3 || status == 4 || status == 8 || status == 14;
		EXPECT_EQ(tgm_ccsr_acquiring(status << 24 | 1u << 28 | 1u), parked) << status;
	}
}

// The kernel's acquires (gpu_exec.c, gpu_sync.c): SEMAPHOREA the VA's bits 39:32, B the low word, C the seqno, D ACQ_GEQ
// with ACQUIRE_SWITCH (0x1004).
TEST(Decode, AFenceAcquireNamesItsWordAndSeqno)
{
	const uint32_t sem[4] = {0x80u, 0x00104040u, 5032u, 0x1004u};
	const tgm_acquire_wait w = tgm_acquire_decode(sem, 0, 0, 1);
	EXPECT_EQ(w.kind, TGM_ACQUIRE_SEMAPHORE);
	EXPECT_EQ(w.va, 0x8000104040ull);
	EXPECT_EQ(w.payload, 5032u);
	EXPECT_EQ(w.operation, 4u);
}

TEST(Decode, ARamfcLeftOnAReleaseHoldsNoAcquire)
{
	const uint32_t release[4] = {0x80u, 0x00104040u, 7u, 0x2u};  // the job's closing fence
	EXPECT_EQ(tgm_acquire_decode(release, 0, 0, 0).kind, TGM_ACQUIRE_NONE);
	EXPECT_EQ(tgm_acquire_decode(release, 0, 0, 1).kind, TGM_ACQUIRE_NONE);
}

TEST(Decode, AParkedChannelWithNoSemaphoreAcquireWaitsOnItsSyncpoint)
{
	const uint32_t release[4] = {0, 0, 0, 0x2u};
	// tgm_syncpt_wait (TegraGM20BSyncpt.h): SYNCPOINTB index 8, WAIT_SWITCH, WAIT
	const tgm_acquire_wait w = tgm_acquire_decode(release, 400u, 18u << 8 | 1u << 4, 1);
	EXPECT_EQ(w.kind, TGM_ACQUIRE_SYNCPOINT);
	EXPECT_EQ(w.syncpoint, 18u);
	EXPECT_EQ(w.payload, 400u);
	EXPECT_EQ(tgm_acquire_decode(release, 400u, 18u << 8 | 1u << 4, 0).kind, TGM_ACQUIRE_NONE);
	EXPECT_EQ(tgm_acquire_decode(release, 0, 18u << 8 | 1u, 1).kind,
		  TGM_ACQUIRE_NONE);                                             // an increment
	EXPECT_EQ(tgm_acquire_decode(release, 0, 0, 1).kind, TGM_ACQUIRE_NONE);  // never used
}

TEST(Blocked, EachOperationAsTheHostTestsIt)
{
	EXPECT_TRUE(tgm_acquire_blocked(0x4, 5031, 5032));
	EXPECT_FALSE(tgm_acquire_blocked(0x4, 5032, 5032));
	EXPECT_FALSE(tgm_acquire_blocked(0x4, 2, 0xfffffffeu));  // a fence word wrapping past zero
	EXPECT_TRUE(tgm_acquire_blocked(0x1, 6, 7));
	EXPECT_FALSE(tgm_acquire_blocked(0x1, 7, 7));
	EXPECT_TRUE(tgm_acquire_blocked(0x8, 0x2, 0x5));
	EXPECT_FALSE(tgm_acquire_blocked(0x8, 0x4, 0x5));
	EXPECT_FALSE(tgm_acquire_blocked(0x2, 0, 7));  // a release waits on nothing
}

TEST(Offset, InsideTheFenceViewOnly)
{
	EXPECT_EQ(tgm_acquire_offset(0x8000104040ull, 0x8000100000ull, 0x104000), 0x4040);
	EXPECT_EQ(tgm_acquire_offset(0x8000100000ull - 4, 0x8000100000ull, 0x104000), -1);
	EXPECT_EQ(tgm_acquire_offset(0x8000204000ull, 0x8000100000ull, 0x104000), -1);
	EXPECT_EQ(tgm_acquire_offset(0x40, 0, 0x104000),
		  -1);  // an address space with no fence view
}
}  // namespace

// The ACQUIRE probe's verdict on a park past TGM_ACQUIRE_NS with no interrupt: on PBDMA0 at the acquire with the timeout
// armed, the interrupt was lost; switched out on it (CCSR pending_acquire), the PBDMA was not timing it.
TEST(Probe, OnThePbdmaWithItsTimeoutArmedIsALostInterruptAndSwitchedOutIsNot)
{
	const uint32_t armed = tgm_acquire_word(tgm_acquire_ptimer_ns(1000000000ull));
	const uint32_t pendingAcquire = 3u << 24, onEngine = 1u << 24;

	EXPECT_EQ(tgm_acquire_probe_where(5, onEngine, 5, 1, armed), TGM_ACQUIRE_LOST);
	EXPECT_EQ(tgm_acquire_probe_where(5, onEngine, 5, 1, armed & ~TGM_ACQUIRE_TIMEOUT_EN),
		  TGM_ACQUIRE_UNTIMED);
	EXPECT_EQ(tgm_acquire_probe_where(5, pendingAcquire, 7, 1, armed),
		  TGM_ACQUIRE_SWITCHED_OUT);
	EXPECT_EQ(tgm_acquire_probe_where(5, pendingAcquire, 5, 0, armed), TGM_ACQUIRE_SWITCHED_OUT)
		<< "PBDMA0 on the channel but past the acquire: CCSR decides";
	EXPECT_EQ(tgm_acquire_probe_where(5, onEngine, 7, 1, armed), TGM_ACQUIRE_ELSEWHERE);
}
