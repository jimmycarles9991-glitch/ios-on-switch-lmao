// SwitchGpuInheritorSleep.h as the kext builds it: a sleep with an owner pushes that thread through the event's
// turnstile, a sleep with none is a plain sleep on the event (stackshot asserts a turnstile sleep's inheritor is a
// thread), and every wake reaches both queues, so neither kind of sleeper is missed.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "drivers/TegraGM20B/SwitchGpuInheritorSleep.h"

namespace
{
struct Call {
	std::string what;
	void *event;
	thread_t inheritor;

	bool operator==(const Call &) const = default;
};

std::vector<Call> gCalls;
IOLock *const kLock = reinterpret_cast<IOLock *>(0x1000);
thread *const kOwner = reinterpret_cast<thread_t>(0x2000);
int gEvent;
}  // namespace

extern "C" {
lck_mtx_t *IOLockGetMachLock(IOLock *lock)
{
	return reinterpret_cast<lck_mtx_t *>(lock);
}

wait_result_t lck_mtx_sleep_with_inheritor(lck_mtx_t *lock, lck_sleep_action_t, event_t event,
					   thread_t inheritor, wait_interrupt_t, uint64_t)
{
	EXPECT_EQ(lock, reinterpret_cast<lck_mtx_t *>(kLock));
	gCalls.push_back({"turnstile sleep", event, inheritor});
	return THREAD_AWAKENED;
}

kern_return_t wakeup_all_with_inheritor(event_t event, wait_result_t)
{
	gCalls.push_back({"turnstile wake", event, nullptr});
	return 0;
}

int IOLockSleep(IOLock *lock, void *event, UInt32)
{
	EXPECT_EQ(lock, kLock);
	gCalls.push_back({"sleep", event, nullptr});
	return THREAD_AWAKENED;
}

void IOLockWakeup(IOLock *lock, void *event, bool oneThread)
{
	EXPECT_EQ(lock, kLock);
	EXPECT_FALSE(oneThread);
	gCalls.push_back({"wake", event, nullptr});
}

thread_t current_thread(void)
{
	return kOwner;
}
}

TEST(GM20BInheritorSleep, AnOwnerIsPushedThroughTheTurnstile)
{
	gCalls.clear();
	switch_gpu_sleep_inheritor(kLock, &gEvent, kOwner);
	EXPECT_EQ(gCalls, (std::vector<Call>{{"turnstile sleep", &gEvent, kOwner}}));
}

TEST(GM20BInheritorSleep, NoOwnerSleepsOnTheEventAlone)
{
	gCalls.clear();
	switch_gpu_sleep_inheritor(kLock, &gEvent, nullptr);
	EXPECT_EQ(gCalls, (std::vector<Call>{{"sleep", &gEvent, nullptr}}));
}

TEST(GM20BInheritorSleep, AWakeReachesBothKindsOfSleeper)
{
	gCalls.clear();
	switch_gpu_wakeup_inheritor(kLock, &gEvent);
	EXPECT_EQ(gCalls, (std::vector<Call>{{"turnstile wake", &gEvent, nullptr},
					     {"wake", &gEvent, nullptr}}));
}
