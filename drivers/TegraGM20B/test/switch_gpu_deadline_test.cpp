// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuJobFences.h"

struct tgm_chan {
	uint32_t gov_done, completed;
};

extern "C" uint32_t tgm_chan_completed(tgm_chan *chan)
{
	return chan->completed;
}

namespace
{
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
constexpr u64 IDLE_DROP_NS = 500ull * 1000 * 1000;

struct {
	int on, low, dropping;
	uint64_t service_until, last_busy;
	u64 kicked, retired;
	void *drop_w, *raise_w, *settle_w;
	int settling;
} tgm_idle;

uint64_t now, last_swap;
unsigned scheduled;

uint64_t nvxnu_nanotime()
{
	return now;
}

uint64_t native_display_last_swap_ns()
{
	return last_swap;
}

void nvxnu_work_schedule(void *)
{
	++scheduled;
}

#define READ_ONCE(value) (value)
#include "drivers/TegraGM20B/SwitchGpuIdleDeadline.h"

class GpuDeadline : public ::testing::Test
{
protected:
	void SetUp() override
	{
		tgm_idle = {};
		tgm_idle.on = 1;
		now = 1000000000;
		last_swap = 0;
		scheduled = 0;
	}
};

TEST_F(GpuDeadline, ClampsServiceWindowAndPreservesExistingHold)
{
	tegra_gm20b_perf_deadline_ns(now + 1000000000);
	EXPECT_EQ(tgm_idle.service_until, now + 20000000);
	tegra_gm20b_perf_deadline_ns(now + 5000000);
	EXPECT_EQ(tgm_idle.service_until, now + 20000000);
	EXPECT_EQ(tgm_idle.last_busy, now);
	EXPECT_EQ(scheduled, 0u);
}

TEST_F(GpuDeadline, IdleAndRacingDropScheduleOnlyExistingRaiseWork)
{
	tgm_idle.low = 1;
	tegra_gm20b_perf_deadline_ns(now + 1000);
	EXPECT_EQ(scheduled, 1u);
	tgm_idle.low = 0;
	tgm_idle.dropping = 1;
	tegra_gm20b_perf_deadline_ns(now + 2000);
	EXPECT_EQ(scheduled, 2u);
}

TEST_F(GpuDeadline, ExpiredAndDisabledRequestsDoNothing)
{
	tegra_gm20b_perf_deadline_ns(now);
	tgm_idle.on = 0;
	tegra_gm20b_perf_deadline_ns(now + 1000);
	EXPECT_EQ(tgm_idle.service_until, 0u);
	EXPECT_EQ(tgm_idle.last_busy, 0u);
	EXPECT_EQ(scheduled, 0u);
}

// One SubmitAddress carrying `batches` work batches, kicked as tgm_chan_fence_job kicks it: every seqno it releases.
u32 kick(u32 submitted, u32 batches)
{
	std::vector<tgm_push> pushes(batches, tgm_push{0x100000, 16, 0, 0, 1, 0});
	pushes.back().fence = 0;
	const switch_gpu_job_emitter emitter = {nullptr, [](void *, u32) {}, [](void *, u32) {}};
	const u32 last = switch_gpu_job_emit(pushes.data(), batches, submitted, &emitter);
	idle_kicked(submitted + 1, last);
	return last;
}

void idleFor(u64 ns)
{
	now += ns;
	tgm_idle.last_busy = now - ns;
}

TEST_F(GpuDeadline, AJobsInternalFencesAllRetireAndTheClockDropsOnce)
{
	tgm_chan chan = {40, 40};
	const u32 last = kick(40, 4);
	ASSERT_EQ(last, 44u);
	EXPECT_EQ(idle_in_flight(), 4u);
	for (u32 seqno = 41; seqno <= last; ++seqno) {
		chan.completed = seqno;
		idle_fenced(&chan);
		EXPECT_EQ(idle_in_flight(), u64(last - seqno)) << seqno;
	}
	idleFor(IDLE_DROP_NS);
	tegra_gm20b_idle_tick();
	EXPECT_EQ(scheduled, 1u);
	EXPECT_EQ(tgm_idle.dropping, 1);
	tegra_gm20b_idle_tick();
	EXPECT_EQ(scheduled, 1u);
}

TEST_F(GpuDeadline, ACompletionSeenOnlyAtTheJobsEndRetiresEveryFence)
{
	tgm_chan chan = {7, 7};
	kick(7, 3);
	const u32 last = kick(10, 28);
	chan.completed = last;
	idle_fenced(&chan);
	EXPECT_EQ(idle_in_flight(), 0u);
	idleFor(IDLE_DROP_NS);
	tegra_gm20b_idle_tick();
	EXPECT_EQ(scheduled, 1u);
}

// Work in flight keeps the run clock; with no kick for the drop time the tick settles it once (a quiet job's end may have
// raised no interrupt to count it), and drops nothing.
TEST_F(GpuDeadline, WorkInFlightOrARecentSwapKeepsTheRunClock)
{
	tgm_chan chan = {0, 0};
	kick(0, 3);
	chan.completed = 2;
	idle_fenced(&chan);
	idleFor(IDLE_DROP_NS / 2);
	tegra_gm20b_idle_tick();
	EXPECT_EQ(scheduled, 0u);
	idleFor(IDLE_DROP_NS);
	tegra_gm20b_idle_tick();
	tegra_gm20b_idle_tick();
	EXPECT_EQ(scheduled, 1u);
	EXPECT_EQ(tgm_idle.settling, 1);
	EXPECT_EQ(tgm_idle.dropping, 0);
	chan.completed = 3;
	idle_fenced(&chan);
	last_swap = now - IDLE_DROP_NS / 2;
	tegra_gm20b_idle_tick();
	EXPECT_EQ(scheduled, 1u);
}

TEST_F(GpuDeadline, TeardownAndALatePassRetireADeadChannelOnce)
{
	tgm_chan chan = {100, 100};
	const u32 last = kick(100, 5);
	chan.completed = 102;
	idle_fenced(&chan);
	idle_retire(&chan, last);
	EXPECT_EQ(idle_in_flight(), 0u);
	chan.completed = last;  // tgm_chan_killed releases the whole sequence
	idle_fenced(&chan);
	idle_retire(&chan, last);
	EXPECT_EQ(idle_in_flight(), 0u);
	EXPECT_EQ(tgm_idle.retired, 5u);
}

TEST_F(GpuDeadline, TheSequenceWrapsWithoutLosingCount)
{
	tgm_chan chan = {UINT32_MAX - 1, UINT32_MAX - 1};
	const u32 last = kick(UINT32_MAX - 1, 4);
	EXPECT_EQ(last, 2u);
	chan.completed = 0;
	idle_fenced(&chan);
	EXPECT_EQ(idle_in_flight(), 2u);
	chan.completed = last;
	idle_fenced(&chan);
	EXPECT_EQ(idle_in_flight(), 0u);
}
}  // namespace
