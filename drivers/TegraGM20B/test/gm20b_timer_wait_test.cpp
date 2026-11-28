// SPDX-License-Identifier: MIT
#include <errno.h>
#include <gtest/gtest.h>
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t s64;

#define NSEC_PER_USEC 1000ULL
#define NSEC_PER_MSEC 1000000ULL

namespace
{
u64 gTimeNs = 0;
u64 gReadStepNs = 0;
int gCanSleep = 1;
unsigned gRelaxes = 0;
unsigned gSleeps = 0;
unsigned gFatals = 0;
u64 gLastSleepLo = 0;
u64 gLastSleepHi = 0;
u64 gTotalSleepUs = 0;
}  // namespace

struct nvkm_subdev {
	int unused;
};

struct nvkm_timer {
	struct nvkm_subdev subdev;
};

struct nvkm_timer_wait {
	struct nvkm_timer *tmr;
	u64 limit;
	u64 time0;
	u64 time1;
	int reads;
};

static u64 nvkm_timer_read(struct nvkm_timer *)
{
	gTimeNs += gReadStepNs;
	return gTimeNs;
}

static int nvxnu_can_sleep(void)
{
	return gCanSleep;
}

static void cpu_relax(void)
{
	++gRelaxes;
}

static void usleep_range(unsigned long lo, unsigned long hi)
{
	++gSleeps;
	gLastSleepLo = lo;
	gLastSleepHi = hi;
	gTotalSleepUs += lo;
	gTimeNs += lo * NSEC_PER_USEC;
}

#define nvkm_fatal(subdev, ...) \
	do {                    \
		(void)(subdev); \
		++gFatals;      \
	} while (0)

#include "drivers/TegraGM20B/test/timer_wait_source.h"

namespace
{
class TimerWaitTest : public testing::Test
{
protected:
	nvkm_timer timer{};

	void SetUp() override
	{
		gTimeNs = 1000000;
		gReadStepNs = 0;
		gCanSleep = 1;
		gRelaxes = 0;
		gSleeps = 0;
		gFatals = 0;
		gLastSleepLo = 0;
		gLastSleepHi = 0;
		gTotalSleepUs = 0;
	}

	nvkm_timer_wait makeWait(u64 limitNs)
	{
		nvkm_timer_wait wait{};
		wait.tmr = &timer;
		wait.limit = limitNs;
		wait.reads = 0;
		return wait;
	}
};
}  // namespace

TEST_F(TimerWaitTest, FastCompletionSpinsWithoutSleeping)
{
	gReadStepNs = 1000;
	auto wait = makeWait(2000ULL * NSEC_PER_MSEC);
	for (int i = 0; i < 8; ++i)
		ASSERT_GE(nvkm_timer_wait_test(&wait), 0);
	EXPECT_EQ(gSleeps, 0u);
	EXPECT_EQ(gRelaxes, 8u);
	EXPECT_EQ(gFatals, 0u);
}

TEST_F(TimerWaitTest, ShortDelayNeverSleeps)
{
	gReadStepNs = 2000;
	auto wait = makeWait(10ULL * NSEC_PER_USEC);
	s64 res = 0;
	while ((res = nvkm_timer_wait_test(&wait)) >= 0) {
	}
	EXPECT_EQ(res, -ETIMEDOUT);
	EXPECT_EQ(gSleeps, 0u);
	EXPECT_GT(gRelaxes, 0u);
}

TEST_F(TimerWaitTest, SlowSleepableWaitBacksOffAndTimesOutWithoutBusySpin)
{
	gReadStepNs = 2000;
	auto wait = makeWait(2000ULL * NSEC_PER_MSEC);
	unsigned iterations = 0;
	s64 res = 0;
	while ((res = nvkm_timer_wait_test(&wait)) >= 0)
		++iterations;
	EXPECT_EQ(res, -ETIMEDOUT);
	EXPECT_EQ(gFatals, 0u);
	EXPECT_GT(gRelaxes, 0u);
	EXPECT_GT(gSleeps, 100u);
	EXPECT_LT(iterations, 3000u);
	EXPECT_GE(gTotalSleepUs, 1990000ULL);
	EXPECT_LE(gLastSleepLo, gLastSleepHi);
}

TEST_F(TimerWaitTest, NonSleepableContextNeverSleeps)
{
	gCanSleep = 0;
	gReadStepNs = 50000;
	auto wait = makeWait(2ULL * NSEC_PER_MSEC);
	s64 res = 0;
	while ((res = nvkm_timer_wait_test(&wait)) >= 0) {
	}
	EXPECT_EQ(res, -ETIMEDOUT);
	EXPECT_EQ(gSleeps, 0u);
	EXPECT_GT(gRelaxes, 20u);
	EXPECT_EQ(gFatals, 0u);
}

TEST_F(TimerWaitTest, StalledTimerTripsFatalWithoutSleeping)
{
	gReadStepNs = 0;
	auto wait = makeWait(2000ULL * NSEC_PER_MSEC);
	s64 res = 0;
	unsigned iterations = 0;
	while ((res = nvkm_timer_wait_test(&wait)) >= 0)
		++iterations;
	EXPECT_EQ(res, -ETIMEDOUT);
	EXPECT_EQ(iterations, 16u);
	EXPECT_EQ(gSleeps, 0u);
	EXPECT_EQ(gFatals, 1u);
}
