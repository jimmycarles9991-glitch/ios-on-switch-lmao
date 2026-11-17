// SPDX-License-Identifier: MIT
// switch_gpu_chan_exec_events against the real backing layer and nvif channel.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include <IOKit/IOLib.h>
#include "drivers/TegraGM20B/TegraGM20BABI.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/SwitchGpuWordWait.h"
#include "drivers/TegraGM20B/test/exec/switch_gpu_exec_backing.h"
#include "drivers/TegraGM20B/test/exec/switch_gpu_exec_fixture.h"

namespace
{
// gpu_exec.c returns the shim's Linux errno values.
constexpr int kAgain = -11;
constexpr int kNoDevice = -19;
constexpr int kTimedOut = -110;
constexpr uint32_t kJob = SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT;
constexpr uint64_t kConsumer = 0x102;
const tgm_push kPush = {0x40000000, 16, 0, 0, 0, 0};

struct Kick {
	bool doorbell, main;
	uint64_t addr;
	uint32_t bytes;
	bool recorded, backing_free, live_free, ordered;
	uint32_t gp_put;
};

TEST(ExecEmpty, FirstWordWaitNeedsNoRegisteredBacking)
{
	uint32_t doorbells = 0, seqno = 0;
	const switch_exec_hooks hooks = {&doorbells, nullptr,
					 [](void *context, switch_exec_chan *, uint32_t) {
						 ++*static_cast<uint32_t *>(context);
						 EXPECT_TRUE(switch_exec_backing::order_held());
						 EXPECT_EQ(ioLocksHeld().size(), 1u);
						 EXPECT_FALSE(switch_exec_mutex_held());
					 },
					 nullptr};
	switch_exec_reset(&hooks);
	auto *chan = switch_exec_chan_new(0x100, 0);
	const switch_gpu_word_wait word = {0x100, 1, 0x101};
	const uint64_t own = ioLockTakes().load(), live = switch_exec_mutex_takes();
	ASSERT_EQ(switch_gpu_chan_exec_events(switch_exec_chan_tgm(chan), nullptr, 0, nullptr, 0,
					      nullptr, 0, &word, 1, &seqno, nullptr,
					      kJob | SWITCH_GPU_SUBMIT_FORCE_FENCE, nullptr, 0,
					      nullptr),
		  0);
	EXPECT_EQ(doorbells, 1u);
	EXPECT_EQ(ioLockTakes().load() - own, 1u);
	EXPECT_EQ(switch_exec_mutex_takes() - live, 0u);
	EXPECT_EQ(switch_exec_chan_word_acquires(chan, 0x100, 1), 1u);
	EXPECT_TRUE(ioLocksHeld().empty());
}

class ExecCommit : public testing::Test
{
protected:
	void SetUp() override
	{
		switch_exec_backing::register_engines();
		const switch_exec_hooks hooks = {this, on_entry, on_doorbell, on_read_get};
		switch_exec_reset(&hooks);
		consumer = switch_exec_chan_new(uint32_t(kConsumer), 2);
		switch_exec_chan_live(consumer, 1);
		written = switch_exec_backing::make();
	}

	void TearDown() override
	{
		switch_gpu_backing_abort(&transaction);
		switch_exec_backing::retire_all();
	}

	switch_exec_chan *producer(uint32_t uid, uint32_t page, switch_gpu_backing *wrote)
	{
		switch_exec_chan *chan = switch_exec_chan_new(uid, page);
		switch_exec_chan_live(chan, 1);
		switch_exec_backing::publish_write(wrote, switch_exec_backing::token(uid, 1));
		return chan;
	}

	// SubmitJob's order: the job's token, begin_held over its uses, the dependencies the transaction finds.
	int begin(std::initializer_list<switch_gpu_backing_use> list)
	{
		uses.assign(list);
		completion = switch_exec_backing::token(kConsumer, pointers().submitted + 1);
		int error =
			switch_gpu_backing_begin_held(uses.data(), uint32_t(uses.size()), nullptr,
						      0, nullptr, 0, &completion, &transaction);
		if (!error)
			error = switch_gpu_backing_dependencies(&transaction, dependencies,
								TegraGM20BMaxJobDependencies,
								&dependency_count);
		return error;
	}

	int exec(const tgm_push *pushes, uint32_t count, uint32_t visibility = kJob)
	{
		return switch_gpu_chan_exec_events(switch_exec_chan_tgm(consumer), pushes, count,
						   dependencies, dependency_count, siblings.data(),
						   uint32_t(siblings.size()), words.data(),
						   uint32_t(words.size()), &seqno, nullptr,
						   visibility, nullptr, 0, &transaction);
	}

	switch_exec_chan_pointers pointers() const
	{
		switch_exec_chan_pointers out = {};
		switch_exec_chan_read(consumer, &out);
		return out;
	}

	bool held() const
	{
		return transaction.count != 0;
	}

	void kicked(bool doorbell, bool main, uint64_t addr, uint32_t bytes)
	{
		const bool ordered = switch_exec_backing::order_held();
		Kick kick = {doorbell,
			     main,
			     addr,
			     bytes,
			     ordered && switch_exec_backing::recorded(written, completion),
			     ioLocksHeld().size() == size_t(ordered),
			     !switch_exec_mutex_held(),
			     ordered,
			     pointers().gp_put};
		kicks.push_back(kick);
		if (doorbell && before_doorbell)
			before_doorbell();
	}

	void expect_published(bool has_write = true, bool ordered = true) const
	{
		ASSERT_FALSE(kicks.empty());
		uint32_t doorbells = 0;
		for (size_t i = 0; i < kicks.size(); ++i) {
			const Kick &kick = kicks[i];
			SCOPED_TRACE(i);
			EXPECT_EQ(kick.ordered, ordered);
			EXPECT_EQ(kick.gp_put, kicks.front().gp_put);
			if (kick.doorbell || !kick.main || kick.recorded) {
				EXPECT_TRUE(!has_write || kick.recorded);
				EXPECT_TRUE(kick.backing_free);
				EXPECT_TRUE(kick.live_free);
			}
			doorbells += kick.doorbell;
		}
		EXPECT_EQ(doorbells, 1u);
		EXPECT_TRUE(kicks.back().doorbell);
		EXPECT_EQ(pointers().gp_put, pointers().gpfifo_cur);
		EXPECT_FALSE(switch_exec_backing::order_held());
	}

	bool pushed_user_entry() const
	{
		for (const Kick &kick : kicks)
			if (!kick.doorbell && !kick.main && kick.addr == kPush.va &&
			    kick.bytes == kPush.dwords * 4)
				return true;
		return false;
	}

	static void on_entry(void *context, switch_exec_chan *, int main, uint64_t addr,
			     uint32_t bytes)
	{
		static_cast<ExecCommit *>(context)->kicked(false, main, addr, bytes);
	}

	static void on_doorbell(void *context, switch_exec_chan *, uint32_t)
	{
		static_cast<ExecCommit *>(context)->kicked(true, true, 0, 0);
	}

	static void on_read_get(void *context, switch_exec_chan *chan, int gpfifo, uint32_t calls)
	{
		auto *test = static_cast<ExecCommit *>(context);
		if (test->read_get)
			test->read_get(chan, gpfifo != 0, calls);
	}

	switch_exec_chan *consumer = nullptr;
	switch_gpu_backing *written = nullptr;
	std::vector<switch_gpu_backing_use> uses;
	switch_gpu_backing_transaction transaction = {};
	switch_gpu_backing_token completion = {};
	switch_gpu_backing_token dependencies[TegraGM20BMaxJobDependencies] = {};
	uint32_t dependency_count = 0;
	uint32_t seqno = 0;
	std::vector<Kick> kicks;
	std::vector<switch_gpu_sibling_wait> siblings;
	std::vector<switch_gpu_word_wait> words;
	std::function<void(switch_exec_chan *, bool, uint32_t)> read_get;
	std::function<void()> before_doorbell;
};

bool operator==(const switch_exec_chan_pointers &a, const switch_exec_chan_pointers &b)
{
	return a.gpfifo_cur == b.gpfifo_cur && a.gp_put == b.gp_put && a.submitted == b.submitted &&
	       a.gpfifo_free == b.gpfifo_free && a.push_bgn == b.push_bgn &&
	       a.push_cur == b.push_cur && a.push_end == b.push_end && a.push_seg == b.push_seg &&
	       a.defer_kick == b.defer_kick;
}
}  // namespace

TEST_F(ExecCommit, PublishesAfterPrivateWaitsAndBeforeUserEntriesAndGpPut)
{
	switch_gpu_backing *read = switch_exec_backing::make();
	switch_exec_chan *source = producer(0x101, 1, read);
	ASSERT_EQ(switch_exec_backing::pending(written), 0u);
	ASSERT_EQ(
		begin({{read, SWITCH_GPU_BACKING_READ, 0}, {written, SWITCH_GPU_BACKING_WRITE, 0}}),
		0);
	ASSERT_EQ(dependency_count, 1u);

	ASSERT_EQ(exec(&kPush, 1), 0);

	expect_published();
	EXPECT_FALSE(kicks.front().recorded);
	EXPECT_FALSE(kicks.front().doorbell)
		<< "the acquire's entry is written before any doorbell";
	EXPECT_TRUE(pushed_user_entry());
	EXPECT_EQ(switch_exec_chan_acquires(consumer, source, 1), 1u);
	EXPECT_EQ(seqno, uint32_t(completion.value));
	EXPECT_EQ(pointers().submitted, seqno);
	EXPECT_EQ(pointers().gp_put, pointers().gpfifo_cur);
	EXPECT_FALSE(held());
	switch_exec_backing::complete(completion);
	EXPECT_EQ(switch_exec_backing::pending(written), 0u) << "the record was the job's own";
}

TEST_F(ExecCommit, AClaimedUsesLockIsReleasedBeforeUserEntries)
{
	constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;
	switch_gpu_backing *claimed = switch_exec_backing::make();
	producer(0x103, 3, claimed);
	switch_exec_backing::complete(switch_exec_backing::token(0x103, 1));
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(claimed, 0, 4096, kWrite), 0);
	std::atomic<int> locked{1};
	std::thread sleeper([&] {
		locked = switch_gpu_backing_cpu_acquire(claimed, 0, 4096, kRead);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	ASSERT_EQ(locked.load(), 1) << "the CPU lock did not wait for the CPU writer";
	ASSERT_EQ(begin({{claimed, kRead | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, 0},
			 {written, kWrite, 0}}),
		  0);

	ASSERT_EQ(exec(&kPush, 1), 0);

	expect_published();
	EXPECT_EQ(switch_gpu_backing_cpu_release(claimed, 0, 4096, kWrite), 0);
	sleeper.join();
	EXPECT_EQ(locked.load(), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(claimed, 0, 4096, kRead), 0);
}

TEST_F(ExecCommit, NoRoomPublishesNothingAndKeepsTheTransaction)
{
	switch_exec_chan_place(consumer, 0, 10, 0);
	switch_exec_chan_set_get(consumer, 0, 11);
	ASSERT_EQ(begin({{written, SWITCH_GPU_BACKING_WRITE, 0}}), 0);
	const switch_exec_chan_pointers before = pointers();

	EXPECT_EQ(exec(&kPush, 1), kAgain);

	EXPECT_TRUE(kicks.empty());
	EXPECT_TRUE(held());
	EXPECT_TRUE(pointers() == before);
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(switch_exec_backing::pending(written), 0u);
}

TEST_F(ExecCommit, FinalInvalidProducerPublishesNothing)
{
	switch_gpu_backing *read = switch_exec_backing::make();
	producer(0x101, 1, read);
	ASSERT_EQ(
		begin({{read, SWITCH_GPU_BACKING_READ, 0}, {written, SWITCH_GPU_BACKING_WRITE, 0}}),
		0);
	ASSERT_EQ(dependency_count, 1u);
	dependencies[dependency_count++] = switch_exec_backing::token(0x777, 1);
	const switch_exec_chan_pointers before = pointers();

	EXPECT_EQ(exec(&kPush, 1), kNoDevice);

	EXPECT_TRUE(kicks.empty());
	EXPECT_TRUE(held());
	EXPECT_TRUE(pointers() == before);
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(switch_exec_backing::pending(written), 0u);
}

// chan_room finds room; nvif's own wait wraps the push buffer (moving bgn/cur/end), then finds the GPFIFO full and
// times out. GET moving back between the two stands in for the two disagreeing.
TEST_F(ExecCommit, GpfifoWaitFailureRestoresTheRingAndPublishesNothing)
{
	const uint32_t near_end = pointers().push_max - 8;
	switch_exec_chan_place(consumer, near_end, 10, 3);
	switch_exec_chan_set_get(consumer, near_end, 10);
	switch_exec_chan_pointers waiting = {};
	read_get = [&](switch_exec_chan *chan, bool gpfifo, uint32_t calls) {
		if (chan == consumer && gpfifo && calls == 2) {
			waiting = pointers();
			switch_exec_chan_set_get(consumer, near_end, 11);
		}
	};
	ASSERT_EQ(begin({{written, SWITCH_GPU_BACKING_WRITE, 0}}), 0);
	const switch_exec_chan_pointers before = pointers();

	EXPECT_EQ(exec(&kPush, 1), kTimedOut);

	EXPECT_NE(waiting.push_bgn, before.push_bgn) << "nvif's wait never moved the push buffer";
	EXPECT_TRUE(pointers() == before);
	EXPECT_TRUE(kicks.empty());
	EXPECT_TRUE(held());
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(switch_exec_backing::pending(written), 0u);
}

// SubmitJob and SubmitAddress begin no transaction for a job with no pushes: owned is the empty one.
TEST_F(ExecCommit, NoPushesNoForcedFenceReturnsWithoutAKick)
{
	const uint64_t own = ioLockTakes().load(), live = switch_exec_mutex_takes();
	ASSERT_EQ(exec(nullptr, 0), 0);

	EXPECT_EQ(seqno, pointers().submitted);
	EXPECT_TRUE(kicks.empty());
	EXPECT_FALSE(held());
	EXPECT_EQ(ioLockTakes().load() - own, 0u);
	EXPECT_EQ(switch_exec_mutex_takes() - live, 0u);
	EXPECT_EQ(switch_exec_backing::pending(written), 0u);
	const uint64_t forced = ioLockTakes().load();
	ASSERT_EQ(exec(nullptr, 0, kJob | SWITCH_GPU_SUBMIT_FORCE_FENCE), 0);
	EXPECT_EQ(ioLockTakes().load() - forced, 0u);
	EXPECT_EQ(switch_exec_mutex_takes() - live, 0u);
	expect_published(false, false);
}

TEST_F(ExecCommit, NoPushesCommitsABegunTransaction)
{
	ASSERT_EQ(begin({{written, SWITCH_GPU_BACKING_WRITE, 0}}), 0);

	ASSERT_EQ(exec(nullptr, 0), 0);

	EXPECT_EQ(seqno, pointers().submitted);
	EXPECT_TRUE(kicks.empty());
	EXPECT_FALSE(held());
}

TEST_F(ExecCommit, ProducerRemovedWhileFindingRoomRejectsAndRestoresTheRing)
{
	switch_gpu_backing *gone_read = switch_exec_backing::make();
	switch_gpu_backing *kept_read = switch_exec_backing::make();
	switch_exec_chan *gone = producer(0x103, 3, gone_read);
	producer(0x104, 4, kept_read);
	const uint32_t near_end = pointers().push_max - 8;
	switch_exec_chan_place(consumer, near_end, 10, 32);
	switch_exec_chan_set_get(consumer, near_end, 10);
	bool left = false;
	read_get = [&](switch_exec_chan *chan, bool gpfifo, uint32_t calls) {
		if (chan == consumer && !gpfifo && calls == 1) {
			switch_exec_chan_live(gone, 0);
			left = true;
		}
	};
	ASSERT_EQ(begin({{gone_read, SWITCH_GPU_BACKING_READ, 0},
			 {kept_read, SWITCH_GPU_BACKING_READ, 0},
			 {written, SWITCH_GPU_BACKING_WRITE, 0}}),
		  0);
	ASSERT_EQ(dependency_count, 2u);
	const switch_exec_chan_pointers before = pointers();

	EXPECT_EQ(exec(&kPush, 1), kNoDevice);

	EXPECT_TRUE(left);
	EXPECT_TRUE(kicks.empty());
	EXPECT_TRUE(pointers() == before);
	EXPECT_TRUE(held());
	EXPECT_EQ(switch_exec_chan_acquires(consumer, gone, 1), 0u);
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(switch_exec_backing::pending(written), 0u);
}

TEST_F(ExecCommit, ProducersStillLiveAreAcquired)
{
	switch_gpu_backing *first_read = switch_exec_backing::make();
	switch_gpu_backing *second_read = switch_exec_backing::make();
	switch_exec_chan *first = producer(0x103, 3, first_read);
	switch_exec_chan *second = producer(0x104, 4, second_read);
	ASSERT_EQ(begin({{first_read, SWITCH_GPU_BACKING_READ, 0},
			 {second_read, SWITCH_GPU_BACKING_READ, 0},
			 {written, SWITCH_GPU_BACKING_WRITE, 0}}),
		  0);

	ASSERT_EQ(exec(&kPush, 1), 0);

	expect_published();
	EXPECT_EQ(switch_exec_chan_acquires(consumer, first, 1), 1u);
	EXPECT_EQ(switch_exec_chan_acquires(consumer, second, 1), 1u);
}

TEST_F(ExecCommit, OwnershipContenderEntersThePublishedRecordOnlyAfterGpPutAndFinish)
{
	ASSERT_EQ(begin({{written, SWITCH_GPU_BACKING_WRITE, 0}}), 0);
	std::mutex lock;
	std::condition_variable changed;
	bool trying = false, entered = false, doorbell = false;
	uint32_t early = 0, observed_put = 0;
	std::thread contender([&] {
		{
			std::lock_guard<std::mutex> hold(lock);
			trying = true;
			changed.notify_all();
		}
		switch_gpu_backing_use read = {written, SWITCH_GPU_BACKING_READ, 0};
		switch_gpu_backing_transaction next = {};
		const auto token = switch_exec_backing::token(0x999, 1);
		EXPECT_EQ(switch_gpu_backing_begin_held(&read, 1, nullptr, 0, nullptr, 0, &token,
							&next),
			  0);
		EXPECT_TRUE(switch_exec_backing::recorded(written, completion));
		observed_put = pointers().gp_put;
		{
			std::lock_guard<std::mutex> hold(lock);
			early += !doorbell;
			entered = true;
			changed.notify_all();
		}
		switch_gpu_backing_abort(&next);
	});
	{
		std::unique_lock<std::mutex> hold(lock);
		changed.wait(hold, [&] {
			return trying;
		});
	}
	before_doorbell = [&] {
		std::unique_lock<std::mutex> hold(lock);
		if (!switch_exec_backing::order_held())
			EXPECT_TRUE(changed.wait_for(hold, std::chrono::seconds(2), [&] {
				return entered;
			}));
		EXPECT_FALSE(entered);
		doorbell = true;
	};
	const int result = exec(&kPush, 1);
	switch_gpu_backing_abort(&transaction);
	contender.join();
	EXPECT_EQ(result, 0);
	EXPECT_EQ(early, 0u);
	EXPECT_EQ(observed_put, pointers().gpfifo_cur);
	expect_published();
}

TEST_F(ExecCommit, ForcedEmptyWaitJobsSealOnlyTheExistingOwnershipOrder)
{
	switch_exec_chan *source = switch_exec_chan_new(0x120, 1);
	switch_exec_chan_live(source, 1);
	for (unsigned mode = 0; mode < 4; ++mode) {
		SCOPED_TRACE(mode);
		consumer = switch_exec_chan_new(0x110 + mode, 2 + mode);
		if (mode < 2)
			words = {{0x100, 1, 0x120}};
		else if (mode == 2)
			siblings = {{switch_exec_chan_tgm(source), 1}};
		else
			switch_exec_chan_task_order(consumer, source, 1);
		switch_exec_chan_high(consumer, mode == 1);
		const uint64_t own = ioLockTakes().load(), live = switch_exec_mutex_takes();
		ASSERT_EQ(exec(nullptr, 0, kJob | SWITCH_GPU_SUBMIT_FORCE_FENCE), 0);
		EXPECT_EQ(ioLockTakes().load() - own, 1u);
		EXPECT_EQ(switch_exec_mutex_takes() - live, mode == 1 || mode == 3 ? 1u : 0u);
		expect_published(false);
		if (mode < 2)
			EXPECT_EQ(switch_exec_chan_word_acquires(consumer, 0x100, 1), 1u);
		else
			EXPECT_EQ(switch_exec_chan_acquires(consumer, source, 1), 1u);
		words.clear();
		siblings.clear();
		kicks.clear();
	}
}

TEST_F(ExecCommit, CombinedWaitsReadEachDependencyOnceUnderOneLiveHold)
{
	auto *a = switch_exec_backing::make(), *b = switch_exec_backing::make();
	auto *first = producer(0x103, 3, a), *second = producer(0x104, 4, b);
	ASSERT_EQ(begin({{a, SWITCH_GPU_BACKING_READ, 0},
			 {b, SWITCH_GPU_BACKING_READ, 0},
			 {written, SWITCH_GPU_BACKING_WRITE, 0}}),
		  0);
	for (uint32_t pool : {0x100u, 0x104u})
		dependencies[dependency_count++] = {1, 1, SWITCH_GPU_ENGINE_PROMISE, pool};
	for (uint32_t id : {7u, 8u})
		dependencies[dependency_count++] = {3, 1, SWITCH_GPU_ENGINE_VIC, id};
	siblings = {{switch_exec_chan_tgm(second), 2}};
	words = {{0x108, 5, 0x103}};
	switch_exec_chan_high(consumer, 1);
	switch_exec_chan_task_order(consumer, first, 2);
	const uint64_t status = switch_exec_backing::status_reads(),
		       live = switch_exec_mutex_takes();
	const uint32_t parked = switch_exec_parked_promises();
	ASSERT_EQ(exec(&kPush, 1), 0);
	EXPECT_EQ(switch_exec_backing::status_reads() - status, dependency_count);
	EXPECT_EQ(switch_exec_mutex_takes() - live, 1u);
	EXPECT_EQ(switch_exec_parked_promises() - parked, 2u);
	expect_published();
	for (auto *source : {first, second})
		EXPECT_EQ(switch_exec_chan_acquires(consumer, source, 1), 1u);
	EXPECT_EQ(switch_exec_chan_acquires(consumer, first, 2), 1u);
	EXPECT_EQ(switch_exec_chan_acquires(consumer, second, 2), 1u);
	for (uint32_t pool : {0x100u, 0x104u})
		EXPECT_EQ(switch_exec_chan_word_acquires(consumer, pool, 1), 1u);
	EXPECT_EQ(switch_exec_chan_word_acquires(consumer, 0x108, 5), 1u);
	for (uint32_t id : {7u, 8u})
		EXPECT_EQ(switch_exec_chan_syncpt_waits(consumer, id, 3), 1u);
}

TEST_F(ExecCommit, PlainAndHostOnlyJobsTakeNoLiveLock)
{
	for (uint32_t host : {0u, 1u}) {
		SCOPED_TRACE(host);
		if (host)
			dependencies[dependency_count++] = {3, 1, SWITCH_GPU_ENGINE_VIC, 7};
		const uint64_t own = ioLockTakes().load(), live = switch_exec_mutex_takes();
		const uint64_t status = switch_exec_backing::status_reads();
		ASSERT_EQ(exec(&kPush, 1), 0);
		EXPECT_EQ(switch_exec_mutex_takes() - live, 0u);
		EXPECT_EQ(ioLockTakes().load() - own, host);
		EXPECT_EQ(switch_exec_backing::status_reads() - status, host);
		expect_published(false, host != 0);
		if (host)
			EXPECT_EQ(switch_exec_chan_syncpt_waits(consumer, 7, 3), 1u);
		kicks.clear();
	}
}
