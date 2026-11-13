// SPDX-License-Identifier: MIT
// The give-up across the GPU's power-down (system sleep, rail-gating: tgm_recover_suspend) and its power-up (tgm_wake,
// tgm_rail_up_locked). The call stays armed and does memory work only while the GPU is down: a reader parked across
// the power-down is still let go at its second, no register is read (the stubs fail any read while down), and at the
// power-up every promise still waited on gets its whole second again. Suspend and resume each wait out a call in flight.
#include <atomic>
#include <chrono>
#include <thread>
#include "switch_gpu_promise_giveup_fixture.h"

namespace
{
using namespace promise_giveup;

class Power : public Fixture
{
protected:
	void powerDown()
	{
		tgm_promise_suspend();
		fake::poweredDown = true;
	}

	void powerUp()
	{
		fake::poweredDown = false;
		tgm_promise_resume();
	}

	int cpuPark(uint32_t offset, const char *waiter, const char *who)
	{
		fake::pool[offset / 4] = kValue - 1;
		tgm_fence_pages.made[0][offset / 4 - TGM_FENCE_PROMISE_WORD] = kValue;
		return tgm_promise_park(offset, kValue, waiter, who, nullptr, 0);
	}
};

// A CPU lock parked on a promise nobody keeps; the display goes off and the GPU rail-gates. Nothing brings the rail up,
// and the call still lets the lock go at its second.
TEST_F(Power, ACpuReaderParkedAcrossARailGateIsLetGoAtItsSecond)
{
	ASSERT_EQ(cpuPark(kOffset, "SpringBoard", "a CPU lock"), 1);
	const uint64_t parked = fake::now;
	until(parked + 200 * kMs);
	powerDown();
	until(parked + 999 * kMs);
	EXPECT_TRUE(lines.empty());
	until(parked + 1000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_NE(lines[0].find("SpringBoard waited 1000 ms for it in a CPU lock"),
		  std::string::npos)
		<< lines[0];
	EXPECT_EQ(fake::pool[kOffset / 4], kValue);
	EXPECT_EQ(status(), -5);
	tgm_promise_leave(kOffset, kValue, 0);
	powerUp();
	until(parked + 10000 * kMs);
	EXPECT_EQ(lines.size(), 1u);
}

// A GPU reader parked on a promise nobody keeps when the GPU goes down: given up at its second with no ACQUIRE probe,
// which reads the reader's PBDMA.
TEST_F(Power, AGpuReaderParkedAcrossAPowerDownIsGivenUpUnprobed)
{
	readerSubmits();
	powerDown();
	until(fake::now + 1000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_EQ(fake::pool[kOffset / 4], kValue);
	EXPECT_EQ(probedReaders, 0u);
}

// Down, with a GPU reader still parked and the promised job queued behind another: every step that reads registers up
// (the hang rule's look at the job ahead, the ACQUIRE probe of the parked reader) is skipped, and the promise is given
// up at its second, since nothing runs before the resume.
TEST_F(Power, TheCallReadsNoRegisterWhileTheGpuIsDown)
{
	readerSubmits();
	makerKeeps(1);
	powerDown();
	until(fake::now + 1000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_EQ(fake::pool[kOffset / 4], kValue);
	EXPECT_EQ(fake::hangLooks, 0u);
	EXPECT_EQ(probedReaders, 0u);
}

// The power-down's drain killed the reader's and the maker's channels; a CPU copy still waits: memory work only, and
// the copy is let go at its second.
TEST_F(Power, AGpuReaderRecordedOnADrainedChannelReadsNoRegister)
{
	readerSubmits();
	makerKeeps(1);
	ASSERT_EQ(tgm_promise_park(kOffset, kValue, "backboardd", "a CPU copy", nullptr, 0), 1);
	reader.error = 1;
	reader.fence = reader.submitted;
	maker.error = 1;
	maker.fence = maker.submitted;
	powerDown();
	until(fake::now + 1000 * kMs);
	ASSERT_EQ(lines.size(), 1u);
	EXPECT_EQ(fake::pool[kOffset / 4], kValue);
	EXPECT_EQ(fake::hangLooks, 0u);
	EXPECT_EQ(probedReaders, 0u);
}

// Two promises parked 300 ms apart, the GPU down from 500 ms to 700 ms: both are judged a whole second from the
// power-up.
TEST_F(Power, ThePowerUpGivesEveryPromiseItsWholeSecondAgain)
{
	const uint64_t t0 = fake::now;
	ASSERT_EQ(cpuPark(kOffset, "SpringBoard", "a CPU lock"), 1);
	until(t0 + 300 * kMs);
	ASSERT_EQ(cpuPark(kOffset + 4, "backboardd", "a CPU copy"), 1);
	until(t0 + 500 * kMs);
	powerDown();
	until(t0 + 700 * kMs);
	powerUp();
	EXPECT_TRUE(fake::call.pending);
	EXPECT_EQ(fake::call.due, t0 + 1700 * kMs);
	until(t0 + 1699 * kMs);
	EXPECT_TRUE(lines.empty());
	until(t0 + 1700 * kMs);
	EXPECT_EQ(lines.size(), 2u);
}

// After the power-up the call reaches the GPU again: a promised job queued behind another is the hang rule's, looked
// at and waited for, not given up at its second.
TEST_F(Power, AfterThePowerUpAJobAheadIsTheHangRulesAgain)
{
	powerDown();
	until(fake::now + 300 * kMs);
	powerUp();
	readerSubmits();
	makerKeeps(1);
	until(fake::now + 1000 * kMs);
	EXPECT_TRUE(lines.empty());
	EXPECT_EQ(fake::hangLooks, 1u);
	EXPECT_EQ(maker.asked.back(), maker.submitted - 1)
		<< "the job ahead's interrupt is asked for";
}

// The call is reading registers (the hang rule's look at the job ahead) when the power-down comes: the suspend waits for
// it, so no register is read once the GPU goes down.
TEST_F(Power, TheSuspendWaitsOutACallInFlight)
{
	readerSubmits();
	makerKeeps(1);
	std::atomic<bool> entered{false}, inLook{false};
	fake::atRegisters = [&] {
		inLook = true;
		entered = true;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		inLook = false;
	};
	fake::now += 1000 * kMs;
	fake::call.pending = false;
	std::thread call([] {
		giveup_run(nullptr);
	});
	while (!entered)
		std::this_thread::yield();
	tgm_promise_suspend();
	const bool overlapped = inLook;
	fake::poweredDown = true;
	call.join();
	fake::atRegisters = nullptr;
	EXPECT_FALSE(overlapped) << "the suspend went ahead while the call read registers";
	EXPECT_EQ(fake::hangLooks, 1u);
	EXPECT_TRUE(lines.empty()) << "the job ahead is the hang rule's";
}

// The call gives a promise up while the GPU is down when the power-up comes: the resume waits for it, so the promise
// is given up once and nothing is armed again for it.
TEST_F(Power, TheResumeWaitsOutACallInFlight)
{
	ASSERT_EQ(cpuPark(kOffset, "SpringBoard", "a CPU lock"), 1);
	powerDown();
	std::atomic<bool> entered{false}, giving{false};
	fake::atAbandon = [&] {
		giving = true;
		entered = true;
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
		giving = false;
	};
	fake::now += 1000 * kMs;
	fake::call.pending = false;
	std::thread call([] {
		giveup_run(nullptr);
	});
	while (!entered)
		std::this_thread::yield();
	powerUp();
	const bool overlapped = giving;
	call.join();
	fake::atAbandon = nullptr;
	EXPECT_FALSE(overlapped) << "the resume went ahead while the call gave the promise up";
	EXPECT_EQ(lines.size(), 1u);
	EXPECT_FALSE(fake::call.pending) << "nothing is waited on after the give-up";
	tgm_promise_leave(kOffset, kValue, 0);
}
}  // namespace
