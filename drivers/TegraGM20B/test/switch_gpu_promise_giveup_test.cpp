// SPDX-License-Identifier: MIT
// A writer promise nobody keeps, given up once (SwitchGpuPromiseGiveUp.h), run as the kernel builds it:
// gpu_promise_giveup.c whole (:promise_giveup_source) and gpu_promise.c's GPU-reader park (:promise_note_source),
// under switch_gpu_promise_giveup_stubs.h's clock, call, fence pool and channels. Each GPU reader kind parks the way
// its submission does (tgm_chan_note_promise after the acquires are kicked); every one is given up about a second after
// it parked, with one line, and its reader goes on with the frame before the promised one.
#include "switch_gpu_promise_giveup_fixture.h"

namespace
{
using namespace promise_giveup;

// The reader parks; nobody keeps the promise. A second later the call gives it up: marked abandoned, the word raised
// (the reader's acquire passes, its job completes), one line naming the maker, the reader and how long it waited. The
// raw untimed wait (10 s contract) and every other reader are let go at 1 s, never at a hang check.
TEST_P(GiveUp, IsGivenUpOnceAboutOneSecondAfterThePark)
{
	readerSubmits();
	EXPECT_EQ(fake::call.armed, 1u);
	until(fake::now + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_NE(reader.fence, reader.submitted);
	until(fake::now + 1 * kMs);
	const uint64_t second = fake::now;
	EXPECT_EQ(lines.size(), 1u) << "given up a second after the park";
	until(second + 9000 * kMs);
	ASSERT_EQ(lines.size(), 1u) << "given up within the raw wait's 10 s contract";
	const std::string expect =
		std::string(
			"tegra-gm20b: maker-app's writer promise at fence offset 2052 (value 9) "
			"was not kept: ") +
		GetParam().owner + " waited 1000 ms for it in " +
		(GetParam().present ? "a present job" : "a GPU job");
	EXPECT_EQ(lines[0].substr(0, expect.size()), expect);
	EXPECT_EQ(fake::pool[kOffset / 4], kValue);
	EXPECT_EQ(reader.fence, reader.submitted) << "the reader's acquire passed";
	EXPECT_EQ(status(), -5);
	EXPECT_EQ(shown(), kPrevious);
	EXPECT_EQ(fake::pathAbandoned, 1u);
	EXPECT_EQ(fake::cpuRaised, 1u);
	EXPECT_EQ(probedReaders, 1u)
		<< "the ACQUIRE probe looks at the reader still parked at the give-up";
	EXPECT_FALSE(fake::call.pending) << "nothing left to wait on";
	until(second + 20000 * kMs);
	EXPECT_EQ(lines.size(), 1u);
	EXPECT_EQ(fake::call.ran, 1u);
}

// An ordinary kept promise: armed once at the park, cancelled by the keeping job's interrupt pass, never run.
TEST_P(GiveUp, AKeptPromiseCancelsTheCallWhichNeverRuns)
{
	readerSubmits();
	makerKeeps(0);
	EXPECT_EQ(maker.asked, std::vector<uint32_t>{maker.submitted}) << "its end is asked for";
	until(fake::now + 300 * kMs);
	makerReleases();
	gpuRuns();
	EXPECT_EQ(fake::call.armed, 1u);
	EXPECT_EQ(fake::call.cancelled, 1u);
	until(fake::now + 5000 * kMs);
	EXPECT_EQ(fake::call.ran, 0u);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(shown(), kPromised);
}

// The reader goes away before the second is up (its channel killed): the call drops the promise, untold and unraised.
TEST_P(GiveUp, AReaderThatLeftIsNotWaitedForAndNothingIsGivenUp)
{
	readerSubmits();
	reader.error = 1;
	until(fake::now + 1000 * kMs);
	EXPECT_EQ(fake::call.ran, 1u);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], kValue - 1);
	EXPECT_EQ(status(), 0) << "a later keep still shows its frame";
	EXPECT_FALSE(fake::call.pending);
}

// The maker's GPU release lands between the mark and the raise: its writes are there, so the mark is taken back and
// nothing is reported.
TEST_P(GiveUp, TheMakersReleaseRacingTheGiveUpShowsItsFrame)
{
	readerSubmits();
	until(fake::now + 999 * kMs);

	struct Race {
		static void release(uint32_t offset, uint32_t value, int step, void *entry)
		{
			if (step == SWITCH_GPU_PROMISE_RAISING)
				fake::pool[offset / 4] = value;
			giveup_stepped(offset, value, step, entry);
		}
	};

	switch_gpu_giveup_ops racing = giveup_ops;
	racing.stepped = Race::release;
	fake::now += 1 * kMs;
	switch_gpu_giveup_run(&giveups, &racing, fake::now);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(status(), 1);
	EXPECT_EQ(shown(), kPromised);
	EXPECT_EQ(fake::cpuRaised, 0u);
}

// The maker closed and its fence page went to another connection, cleared: the word is not the promise's any more. It is
// dropped, never raised into the new owner's word, and nothing is told.
TEST_P(GiveUp, AWordWhosePageChangedOwnerIsDroppedNeverRaised)
{
	readerSubmits();
	++fake::owner;
	fake::pool[kOffset / 4] = 0;
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], 0u);
	EXPECT_TRUE(fake::abandoned.empty());
	EXPECT_FALSE(fake::call.pending);
}

// The promised job is submitted behind another job of its channel still running. At the second it is not given up: it
// waits for that job (its interrupt asked for), and from that job's completion the promised job has its own second.
TEST_P(GiveUp, AJobQueuedBehindAnotherIsJudgedFromThatJobsCompletion)
{
	readerSubmits();
	makerKeeps(1);
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty());
	ASSERT_FALSE(maker.asked.empty());
	EXPECT_EQ(maker.asked.back(), maker.submitted - 1)
		<< "the job ahead's interrupt is asked for";
	until(fake::now + 1500 * kMs);
	EXPECT_TRUE(lines.empty());
	maker.fence = maker.submitted - 1;
	const uint64_t ahead_done = fake::now;
	pass();
	EXPECT_TRUE(fake::call.pending);
	EXPECT_EQ(fake::call.due, ahead_done) << "the pass runs the call now";
	until(ahead_done + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(ahead_done + 1000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find("waited 3500 ms"), std::string::npos) << lines[0];
	EXPECT_EQ(shown(), kPrevious);
}

// The job ahead never completes: the hang rule's, not this rule's. The call is that channel's waiter on the hang rule's
// own two strikes; the recovery (here an engine reset that lets the job ahead complete) hands the promised job back,
// judged from its own start.
TEST_P(GiveUp, AStuckJobAheadIsTheHangRulesAndThePromisedJobIsJudgedFromItsOwnStart)
{
	readerSubmits();
	makerKeeps(1);
	maker.recovery = 2;
	until(fake::now + 1000 * kMs);
	const uint64_t deferred = fake::now;
	EXPECT_EQ(fake::hangLooks, 1u);
	until(deferred + 4000 * kMs);
	EXPECT_EQ(maker.fence, maker.submitted - 1) << "recovered at the second strike";
	const uint64_t start = fake::now;
	EXPECT_TRUE(lines.empty());
	until(start + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(start + 1000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_EQ(shown(), kPrevious);
}

// The hang rule's recovery kills the maker's channel: the promised job died with it, and the promise is given up at the
// next look.
TEST_P(GiveUp, AJobAheadWhoseChannelTheHangRuleKillsGivesThePromiseUp)
{
	readerSubmits();
	makerKeeps(1);
	maker.recovery = 1;
	until(fake::now + 1000 * kMs + 6000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_EQ(maker.error, 1u);
	EXPECT_EQ(shown(), kPrevious);
}

INSTANTIATE_TEST_SUITE_P(Readers, GiveUp, testing::ValuesIn(kKinds),
			 [](const testing::TestParamInfo<Kind> &info) {
				 return std::string(info.param.name);
			 });

// Waiters with no GPU job: each parks itself and leaves itself, at the keep included (the direct present then watches
// the job; a CPU reader then waits on the job's fence).
struct Waiter {
	const char *name;
	const char *waiter, *who;
	int display;
};

const Waiter kWaiters[] = {
	{"DirectPresentWatch", "the display", "a direct present", 1},
	{"CpuLockIOSurface", "SpringBoard", "a CPU lock", 0},
	{"CpuLeaseDisplayCopy", "backboardd", "a CPU copy", 0},
};

class Leaves : public Fixture, public testing::WithParamInterface<Waiter>
{
protected:
	int park()
	{
		return tgm_promise_park(kOffset, kValue, GetParam().waiter, GetParam().who, nullptr,
					GetParam().display);
	}

	void leave()
	{
		tgm_promise_leave(kOffset, kValue, GetParam().display);
	}

	const Waiter &waiter() const
	{
		return GetParam();
	}
};

TEST_P(Leaves, IsGivenUpOnceAboutOneSecondAfterThePark)
{
	ASSERT_EQ(park(), 1);
	until(fake::now + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(fake::now + 1 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	const std::string expect =
		std::string(
			"tegra-gm20b: maker-app's writer promise at fence offset 2052 (value 9) "
			"was not kept: ") +
		waiter().waiter + " waited 1000 ms for it in " + waiter().who;
	EXPECT_EQ(lines[0].substr(0, expect.size()), expect);
	EXPECT_EQ(status(), -5);
	EXPECT_EQ(shown(), kPrevious);
	EXPECT_EQ(fake::directGivenUp, unsigned(waiter().display));
	leave();
	until(fake::now + 10000 * kMs);
	EXPECT_EQ(lines.size(), 1u);
}

// Kept: the waiter leaves at the keep (its wait is on the job from then), which cancels the call.
TEST_P(Leaves, AKeptPromiseCancelsTheCallWhichNeverRuns)
{
	ASSERT_EQ(park(), 1);
	makerKeeps(0);
	leave();
	makerReleases();
	until(fake::now + 5000 * kMs);
	EXPECT_EQ(fake::call.armed, 1u);
	EXPECT_EQ(fake::call.cancelled, 1u);
	EXPECT_EQ(fake::call.ran, 0u);
	EXPECT_TRUE(lines.empty());
}

TEST_P(Leaves, TheLastWaiterLeavingCancelsTheCall)
{
	ASSERT_EQ(park(), 1);
	ASSERT_EQ(park(), 1);
	leave();
	EXPECT_EQ(fake::call.cancelled, 0u);
	leave();
	EXPECT_EQ(fake::call.cancelled, 1u);
	until(fake::now + 5000 * kMs);
	EXPECT_EQ(fake::call.ran, 0u);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(status(), 0);
}

TEST_P(Leaves, TheMakersReleaseRacingTheGiveUpShowsItsFrame)
{
	ASSERT_EQ(park(), 1);

	struct Race {
		static void release(uint32_t offset, uint32_t value, int step, void *entry)
		{
			if (step == SWITCH_GPU_PROMISE_RAISING)
				fake::pool[offset / 4] = value;
			giveup_stepped(offset, value, step, entry);
		}
	};

	switch_gpu_giveup_ops racing = giveup_ops;
	racing.stepped = Race::release;
	fake::now += 1000 * kMs;
	switch_gpu_giveup_run(&giveups, &racing, fake::now);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(status(), 1);
}

TEST_P(Leaves, AWordWhosePageChangedOwnerIsDroppedNeverRaised)
{
	ASSERT_EQ(park(), 1);
	++fake::owner;
	fake::pool[kOffset / 4] = 0;
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], 0u);
	EXPECT_TRUE(fake::abandoned.empty());
}

// The promised job behind another, or stuck behind one: the waiter left at the keep, so the job is the hang rule's (the
// present's deadline from the keep, the CPU wait's own looks), and nothing is given up here.
TEST_P(Leaves, AJobBehindAnotherIsNotThisWaitersPromiseAnyMore)
{
	ASSERT_EQ(park(), 1);
	makerKeeps(1);
	maker.recovery = 2;
	leave();
	until(fake::now + 10000 * kMs);
	EXPECT_EQ(fake::call.ran, 0u);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::hangLooks, 0u);
}

// The word moves with no keep the waiter heard of: the pass runs the call, which hands the direct present its frame.
TEST_P(Leaves, AWordMovedUnheardIsHandedToTheDirectPresent)
{
	ASSERT_EQ(park(), 1);
	fake::pool[kOffset / 4] = kValue;
	pass();
	until(fake::now);
	EXPECT_EQ(presentReleased, unsigned(waiter().display));
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(status(), 1);
}

INSTANTIATE_TEST_SUITE_P(Waiters, Leaves, testing::ValuesIn(kWaiters),
			 [](const testing::TestParamInfo<Waiter> &info) {
				 return std::string(info.param.name);
			 });
}  // namespace
