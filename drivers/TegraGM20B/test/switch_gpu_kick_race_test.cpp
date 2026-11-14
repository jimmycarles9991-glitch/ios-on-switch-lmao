// SPDX-License-Identifier: MIT
// A job's records go up at exec's point of no return, before its kick (switch_gpu_chan_exec_events' `owned`), so what
// meets its backings while it kicks goes on: the interrupt pass releasing the display's scanout lease, the display's
// next snapshot and a CPU lock trap. The same actors in a loop, under ThreadSanitizer.
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "switch_gpu_submit_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuAdmission.h"
#include "drivers/TegraGM20B/SwitchGpuScanoutSnapshot.h"

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ;
constexpr uint32_t kWrite = SWITCH_GPU_BACKING_WRITE | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION;
constexpr uint32_t kPages = 4;
constexpr uint64_t kBytes = uint64_t(kPages) << 12;

struct Channel {
	volatile uint32_t fence = 0;
	switch_gpu_timeline *timeline = nullptr;
	uint32_t next = 0;

	switch_gpu_backing_token token(uint32_t value) const
	{
		switch_gpu_backing_token made = {};
		switch_gpu_timeline_token(timeline, value, &made);
		return made;
	}

	void complete(uint32_t value)
	{
		__atomic_store_n(&fence, value, __ATOMIC_RELEASE);
	}
};

Channel compositor, app, other;
uint64_t nextPage = 0xc00000000ull;

// The device's CPU waits for a job sleep until its fence; here, until the test's GPU completes it.
void waitForJob(uint32_t queue, uint32_t value)
{
	const Channel *channel = queue == compositor.token(0).queue ? &compositor
				 : queue == app.token(0).queue      ? &app
								    : &other;
	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
	while (int32_t(__atomic_load_n(&channel->fence, __ATOMIC_ACQUIRE) - value) < 0 &&
	       std::chrono::steady_clock::now() < end)
		std::this_thread::sleep_for(std::chrono::microseconds(50));
}

void open()
{
	static bool opened;
	if (opened)
		return;
	writerWait = waitForJob;
	ASSERT_EQ(switch_gpu_timeline_init(), 0);
	compositor.timeline = switch_gpu_timeline_create(1, &compositor.fence);
	app.timeline = switch_gpu_timeline_create(2, &app.fence);
	other.timeline = switch_gpu_timeline_create(3, &other.fence);
	opened = true;
}

switch_gpu_backing *surface()
{
	uint64_t pages[kPages];
	for (uint32_t i = 0; i < kPages; ++i)
		pages[i] = (nextPage += 4096);
	nextPage += 0x100000;
	return switch_gpu_backing_register(pages, kPages, 0, nullptr, nullptr);
}

void noDone(void *, int)
{
}

enum class Order {
	PublishThenKick,
	KickThenPublish
};

// The compositor's job, writing `target` and reading `texture`, as SubmitJob runs it: `kick` where exec kicks.
void composite(switch_gpu_backing *target, switch_gpu_backing *texture, Order order,
	       const std::function<void()> &kick)
{
	switch_gpu_backing_use uses[2] = {{target, kWrite, 0}, {texture, kRead, 0}};
	const uint32_t frame = ++compositor.next;
	const auto completion = compositor.token(frame);
	switch_gpu_backing_transaction transaction = {};
	ASSERT_EQ(switch_gpu_backing_begin_held(uses, 2, nullptr, 0, nullptr, 0, &completion,
						&transaction),
		  0);
	switch_gpu_backing_token dependencies[8] = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, dependencies, 8, &needed), 0);
	if (order == Order::PublishThenKick)
		switch_gpu_backing_commit(&transaction);
	kick();
	if (order == Order::KickThenPublish)
		switch_gpu_backing_commit(&transaction);
	compositor.complete(frame);
}

// Whether the interrupt pass, the display's next swap and a CPU lock trap, each on its own thread as on the device, all
// finish while the compositor's job on their backings kicks (the kick gives up waiting for them after `bound`).
bool metDuringKick(Order order, std::chrono::milliseconds bound)
{
	auto *target = surface(), *texture = surface();
	SwitchGpuScanoutSnapshot shown = {}, next = {};
	shown.done = next.done = noDone;
	EXPECT_EQ(shown.begin(nullptr, 0), 0);
	EXPECT_EQ(shown.add(target), 0);
	std::mutex lock;
	std::condition_variable changed;
	int finished = 0;
	bool met = false;
	std::vector<std::thread> meeting;
	const auto meet = [&](std::function<void()> work) {
		meeting.emplace_back([&, work] {
			work();
			std::lock_guard<std::mutex> hold(lock);
			++finished;
			changed.notify_all();
		});
	};
	std::thread submitter([&] {
		composite(target, texture, order, [&] {
			meet([&] {
				SwitchGpuScanoutSnapshot::complete(&shown, 0);
			});
			meet([&] {
				EXPECT_EQ(next.begin(nullptr, 0), 0);
				EXPECT_EQ(next.add(target), 0);
			});
			meet([&] {
				EXPECT_EQ(switch_gpu_backing_cpu_acquire(texture, 0, kBytes, kRead),
					  0);
			});
			std::unique_lock<std::mutex> hold(lock);
			met = changed.wait_for(hold, bound, [&] {
				return finished == 3;
			});
		});
	});
	submitter.join();
	for (auto &thread : meeting)
		thread.join();
	next.cancel();
	EXPECT_EQ(switch_gpu_backing_cpu_release(texture, 0, kBytes, kRead), 0);
	switch_gpu_backing_release(target);
	switch_gpu_backing_release(texture);
	return met;
}
}  // namespace

TEST(KickRace, ThePassTheDisplayAndACpuLockGoOnWhileAJobOnTheirBackingsKicks)
{
	open();
	EXPECT_TRUE(metDuringKick(Order::PublishThenKick, std::chrono::seconds(2)));
}

// The same meeting against a job that kicks before its records go up, holding its backings' locks: none of them gets
// through until the kick is over, which is what the test above would see of a kick under the locks.
TEST(KickRace, AKickUnderItsBackingsLocksHoldsThemAllBack)
{
	open();
	EXPECT_FALSE(metDuringKick(Order::KickThenPublish, std::chrono::milliseconds(300)));
}

TEST(KickRace, JobsKickWhileThePassTheDisplayAndCpuLocksMeetTheirBackings)
{
	constexpr uint32_t kFrames = 300;
	open();
	auto *target = surface(), *texture = surface();
	std::atomic<bool> done{false};
	std::thread compositing([&] {
		hostCpu() = 0;
		for (uint32_t frame = 0; frame < kFrames; ++frame)
			composite(target, texture, Order::PublishThenKick, [] {
				std::this_thread::yield();
			});
		done = true;
	});
	std::thread drawing([&] {
		hostCpu() = 1;
		for (uint32_t value = ++app.next; !done; value = ++app.next) {
			switch_gpu_backing_use use = {texture, kWrite, 0};
			const auto drawn = app.token(value);
			switch_gpu_backing_transaction transaction = {};
			ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &drawn, &transaction), 0);
			switch_gpu_backing_commit(&transaction);
			app.complete(value);
		}
	});
	// the display hands each swap's snapshot to the interrupt pass, which releases its leases at the frame's end
	std::mutex lock;
	std::condition_variable changed;
	SwitchGpuScanoutSnapshot *shown = nullptr;
	bool closed = false;
	std::thread display([&] {
		hostCpu() = 2;
		SwitchGpuScanoutSnapshot snapshot = {};
		snapshot.done = noDone;
		while (!done) {
			EXPECT_EQ(snapshot.begin(nullptr, 0), 0);
			EXPECT_EQ(snapshot.add(target), 0);
			std::unique_lock<std::mutex> hold(lock);
			shown = &snapshot;
			changed.notify_all();
			changed.wait(hold, [&] {
				return !shown;
			});
		}
		std::lock_guard<std::mutex> hold(lock);
		closed = true;
		changed.notify_all();
	});
	std::thread pass([&] {
		hostCpu() = 3;
		for (;;) {
			std::unique_lock<std::mutex> hold(lock);
			changed.wait(hold, [&] {
				return shown || closed;
			});
			if (!shown)
				return;
			auto *ended = shown;
			hold.unlock();
			SwitchGpuScanoutSnapshot::complete(ended, 0);
			hold.lock();
			shown = nullptr;
			changed.notify_all();
		}
	});
	std::thread locking([&] {
		hostCpu() = 4;
		while (!done) {
			for (auto *backing : {target, texture})
				if (!switch_gpu_backing_cpu_acquire(backing, 0, kBytes, kRead))
					EXPECT_EQ(switch_gpu_backing_cpu_release(backing, 0, kBytes,
										 kRead),
						  0);
		}
	});
	compositing.join();
	drawing.join();
	display.join();
	pass.join();
	locking.join();
	switch_gpu_backing_release(target);
	switch_gpu_backing_release(texture);
	for (int i = 0; i < 4; ++i)
		switch_gpu_backing_retire();
}

// Every other party beside the compositor's ordered jobs: a second channel's ordered job reading the texture, an app's
// claimed job writing it, an admission group over both backings, the registry finding the texture again and retiring,
// CPU locks of either kind, and the display with its pass.
TEST(KickRace, EveryPartyMeetsJobsPublishingInTheOrder)
{
	constexpr uint32_t kFrames = 200;
	open();
	auto *target = surface(), *texture = surface();
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(texture, &info);
	const uint64_t page = info.pages[0];
	std::atomic<bool> done{false};
	std::vector<std::thread> parties;
	parties.emplace_back([&] {
		for (uint32_t frame = 0; frame < kFrames; ++frame)
			composite(target, texture, Order::PublishThenKick, [] {});
		done = true;
	});
	parties.emplace_back([&] {
		while (!done) {
			switch_gpu_backing_use use = {texture, kRead, 0};
			const uint32_t value = ++other.next;
			const auto reading = other.token(value);
			switch_gpu_backing_transaction transaction = {};
			ASSERT_EQ(switch_gpu_backing_begin_held(&use, 1, nullptr, 0, nullptr, 0,
								&reading, &transaction),
				  0);
			switch_gpu_backing_commit(&transaction);
			other.complete(value);
		}
	});
	parties.emplace_back([&] {
		while (!done) {
			switch_gpu_backing_use use = {texture, kWrite, 0};
			const uint32_t value = ++app.next;
			const auto drawn = app.token(value);
			switch_gpu_backing_transaction transaction = {};
			ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &drawn, &transaction), 0);
			switch_gpu_backing_commit(&transaction);
			app.complete(value);
		}
	});
	parties.emplace_back([&] {
		while (!done) {
			SwitchGpuAdmission admission;
			const switch_gpu_backing_use uses[2] = {{texture, kRead, 0},
								{target, kRead, 0}};
			if (!admission.begin(uses, 2, nullptr, 0, nullptr, 0))
				admission.end();
		}
	});
	parties.emplace_back([&] {
		while (!done) {
			if (auto *found = switch_gpu_backing_find_pa(page))
				switch_gpu_backing_release(found);
			switch_gpu_backing_retire();
		}
	});
	parties.emplace_back([&] {
		for (uint32_t lock = 0; !done; ++lock) {
			const uint32_t access = (lock & 1) ? kRead : kWrite;
			if (!switch_gpu_backing_cpu_acquire(texture, 0, kBytes, access))
				EXPECT_EQ(
					switch_gpu_backing_cpu_release(texture, 0, kBytes, access),
					0);
		}
	});
	parties.emplace_back([&] {
		SwitchGpuScanoutSnapshot shown = {};
		shown.done = noDone;
		while (!done) {
			EXPECT_EQ(shown.begin(nullptr, 0), 0);
			EXPECT_EQ(shown.add(target), 0);
			SwitchGpuScanoutSnapshot::complete(&shown, 0);
		}
	});
	for (auto &party : parties)
		party.join();
	switch_gpu_backing_release(target);
	switch_gpu_backing_release(texture);
	for (int i = 0; i < 4; ++i)
		switch_gpu_backing_retire();
}
