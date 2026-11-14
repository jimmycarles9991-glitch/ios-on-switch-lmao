// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <cstdint>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuBusy.h"
#include "drivers/TegraGM20B/SwitchGpuJobFences.h"

switch_gpu_busy tgm_busy;

namespace
{
using u32 = uint32_t;
using u64 = uint64_t;
using s32 = int32_t;
constexpr u64 IDLE_DROP_NS = 500ull * 1000 * 1000;

struct {
	int on, low, dropping;
	uint64_t service_until, last_busy;
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
		tgm_busy = {};
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
constexpr u32 kChid = 9;

u32 kick(u32 submitted, u32 batches)
{
	std::vector<tgm_push> pushes(batches, tgm_push{0x100000, 16, 0, 0, 1, 0});
	pushes.back().fence = 0;
	const switch_gpu_job_emitter emitter = {nullptr, [](void *, u32) {}, [](void *, u32) {}};
	const u32 last = switch_gpu_job_emit(pushes.data(), batches, submitted, &emitter);
	switch_gpu_busy_kick(&tgm_busy, kChid, last);
	return last;
}

void idleFor(u64 ns)
{
	now += ns;
	tgm_idle.last_busy = now - ns;
}

TEST_F(GpuDeadline, AJobsInternalFencesAllRetireAndTheClockDropsOnce)
{
	switch_gpu_busy_open(&tgm_busy, kChid, 40);
	const u32 last = kick(40, 4);
	ASSERT_EQ(last, 44u);
	EXPECT_EQ(idle_in_flight(), 1);
	for (u32 seqno = 41; seqno <= last; ++seqno) {
		switch_gpu_busy_done(&tgm_busy, kChid, seqno);
		EXPECT_EQ(idle_in_flight(), seqno == last ? 0 : 1) << seqno;
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
	switch_gpu_busy_open(&tgm_busy, kChid, 7);
	kick(7, 3);
	const u32 last = kick(10, 28);
	switch_gpu_busy_done(&tgm_busy, kChid, last);
	EXPECT_EQ(idle_in_flight(), 0);
	idleFor(IDLE_DROP_NS);
	tegra_gm20b_idle_tick();
	EXPECT_EQ(scheduled, 1u);
}

// Work in flight keeps the run clock; with no kick for the drop time the tick settles it once (a quiet job's end may have
// raised no interrupt to count it), and drops nothing.
TEST_F(GpuDeadline, WorkInFlightOrARecentSwapKeepsTheRunClock)
{
	switch_gpu_busy_open(&tgm_busy, kChid, 0);
	kick(0, 3);
	switch_gpu_busy_done(&tgm_busy, kChid, 2);
	idleFor(IDLE_DROP_NS / 2);
	tegra_gm20b_idle_tick();
	EXPECT_EQ(scheduled, 0u);
	idleFor(IDLE_DROP_NS);
	tegra_gm20b_idle_tick();
	tegra_gm20b_idle_tick();
	EXPECT_EQ(scheduled, 1u);
	EXPECT_EQ(tgm_idle.settling, 1);
	EXPECT_EQ(tgm_idle.dropping, 0);
	switch_gpu_busy_settle(&tgm_busy, kChid,
			       3);  // the settle read the quiet job's end from its fence
	last_swap = now - IDLE_DROP_NS / 2;
	tegra_gm20b_idle_tick();
	EXPECT_EQ(scheduled, 1u);
}

// A torn-down channel counts no more, whatever its fence; a late pass of it changes nothing.
TEST_F(GpuDeadline, TeardownForgetsADeadChannel)
{
	switch_gpu_busy_open(&tgm_busy, kChid, 100);
	kick(100, 5);
	switch_gpu_busy_done(&tgm_busy, kChid, 102);
	EXPECT_EQ(idle_in_flight(), 1);
	switch_gpu_busy_close(&tgm_busy, kChid);
	EXPECT_EQ(idle_in_flight(), 0);
	switch_gpu_busy_done(&tgm_busy, kChid, 103);
	EXPECT_EQ(idle_in_flight(), 0);
}

TEST_F(GpuDeadline, TheSequenceWraps)
{
	switch_gpu_busy_open(&tgm_busy, kChid, UINT32_MAX - 1);
	const u32 last = kick(UINT32_MAX - 1, 4);
	EXPECT_EQ(last, 2u);
	switch_gpu_busy_done(&tgm_busy, kChid, 0);
	EXPECT_EQ(idle_in_flight(), 1);
	switch_gpu_busy_done(&tgm_busy, kChid, last);
	EXPECT_EQ(idle_in_flight(), 0);
}
}  // namespace
