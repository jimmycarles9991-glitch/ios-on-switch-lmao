// SwitchGpuSubmitGate.h as the kext builds it, over a lock that really sleeps: a submission that went to sleep while a
// mutation was only queued (no owner yet, so a plain sleep) sleeps again pushing the mutator once it runs (review KR3:
// beginMutation woke no one, so the submission pushed nobody for the whole mutation).
#include <gtest/gtest.h>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <vector>

#include "drivers/TegraGM20B/SwitchGpuSubmitGate.h"

struct IOLock {
	std::mutex mutex;
	std::condition_variable woken;
};

namespace
{
using namespace std::chrono_literals;

struct Sleep {
	thread_t sleeper;
	thread_t pushed;  // nullptr: a plain sleep
};

std::mutex gRecord;
std::condition_variable gRecorded;
std::vector<Sleep> gSleeps;
std::atomic<unsigned> gWakes{0};
thread_local char tThread;

void record(thread_t pushed)
{
	{
		std::lock_guard<std::mutex> held(gRecord);
		gSleeps.push_back({current_thread(), pushed});
	}
	gRecorded.notify_all();
}

void sleepOn(IOLock *lock)
{
	std::unique_lock<std::mutex> held(lock->mutex, std::adopt_lock);
	lock->woken.wait(held);
	held.release();
}

bool waitFor(const std::function<bool(const std::vector<Sleep> &)> &seen)
{
	std::unique_lock<std::mutex> held(gRecord);
	return gRecorded.wait_for(held, 2s, [&] {
		return seen(gSleeps);
	});
}
}  // namespace

extern "C" {
void IOLockLock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.lock();
}

void IOLockUnlock(IOLock *lock) __attribute__((no_thread_safety_analysis))
{
	lock->mutex.unlock();
}

lck_mtx_t *IOLockGetMachLock(IOLock *lock)
{
	return reinterpret_cast<lck_mtx_t *>(lock);
}

int IOLockSleep(IOLock *lock, void *, UInt32)
{
	record(nullptr);
	sleepOn(lock);
	return THREAD_AWAKENED;
}

void IOLockWakeup(IOLock *lock, void *, bool)
{
	++gWakes;
	lock->woken.notify_all();
}

wait_result_t lck_mtx_sleep_with_inheritor(lck_mtx_t *lock, lck_sleep_action_t, event_t,
					   thread_t inheritor, wait_interrupt_t, uint64_t)
{
	record(inheritor);
	sleepOn(reinterpret_cast<IOLock *>(lock));
	return THREAD_AWAKENED;
}

kern_return_t wakeup_all_with_inheritor(event_t, wait_result_t)
{
	++gWakes;
	return 0;
}

thread_t current_thread(void)
{
	return reinterpret_cast<thread_t>(&tThread);
}
}

TEST(SubmitGatePush, ASubmissionQueuedBehindAMutationPushesItOnceItRuns)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	ASSERT_TRUE(gate.enterFast());

	std::promise<thread_t> mutatorThread;
	std::promise<void> end;
	auto mutation = std::async(std::launch::async, [&] {
		mutatorThread.set_value(current_thread());
		IOLockLock(&lock);
		const bool began = gate.beginMutation(&lock);
		IOLockUnlock(&lock);
		end.get_future().wait();
		IOLockLock(&lock);
		if (began)
			gate.endMutation(&lock);
		IOLockUnlock(&lock);
		return began;
	});
	const thread_t mutator = mutatorThread.get_future().get();
	ASSERT_TRUE(waitFor([&](const std::vector<Sleep> &s) {
		return !s.empty() && s.back().sleeper == mutator;
	})) << "the mutation waits for the submission in flight";

	std::promise<thread_t> submitterThread;
	auto submit = std::async(std::launch::async, [&] {
		submitterThread.set_value(current_thread());
		if (gate.enterFast())
			return false;
		IOLockLock(&lock);
		const bool entered = gate.enterSlow(&lock);
		IOLockUnlock(&lock);
		if (entered)
			gate.leaveFast(&lock);
		return entered;
	});
	const thread_t submitter = submitterThread.get_future().get();
	ASSERT_TRUE(waitFor([&](const std::vector<Sleep> &s) {
		for (const Sleep &one : s)
			if (one.sleeper == submitter && !one.pushed)
				return true;
		return false;
	})) << "queued behind a mutation with no owner yet, the submission sleeps plain";

	gate.leaveFast(&lock);
	EXPECT_TRUE(waitFor([&](const std::vector<Sleep> &s) {
		for (const Sleep &one : s)
			if (one.sleeper == submitter && one.pushed == mutator)
				return true;
		return false;
	})) << "the running mutation's owner was never pushed by the submission waiting for it";

	end.set_value();
	EXPECT_TRUE(mutation.get());
	EXPECT_TRUE(submit.get());
}

// Review of 5f634f298 (blocking): releaseLeft slept on the gate's event outside its sleeper count, so a mutation that ended
// while only it slept woke no one and a client's close hung. A wait for a mutation's end is counted: endMutation wakes it,
// and the wait pushes the mutator meanwhile.
TEST(SubmitGatePush, AWaitForAMutationsEndIsWokenByIt)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	std::promise<thread_t> mutatorThread;
	std::promise<void> end;
	auto mutation = std::async(std::launch::async, [&] {
		IOLockLock(&lock);
		const bool began = gate.beginMutation(&lock);
		IOLockUnlock(&lock);
		mutatorThread.set_value(current_thread());
		end.get_future().wait();
		IOLockLock(&lock);
		if (began)
			gate.endMutation(&lock);
		IOLockUnlock(&lock);
		return began;
	});
	const thread_t mutator = mutatorThread.get_future().get();
	std::promise<thread_t> waiterThread;
	auto waiter = std::async(std::launch::async, [&] {
		waiterThread.set_value(current_thread());
		IOLockLock(&lock);
		gate.waitMutationEnd(&lock);
		IOLockUnlock(&lock);
	});
	const thread_t waiting = waiterThread.get_future().get();
	ASSERT_TRUE(waitFor([&](const std::vector<Sleep> &s) {
		for (const Sleep &one : s)
			if (one.sleeper == waiting && one.pushed == mutator)
				return true;
		return false;
	})) << "the wait for the mutation's end pushes the mutator";
	end.set_value();
	EXPECT_EQ(waiter.wait_for(2s), std::future_status::ready)
		<< "the mutation's end woke no one";
	EXPECT_TRUE(mutation.get());
}

// Review nit 7: every bind cut and gated method ends a mutation; with no one asleep on the gate that is no wake call
TEST(SubmitGatePush, AnUncontendedMutationWakesNoOne)
{
	IOLock lock;
	SwitchGpuSubmitGate gate;
	const unsigned before = gWakes.load();
	IOLockLock(&lock);
	ASSERT_TRUE(gate.beginMutation(&lock));
	gate.endMutation(&lock);
	IOLockUnlock(&lock);
	EXPECT_EQ(gWakes.load(), before);
}
