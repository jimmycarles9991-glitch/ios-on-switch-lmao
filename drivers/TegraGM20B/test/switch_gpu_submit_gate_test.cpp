// SPDX-License-Identifier: MIT
#include <atomic>
#include "bazel/testing/require.h"
#include <gtest/gtest.h>
#include <chrono>
#include <future>
#include <thread>
#include <vector>
#include "drivers/TegraGM20B/SwitchGpuSubmitGate.h"
using namespace std::chrono_literals;

TEST(SubmitGate, MutationDrainsExistingSubmissionsBeforeNewEntries)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	IOLockLock(&lock);
	ASSERT_TRUE(gate.enter(&lock));
	ASSERT_TRUE(gate.enter(&lock));
	std::promise<void> asleep;
	bool notified = false;
	lock.sleeping = [&] {
		if (!notified) {
			notified = true;
			asleep.set_value();
		}
	};
	IOLockUnlock(&lock);
	std::atomic<int> order{0};
	auto mutation = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		ASSERT_TRUE(gate.mutate(&lock));
		ASSERT_TRUE(order.fetch_add(1) == 0);
		IOLockUnlock(&lock);
	});
	ASSERT_TRUE(asleep.get_future().wait_for(2s) == std::future_status::ready);
	auto next = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		ASSERT_TRUE(gate.enter(&lock));
		ASSERT_TRUE(order.fetch_add(1) == 1);
		gate.leave(&lock);
		IOLockUnlock(&lock);
	});
	IOLockLock(&lock);
	gate.leave(&lock);
	ASSERT_TRUE(order.load() == 0);
	gate.leave(&lock);
	IOLockUnlock(&lock);
	mutation.get();
	next.get();
}

TEST(SubmitGate, CloseDrainsAndRefusesLaterWork)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	IOLockLock(&lock);
	ASSERT_TRUE(gate.enter(&lock));
	IOLockUnlock(&lock);
	std::promise<void> asleep;
	bool notified = false;
	lock.sleeping = [&] {
		if (!notified) {
			notified = true;
			asleep.set_value();
		}
	};
	auto closing = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		gate.close(&lock);
		IOLockUnlock(&lock);
	});
	ASSERT_TRUE(asleep.get_future().wait_for(2s) == std::future_status::ready);
	IOLockLock(&lock);
	ASSERT_TRUE(gate.closed());
	ASSERT_TRUE(!gate.enter(&lock));
	ASSERT_TRUE(!gate.mutate(&lock));
	gate.leave(&lock);
	IOLockUnlock(&lock);
	closing.get();
}

TEST(SubmitGate, ConcurrentChannelsCannotOverlapMutations)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	std::atomic<int> inside{0}, violations{0};
	std::vector<std::thread> threads;
	for (int j = 0; j < 4; j++)
		threads.emplace_back([&] {
			for (int i = 0; i < 1000; i++) {
				IOLockLock(&lock);
				ASSERT_TRUE(gate.enter(&lock));
				IOLockUnlock(&lock);
				inside++;
				std::this_thread::yield();
				inside--;
				IOLockLock(&lock);
				gate.leave(&lock);
				IOLockUnlock(&lock);
			}
		});
	for (int j = 0; j < 2; j++)
		threads.emplace_back([&] {
			for (int i = 0; i < 200; i++) {
				IOLockLock(&lock);
				ASSERT_TRUE(gate.mutate(&lock));
				if (inside)
					violations++;
				std::this_thread::yield();
				if (inside)
					violations++;
				IOLockUnlock(&lock);
			}
		});
	for (auto &thread : threads)
		thread.join();
	ASSERT_TRUE(violations == 0);
}

TEST(SubmitGate, MutationReservationSurvivesDroppingConnectionLock)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	IOLockLock(&lock);
	ASSERT_TRUE(gate.beginMutation(&lock));
	IOLockUnlock(&lock);
	std::atomic<unsigned> admitted{0};
	auto submit = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		REQUIRE(gate.enter(&lock));
		++admitted;
		gate.leave(&lock);
		IOLockUnlock(&lock);
	});
	auto mutation = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		REQUIRE(gate.beginMutation(&lock));
		++admitted;
		gate.endMutation(&lock);
		IOLockUnlock(&lock);
	});
	EXPECT_EQ(submit.wait_for(10ms), std::future_status::timeout);
	EXPECT_EQ(mutation.wait_for(10ms), std::future_status::timeout);
	EXPECT_EQ(admitted.load(), 0u);
	IOLockLock(&lock);
	gate.endMutation(&lock);
	IOLockUnlock(&lock);
	submit.get();
	mutation.get();
	EXPECT_EQ(admitted.load(), 2u);
}

TEST(SubmitGate, CloseWaitsForAnUnlockedMutationToFinish)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	IOLockLock(&lock);
	ASSERT_TRUE(gate.beginMutation(&lock));
	IOLockUnlock(&lock);
	auto closing = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		gate.close(&lock);
		IOLockUnlock(&lock);
	});
	EXPECT_EQ(closing.wait_for(10ms), std::future_status::timeout);
	IOLockLock(&lock);
	gate.endMutation(&lock);
	IOLockUnlock(&lock);
	EXPECT_EQ(closing.wait_for(2s), std::future_status::ready);
	closing.get();
}

// SubmitJob's entry takes no lock: a mutation still waits for every submission inside, and wakes when the last leaves.
TEST(SubmitGate, AMutationWaitsForLockFreeSubmissionsAndIsWokenByTheLast)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	ASSERT_TRUE(gate.enterFast());
	ASSERT_TRUE(gate.enterFast());
	std::promise<void> asleep;
	bool notified = false;
	lock.sleeping = [&] {
		if (!notified) {
			notified = true;
			asleep.set_value();
		}
	};
	auto mutation = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		const bool began = gate.beginMutation(&lock);
		IOLockUnlock(&lock);
		return began;
	});
	ASSERT_EQ(asleep.get_future().wait_for(2s), std::future_status::ready);
	EXPECT_FALSE(gate.enterFast());
	gate.leaveFast(&lock);
	EXPECT_EQ(mutation.wait_for(20ms), std::future_status::timeout);
	gate.leaveFast(&lock);
	ASSERT_EQ(mutation.wait_for(2s), std::future_status::ready);
	ASSERT_TRUE(mutation.get());
	auto slow = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		const bool entered = gate.enterSlow(&lock);
		IOLockUnlock(&lock);
		return entered;
	});
	EXPECT_EQ(slow.wait_for(20ms), std::future_status::timeout);
	IOLockLock(&lock);
	gate.endMutation(&lock);
	IOLockUnlock(&lock);
	ASSERT_TRUE(slow.get());
	EXPECT_TRUE(gate.enterFast());
	gate.leaveFast(&lock);
	gate.leaveFast(&lock);
}

// The four submitters and two mutators below hung under load (T-v3g's laptop gates): a mutation that counted a refused
// enterFast went to sleep, the refusal undid its count without a wake, and every submitter then slept in enterSlow
// behind that mutation. A refused submission's enterSlow wakes the mutation; the mutation looks again.
TEST(SubmitGate, ARefusedFastEntryWakesTheMutationItHeldAsleep)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	ASSERT_TRUE(gate.enterFast());
	std::promise<void> asleep;
	int sleeps = 0;
	lock.sleeping = [&] {
		if (++sleeps == 1)
			asleep.set_value();
	};
	auto mutation = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		EXPECT_TRUE(gate.beginMutation(&lock));
		gate.endMutation(&lock);
		IOLockUnlock(&lock);
	});
	ASSERT_EQ(asleep.get_future().wait_for(2s), std::future_status::ready);
	EXPECT_FALSE(gate.enterFast());
	auto refused = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		EXPECT_TRUE(gate.enterSlow(&lock));
		IOLockUnlock(&lock);
		gate.leaveFast(&lock);
	});
	for (int i = 0; i < 200; i++) {
		IOLockLock(&lock);
		const int seen = sleeps;
		IOLockUnlock(&lock);
		if (seen >= 3)
			break;
		std::this_thread::sleep_for(5ms);
	}
	IOLockLock(&lock);
	EXPECT_GE(sleeps, 3)
		<< "the mutation looked again (and the refused submitter slept behind it)";
	IOLockUnlock(&lock);
	gate.leaveFast(&lock);
	ASSERT_EQ(mutation.wait_for(2s), std::future_status::ready);
	ASSERT_EQ(refused.wait_for(2s), std::future_status::ready);
}

TEST(SubmitGate, LockFreeSubmissionsNeverOverlapAMutation)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	std::atomic<int> inside{0}, violations{0};
	std::vector<std::thread> threads;
	for (int j = 0; j < 4; j++)
		threads.emplace_back([&] {
			for (int i = 0; i < 2000; i++) {
				if (!gate.enterFast()) {
					IOLockLock(&lock);
					const bool entered = gate.enterSlow(&lock);
					IOLockUnlock(&lock);
					ASSERT_TRUE(entered);
				}
				inside++;
				std::this_thread::yield();
				inside--;
				gate.leaveFast(&lock);
			}
		});
	for (int j = 0; j < 2; j++)
		threads.emplace_back([&] {
			for (int i = 0; i < 200; i++) {
				IOLockLock(&lock);
				ASSERT_TRUE(gate.beginMutation(&lock));
				IOLockUnlock(&lock);
				if (inside)
					violations++;
				std::this_thread::yield();
				if (inside)
					violations++;
				IOLockLock(&lock);
				gate.endMutation(&lock);
				IOLockUnlock(&lock);
			}
		});
	for (auto &thread : threads)
		thread.join();
	EXPECT_EQ(violations.load(), 0);
}

TEST(SubmitGate, CloseDrainsLockFreeSubmissions)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	ASSERT_TRUE(gate.enterFast());
	auto closing = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		gate.close(&lock);
		IOLockUnlock(&lock);
	});
	EXPECT_EQ(closing.wait_for(20ms), std::future_status::timeout);
	gate.leaveFast(&lock);
	ASSERT_EQ(closing.wait_for(2s), std::future_status::ready);
	EXPECT_FALSE(gate.enterFast());
	IOLockLock(&lock);
	EXPECT_FALSE(gate.enterSlow(&lock));
	IOLockUnlock(&lock);
}
