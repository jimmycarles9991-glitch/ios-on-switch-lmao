// SPDX-License-Identifier: MIT
// A writer promise nobody keeps, given up once (SwitchGpuPromiseGiveUp.h), run as the kernel builds it:
// gpu_promise_giveup.c whole (:promise_giveup_source) and gpu_promise.c's GPU-reader park (:promise_park_source),
// under switch_gpu_promise_giveup_stubs.h's clock, call, fence pool and channels. Each GPU reader kind parks the way
// its submission does (switch_gpu_chan_park_promises_locked after the acquires are kicked). Every unkept promise is
// given up at its bound whether its maker runs or not, its holder's armed one or else TegraGM20BPromiseBoundNs from its
// first reader's park, with one line, and its reader goes on with the frame before the promised one; a kept promise is
// never given up by time.
#include "switch_gpu_promise_giveup_fixture.h"

namespace
{
using namespace promise_giveup;

// The holder stopped right after its commit's make, before its submit worker armed anything (iOS suspended it, a
// debugger stopped it): the reader parks on a promise nobody armed. The default bound after the park gives it up as an
// armed one does: marked abandoned, the word raised (the reader's acquire passes), one line, the frame before shown.
// Nothing armed it to fail, so when the holder runs again its keep is accepted and its job runs.
TEST_P(GiveUp, AStoppedHolderBeforeItsArmIsGivenUpAtTheDefaultBound)
{
	readerSubmits();
	const uint64_t parked = fake::now;
	until(parked + 4999 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_NE(reader.fence, reader.submitted);
	until(parked + 5000 * kMs);
	ASSERT_EQ(lines.size(), 1u)
		<< "a reader of a promise nobody armed still waits for its holder";
	const std::string expect =
		"tegra-gm20b: maker-app's writer promise at fence offset " +
		std::to_string(kOffset) + " (value 9) was not kept: " + GetParam().owner +
		" waited 5000 ms for it in " + (GetParam().present ? "a present job" : "a GPU job");
	EXPECT_EQ(lines[0].substr(0, expect.size()), expect);
	EXPECT_EQ(fake::pool[kOffset / 4], kValue);
	EXPECT_EQ(reader.fence, reader.submitted) << "the reader's acquire passed";
	EXPECT_EQ(status(), -5);
	EXPECT_EQ(shown(), kPrevious);
	EXPECT_FALSE(fake::call.pending);
	EXPECT_EQ(tgm_promise_unhold(kOffset, kValue), 0) << "the holder's late keep is accepted";
	makerKeeps(0);
	makerReleases();
	until(parked + 20000 * kMs);
	EXPECT_EQ(lines.size(), 1u);
}

// A maker that runs withdraws before the bound: its reader goes on at the withdraw, and nothing is given up or told
TEST_P(GiveUp, AWithdrawBeforeTheDefaultBoundLetsItsReaderGo)
{
	readerSubmits();
	until(fake::now + 4999 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_NE(reader.fence, reader.submitted);
	makerRetires();
	EXPECT_EQ(reader.fence, reader.submitted) << "the reader's acquire passed at the withdraw";
	EXPECT_EQ(status(), -5);
	EXPECT_EQ(shown(), kPrevious);
	until(fake::now + 10000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_FALSE(fake::call.pending);
}

// An arm after the reader parked replaces the default bound with the holder's own
TEST_P(GiveUp, AnArmReplacesTheDefaultBound)
{
	readerSubmits();
	const uint64_t parked = fake::now;
	until(parked + 1000 * kMs);
	holderArms(10000);
	until(parked + 10999 * kMs);
	EXPECT_TRUE(lines.empty())
		<< "given up at the default bound after the holder armed its own";
	until(parked + 11000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find("waited 11000 ms"), std::string::npos) << lines[0];
}

// A keep at the instant of the default bound, taken first under the call's simple lock, ends the bound: the call due
// then gives nothing up
TEST_F(Fixture, AKeepAtTheDefaultBoundEndsItBeforeTheCall)
{
	readerSubmits();
	fake::now += 5000 * kMs;
	EXPECT_EQ(tgm_promise_unhold(kOffset, kValue), 0);
	until(fake::now);
	EXPECT_TRUE(lines.empty()) << "given up after its keep";
	EXPECT_EQ(fake::pool[kOffset / 4], kValue - 1);
	EXPECT_FALSE(fake::call.pending);
	makerKeeps(0);
	makerReleases();
	gpuRuns();
	EXPECT_EQ(shown(), kPromised);
}

// The job that was to keep the promise is refused after its keep (a stale view, a busy surface): the promise is unkept
// again and its bound runs again from the first reader's park, so a holder that stops before it withdraws still lets
// its reader go
TEST_F(Fixture, AJobRefusedAfterItsKeepGivesThePromiseItsBoundAgain)
{
	readerSubmits();
	const uint64_t parked = fake::now;
	until(parked + 1000 * kMs);
	EXPECT_EQ(tgm_promise_unhold(kOffset, kValue), 0);
	until(parked + 6000 * kMs);
	EXPECT_TRUE(lines.empty()) << "given up while its keep was under way";
	tgm_promise_rehold(kOffset, kValue);
	until(fake::now);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find("waited 6000 ms"), std::string::npos) << lines[0];
	EXPECT_EQ(reader.fence, reader.submitted);
}

// The holder armed Metal's bound when its wait became its queue's head, then stopped running (an app iOS suspended
// mid-hold). At the bound the call gives the promise up: marked abandoned, the word raised (the reader's acquire passes,
// its job completes), one line naming the maker, the reader and how long it waited. A keep that comes later is refused,
// and the holder's own Withdraw, once it runs again, tells nothing more.
TEST_P(GiveUp, AStoppedHoldersPromiseIsGivenUpAtItsBound)
{
	readerSubmits();
	holderArms(5000);
	EXPECT_EQ(fake::call.armed, 1u);
	const uint64_t parked = fake::now;
	until(parked + 4999 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_NE(reader.fence, reader.submitted);
	until(parked + 5000 * kMs);
	ASSERT_EQ(lines.size(), 1u) << "given up at the holder's bound";
	const std::string expect =
		"tegra-gm20b: maker-app's writer promise at fence offset " +
		std::to_string(kOffset) + " (value 9) was not kept: " + GetParam().owner +
		" waited 5000 ms for it in " + (GetParam().present ? "a present job" : "a GPU job");
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
	EXPECT_EQ(tgm_promise_unhold(kOffset, kValue), 1) << "a keep after the bound is refused";
	makerRetires();
	until(parked + 20000 * kMs);
	EXPECT_EQ(lines.size(), 1u);
	EXPECT_EQ(fake::call.ran, 1u);
}

// A reader that parks after the armed bound passed, the holder still not running, goes on at once
TEST_P(GiveUp, AReaderThatParksPastTheBoundGoesOnAtOnce)
{
	holderArms(5000);
	EXPECT_EQ(fake::call.armed, 0u) << "nothing armed while no reader waits";
	until(fake::now + 6000 * kMs);
	readerSubmits();
	until(fake::now);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find("waited 0 ms"), std::string::npos) << lines[0];
	EXPECT_EQ(reader.fence, reader.submitted);
	EXPECT_EQ(status(), -5);
}

// The signal reaches the holder before its bound: its keep ends the bound (tgm_promise_unhold under the call's lock),
// the call due at the bound gives nothing up, and the promised job's end shows its frame
TEST_P(GiveUp, AKeepBeforeTheBoundEndsIt)
{
	readerSubmits();
	holderArms(5000);
	until(fake::now + 4999 * kMs);
	EXPECT_EQ(tgm_promise_unhold(kOffset, kValue), 0);
	makerKeeps(0);
	until(fake::now + 1 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], kValue - 1);
	makerReleases();
	gpuRuns();
	until(fake::now + 20000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(shown(), kPromised);
}

// The keep and an armed bound its command buffer fails at, at the same instant: the bound wins. With no reader parked,
// nothing was given up yet, but the keep is refused all the same, and every keep after it, so the command buffer fails
// as Metal fails it there.
TEST_F(Fixture, AKeepAtTheBoundIsRefused)
{
	holderArms(5000);
	fake::now += 5000 * kMs;
	EXPECT_EQ(tgm_promise_unhold(kOffset, kValue), 1);
	EXPECT_EQ(tgm_promise_unhold(kOffset, kValue), 1) << "a second keep is refused too";
}

// A command buffer held behind the head runs once the head fails: its bound (a bound past the head's) gives its readers
// up if the holder cannot run, but a keep after it is not refused
TEST_F(Fixture, ABufferThatRunsAfterItsBoundIsNotRefused)
{
	readerSubmits();
	holderArms(10000, 0);
	until(fake::now + 10000 * kMs);
	EXPECT_EQ(lines.size(), 1u);
	EXPECT_EQ(reader.fence, reader.submitted);
	EXPECT_EQ(tgm_promise_unhold(kOffset, kValue), 0);
}

// The holder dies while armed: its close releases the promise (abandoned, its word raised), and the call due at the bound
// finds the word moved and tells nothing
TEST_P(GiveUp, AHolderThatDiesWhileArmedIsReleasedByItsClose)
{
	readerSubmits();
	holderArms(5000);
	until(fake::now + 1000 * kMs);
	makerRetires();
	EXPECT_EQ(reader.fence, reader.submitted);
	until(fake::now + 10000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(status(), -5);
	EXPECT_FALSE(fake::call.pending);
}

// An ordinary kept promise: armed at the park for its bound and again at the keep for the hang rule's first look,
// cancelled by the keeping job's interrupt pass, never run.
TEST_P(GiveUp, AKeptPromiseCancelsTheCallWhichNeverRuns)
{
	readerSubmits();
	makerKeeps(0);
	EXPECT_EQ(maker.asked, std::vector<uint32_t>{maker.submitted}) << "its end is asked for";
	until(fake::now + 300 * kMs);
	makerReleases();
	gpuRuns();
	EXPECT_EQ(fake::call.armed, 2u);
	EXPECT_EQ(fake::call.cancelled, 1u);
	until(fake::now + 5000 * kMs);
	EXPECT_EQ(fake::call.ran, 0u);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(shown(), kPromised);
}

// The reader goes away before the bound (its channel killed): the call drops the promise, untold and unraised.
TEST_P(GiveUp, AReaderThatLeftIsNotWaitedForAndNothingIsGivenUp)
{
	readerSubmits();
	holderArms(1000);
	reader.error = 1;
	until(fake::now + 1000 * kMs);
	EXPECT_EQ(fake::call.ran, 1u);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], kValue - 1);
	EXPECT_EQ(status(), 0) << "a later keep still shows its frame";
	EXPECT_FALSE(fake::call.pending);
}

// The maker's release lands between the mark and the raise at the bound: its writes are there, so the mark is taken back
// and nothing is reported.
TEST_P(GiveUp, TheMakersReleaseRacingTheGiveUpShowsItsFrame)
{
	readerSubmits();
	holderArms(1000);
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
	holderArms(1000);
	++fake::owner;
	fake::pool[kOffset / 4] = 0;
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], 0u);
	EXPECT_TRUE(fake::abandoned.empty());
	EXPECT_FALSE(fake::call.pending);
}

// The promised job is submitted behind another job of its channel still running. It is never given up: the call waits
// for that job (its interrupt asked for) as the hang rule's waiter, then for the promised job itself, and the promised
// job's end shows its frame.
TEST_P(GiveUp, AJobQueuedBehindAnotherIsWaitedForNeverGivenUp)
{
	readerSubmits();
	makerKeeps(1);
	until(fake::now + 2000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::hangLooks, 1u) << "the hang rule's first look, as the job ahead's waiter";
	ASSERT_FALSE(maker.asked.empty());
	EXPECT_EQ(maker.asked.back(), maker.submitted - 1)
		<< "the job ahead's interrupt is asked for";
	maker.fence = maker.submitted - 1;
	const uint64_t ahead_done = fake::now + 500 * kMs;
	fake::now = ahead_done;
	pass();
	EXPECT_TRUE(fake::call.pending);
	EXPECT_EQ(fake::call.due, ahead_done) << "the pass runs the call now";
	until(ahead_done);
	EXPECT_EQ(maker.asked.back(), maker.submitted) << "then the promised job's own interrupt";
	until(ahead_done + 10000 * kMs);
	EXPECT_TRUE(lines.empty()) << "a kept promise given up by time";
	makerReleases();
	gpuRuns();
	EXPECT_EQ(shown(), kPromised);
	EXPECT_FALSE(fake::call.pending);
}

// The job ahead never completes: the hang rule's. The call is that channel's waiter on the rule's own two strikes; the
// recovery (here an engine reset that lets the job ahead complete) hands the promised job back, never given up. A
// compositor's reader waits so too: no reader kind has a shorter second of its own.
TEST_P(GiveUp, AStuckJobAheadIsTheHangRules)
{
	readerSubmits();
	makerKeeps(1);
	maker.recovery = 2;
	until(fake::now + 10000 * kMs);
	EXPECT_EQ(maker.fence, maker.submitted - 1) << "recovered at the second strike";
	EXPECT_GE(fake::hangLooks, 3u);
	EXPECT_TRUE(lines.empty());
	EXPECT_NE(reader.fence, reader.submitted) << "the reader still waits for the promised job";
	makerReleases();
	gpuRuns();
	EXPECT_EQ(shown(), kPromised);
}

// The hang rule's recovery kills the maker's channel: the promised job died with it, and that kill's retirement releases
// the promise; the call gives nothing up.
TEST_P(GiveUp, AJobAheadWhoseChannelTheHangRuleKillsIsReleasedByTheKillsRetirement)
{
	readerSubmits();
	makerKeeps(1);
	maker.recovery = 1;
	until(fake::now + 7000 * kMs);
	EXPECT_EQ(maker.error, 1u);
	EXPECT_TRUE(lines.empty()) << "the call gave up a promise the kill's retirement releases";
	EXPECT_EQ(fake::pool[kOffset / 4], kValue - 1);
	EXPECT_FALSE(fake::call.pending);
	makerRetires();
	EXPECT_EQ(reader.fence, reader.submitted);
	EXPECT_EQ(shown(), kPrevious);
	EXPECT_TRUE(lines.empty());
}

// The maker's channel is killed with the promised job on it (its fence passed by the kill, the word not released): every
// reader, the compositor's included, is let go by that kill's retirement, not by the call due meanwhile.
TEST_P(GiveUp, APromiseWhoseJobsChannelWasKilledIsReleasedByItsRetirementNotTheCall)
{
	readerSubmits();
	makerKeeps(0);
	maker.fence = maker.submitted;
	maker.error = 1;
	until(fake::now + 5000 * kMs);
	EXPECT_TRUE(lines.empty()) << "the call gave up a promise the kill's retirement releases";
	EXPECT_TRUE(fake::abandoned.empty());
	EXPECT_EQ(fake::pool[kOffset / 4], kValue - 1);
	EXPECT_NE(reader.fence, reader.submitted);
	EXPECT_FALSE(fake::call.pending) << "nothing left for the call";
	makerRetires();
	EXPECT_EQ(reader.fence, reader.submitted) << "the reader's acquire passed at the release";
	EXPECT_EQ(shown(), kPrevious);
	EXPECT_TRUE(lines.empty());
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

// The holder stopped before it armed anything: the waiter is let go at the default bound after it parked, the direct
// present told it was given up; the maker's withdraw afterwards tells nothing more
TEST_P(Leaves, AStoppedHolderBeforeItsArmIsGivenUpAtTheDefaultBound)
{
	ASSERT_EQ(park(), 1);
	until(fake::now + 4999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(fake::now + 1 * kMs);
	ASSERT_EQ(lines.size(), 1u)
		<< "a waiter on a promise nobody armed still waits for its holder";
	const std::string expect = "tegra-gm20b: maker-app's writer promise at fence offset " +
				   std::to_string(kOffset) +
				   " (value 9) was not kept: " + waiter().waiter +
				   " waited 5000 ms for it in " + waiter().who;
	EXPECT_EQ(lines[0].substr(0, expect.size()), expect);
	EXPECT_EQ(status(), -5);
	EXPECT_EQ(shown(), kPrevious);
	EXPECT_EQ(fake::directGivenUp, unsigned(waiter().display));
	leave();
	makerRetires();
	until(fake::now + 10000 * kMs);
	EXPECT_EQ(lines.size(), 1u);
}

// Its maker withdraws before the bound: the waiter hears it there (the direct present through its release, a CPU
// waiter through the abandon's wake), and nothing is given up
TEST_P(Leaves, AWithdrawBeforeTheDefaultBoundReleasesTheWaiter)
{
	ASSERT_EQ(park(), 1);
	until(fake::now + 4999 * kMs);
	EXPECT_TRUE(lines.empty());
	makerRetires();
	EXPECT_EQ(presentReleased, 1u);
	EXPECT_EQ(fake::abandoned.size(), 1u);
	EXPECT_EQ(status(), -5);
	EXPECT_EQ(shown(), kPrevious);
	leave();
	until(fake::now + 10000 * kMs);
	EXPECT_TRUE(lines.empty());
}

// The holder stopped after arming its bound: the waiter is let go at it, the direct present told it was given up
TEST_P(Leaves, AStoppedHoldersPromiseIsGivenUpAtItsBound)
{
	ASSERT_EQ(park(), 1);
	holderArms(5000);
	until(fake::now + 4999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(fake::now + 1 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	const std::string expect = "tegra-gm20b: maker-app's writer promise at fence offset " +
				   std::to_string(kOffset) +
				   " (value 9) was not kept: " + waiter().waiter +
				   " waited 5000 ms for it in " + waiter().who;
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
	EXPECT_EQ(fake::call.armed, 2u);
	EXPECT_EQ(fake::call.cancelled, 1u);
	EXPECT_EQ(fake::call.ran, 0u);
	EXPECT_TRUE(lines.empty());
}

TEST_P(Leaves, TheLastWaiterLeavingCancelsTheCall)
{
	ASSERT_EQ(park(), 1);
	ASSERT_EQ(park(), 1);
	keeps();
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
	holderArms(1000);

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
	holderArms(1000);
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
