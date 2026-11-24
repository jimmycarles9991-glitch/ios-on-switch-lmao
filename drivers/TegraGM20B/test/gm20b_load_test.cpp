// TegraGM20BLoad.h on the host: the PMU idle counters' program (nvgpu's raw load pair) and their reads extended to 64
// bits, an interval counted only when it cannot hide a wrap.
#include <gtest/gtest.h>

#include "drivers/TegraGM20B/TegraGM20BLoad.h"

namespace
{

constexpr uint64_t kMs = 1000000;

TEST(Gm20bLoad, TheProgramIsNvgpusRawLoadPair)
{
	// gk20a_pmu_init_perfmon_counter: idle_mask(1) = GR | CE2, idle_ctrl(1) busy and idle_ctrl(2) always, unfiltered
	// (hw_pwr_gm20b.h: mask 0x10a504 + 16i, count 0x10a508 + 16i, ctrl 0x10a50c + 16i).
	EXPECT_EQ(TGM_LOAD_MASK(TGM_LOAD_BUSY), 0x10a514u);
	EXPECT_EQ(TGM_LOAD_COUNT(TGM_LOAD_BUSY), 0x10a518u);
	EXPECT_EQ(TGM_LOAD_COUNT(TGM_LOAD_ALWAYS), 0x10a528u);
	EXPECT_EQ(TGM_LOAD_CTRL(TGM_LOAD_ALWAYS), 0x10a52cu);
	EXPECT_EQ(tgm_load_ctrl(0xffffffffu, TGM_LOAD_CTRL_BUSY), 0xfffffffau)
		<< "value busy, filter off, rest kept";
	EXPECT_EQ(tgm_load_ctrl(0x00000004u, TGM_LOAD_CTRL_ALWAYS), 0x00000003u);
	EXPECT_TRUE(tgm_load_armed(TGM_LOAD_GR_CE2));
	EXPECT_FALSE(tgm_load_armed(0)) << "a power cycle loses the program";
}

TEST(Gm20bLoad, ReadsAddWhatPassedBetweenThem)
{
	tgm_load l = {};
	EXPECT_FALSE(tgm_load_step(&l, 100 * kMs, 0, 0, 1)) << "armed by this read: start here";
	EXPECT_TRUE(tgm_load_step(&l, 116 * kMs, 1000, 3264000, 0));
	EXPECT_EQ(l.busy, 1000u);
	EXPECT_EQ(l.total, 3264000u);
	EXPECT_EQ(l.counted_ns, 16 * kMs);
	EXPECT_EQ(l.rate_khz, 204000u);
	EXPECT_EQ(l.reads, 2u);
	EXPECT_EQ(l.restarts, 0u);
}

TEST(Gm20bLoad, ACounterWrapIsCarried)
{
	tgm_load l = {};
	tgm_load_step(&l, 100 * kMs, 0x7fffff00u, 0x7ffff000u, 0);
	EXPECT_TRUE(tgm_load_step(&l, 101 * kMs, 0x00000100u, 0x00001000u, 0));
	EXPECT_EQ(l.busy, 0x200u);
	EXPECT_EQ(l.total, 0x2000u);
}

TEST(Gm20bLoad, AGapThatCouldHideAWrapStartsOver)
{
	tgm_load l = {};
	tgm_load_step(&l, 100 * kMs, 0, 0, 0);
	// Before any rate is seen only 2 s is trusted (2^31 cycles of a clock up to 1 GHz).
	EXPECT_FALSE(tgm_load_step(&l, 2200 * kMs, 5, 10, 0));
	EXPECT_EQ(l.total, 0u);
	EXPECT_EQ(l.restarts, 1u);
	// At 204 MHz 2^31 cycles are 10.5 s: 5 s is counted, 10 s is not.
	EXPECT_TRUE(tgm_load_step(&l, 2300 * kMs, 5, 10 + 20400000, 0));
	EXPECT_TRUE(tgm_load_step(&l, 7300 * kMs, 5, 10 + 20400000 + 1020000000, 0));
	EXPECT_FALSE(tgm_load_step(&l, 17300 * kMs, 5, 10, 0));
	EXPECT_EQ(l.restarts, 2u);
}

TEST(Gm20bLoad, APowerCycleStartsOver)
{
	tgm_load l = {};
	tgm_load_step(&l, 100 * kMs, 5000, 9000, 0);
	// The counters restart from zero, armed again: no difference across the cycle is counted.
	EXPECT_FALSE(tgm_load_step(&l, 110 * kMs, 0, 0, 1));
	EXPECT_EQ(l.total, 0u);
	// A busy count past the total cannot be: the counters moved under us.
	EXPECT_FALSE(tgm_load_step(&l, 120 * kMs, 500, 100, 0));
	EXPECT_EQ(l.total, 0u);
}

TEST(Gm20bLoad, ASubmissionReadsOnlyWhenNothingReadForASecond)
{
	EXPECT_TRUE(tgm_load_due(0, 5 * kMs)) << "no read yet";
	EXPECT_FALSE(tgm_load_due(100 * kMs, 1099 * kMs))
		<< "the display's frame end read 999 ms ago";
	EXPECT_TRUE(tgm_load_due(100 * kMs, 1100 * kMs));
}

TEST(Gm20bLoad, AWindowIsTheDifferenceOfTwoCopies)
{
	tgm_load was = {};
	tgm_load now = {};
	now.busy = 250;
	now.total = 1000;
	now.counted_ns = 4000 * kMs;
	now.reads = 300;
	now.restarts = 1;
	now.read_ns = 300 * 1500;
	now.rate_khz = 204000;
	const tgm_load_window w = tgm_load_between(&was, &now, 5000 * kMs);
	EXPECT_EQ(w.busy_permille, 250);
	EXPECT_EQ(w.counted_permille, 800);
	EXPECT_EQ(w.reads, 300u);
	EXPECT_EQ(w.restarts, 1u);
	EXPECT_EQ(w.read_ns_each, 1500u);
	EXPECT_EQ(tgm_load_between(&now, &now, 5000 * kMs).busy_permille, -1)
		<< "nothing counted is not 0% busy";
}

}  // namespace
