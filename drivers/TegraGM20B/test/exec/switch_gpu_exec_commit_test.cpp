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
	uint32_t gp_put, submitted;
};

TEST(ExecEmpty, FirstWordWaitNeedsNoRegisteredBacking)
{
	uint32_t doorbells = 0, seqno = 0;
	const switch_exec_hooks hooks = {&doorbells, nullptr,
					 [](void *context, switch_exec_chan *, uint32_t) {
						 ++*static_cast<uint32_t *>(context);
						 EXPECT_TRUE(ioLocksHeld().empty());
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
	EXPECT_EQ(ioLockTakes().load() - own, 0u)
		<< "a job with no records took the ownership order";
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
		const bool locked = !uses.empty() && switch_exec_backing::lock_held(written);
		const switch_exec_chan_pointers now = pointers();
		Kick kick = {doorbell,
			     main,
			     addr,
			     bytes,
			     locked && switch_exec_backing::recorded(written, completion),
			     ioLocksHeld().size() == (locked ? uses.size() : size_t(0)),
			     !switch_exec_mutex_held(),
			     locked,
			     now.gp_put,
			     now.submitted};
		kicks.push_back(kick);
		if (doorbell && before_doorbell)
			before_doorbell();
	}

	// An ordered job holds its uses' per-backing locks until its channel's submitted seqno names it,
	// and lets them go before its fence's own kick and GP_PUT.
	void expect_published(bool has_write = true, bool ordered = true) const
	{
		ASSERT_FALSE(kicks.empty());
		uint32_t doorbells = 0;
		for (size_t i = 0; i < kicks.size(); ++i) {
			const Kick &kick = kicks[i];
			SCOPED_TRACE(i);
			EXPECT_EQ(kick.ordered, ordered && kick.submitted != seqno);
			EXPECT_EQ(kick.gp_put, kicks.front().gp_put);
			if (kick.doorbell || !kick.main || kick.recorded) {
				EXPECT_TRUE(!has_write || !kick.ordered || kick.recorded);
				EXPECT_TRUE(kick.backing_free);
				EXPECT_TRUE(kick.live_free);
			}
			doorbells += kick.doorbell;
		}
		EXPECT_EQ(doorbells, 1u);
		EXPECT_TRUE(kicks.back().doorbell);
		EXPECT_EQ(pointers().gp_put, pointers().gpfifo_cur);
		EXPECT_FALSE(switch_exec_backing::any_lock_held());
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

// SubmitJob begins no transaction for a job with no pushes: owned is the empty one.
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

// A reader that finds the job's record waits for a seqno a kill of the channel releases (tgm_chan_killed writes
// chan->submitted to the fence), and the job's doorbell, MMIO and PLL work are not inside the ownership order.
TEST_F(ExecCommit, AContenderEntersOnceTheRecordsSeqnoIsSubmittedAndBeforeGpPut)
{
	ASSERT_EQ(begin({{written, SWITCH_GPU_BACKING_WRITE, 0}}), 0);
	std::mutex lock;
	std::condition_variable changed;
	bool trying = false, entered = false, entered_first = false;
	uint32_t observed_submitted = 0;
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
		observed_submitted = pointers().submitted;
		{
			std::lock_guard<std::mutex> hold(lock);
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
		entered_first = changed.wait_for(hold, std::chrono::seconds(2), [&] {
			return entered;
		});
	};
	const int result = exec(&kPush, 1);
	switch_gpu_backing_abort(&transaction);
	contender.join();
	EXPECT_EQ(result, 0);
	EXPECT_TRUE(entered_first) << "the job rang its doorbell inside the ownership order";
	EXPECT_EQ(observed_submitted, seqno)
		<< "a reader found the record before the channel's submitted seqno named it";
	expect_published();
}

// Raised by switch_gpu_chan_submit_enter before taking backing locks: a PLL switch that may sleep on the clock's mutex,
// never under the ownership order every CPU backing operation takes.
TEST_F(ExecCommit, AnIdleClockGoesUpOutsideTheOwnershipOrderBeforeTheDoorbell)
{
	struct Raise {
		const std::vector<Kick> *kicks;
		uint32_t raises;
		bool ordered, after_doorbell;
	} raise = {&kicks, 0, false, false};

	switch_exec_idle_low(
		[](void *context) {
			auto *r = static_cast<Raise *>(context);
			++r->raises;
			r->ordered |= switch_exec_backing::any_lock_held();
			for (const Kick &kick : *r->kicks)
				r->after_doorbell |= kick.doorbell;
		},
		&raise);

	switch_gpu_chan_submit_enter(switch_exec_chan_tgm(consumer));
	ASSERT_EQ(begin({{written, SWITCH_GPU_BACKING_WRITE, 0}}), 0);
	const int result = exec(&kPush, 1);
	switch_gpu_chan_submit_leave();
	switch_exec_idle_low(nullptr, nullptr);

	EXPECT_EQ(result, 0);
	EXPECT_EQ(raise.raises, 1u);
	EXPECT_FALSE(raise.ordered) << "the PLL switch ran inside the ownership order";
	EXPECT_FALSE(raise.after_doorbell);
	expect_published();
}

TEST_F(ExecCommit, ForcedEmptyWaitJobsTakeNoOwnershipOrder)
{
	switch_exec_chan *source = switch_exec_chan_new(0x120, 1);
	switch_exec_chan_live(source, 1);
	for (unsigned mode = 0; mode < 3; ++mode) {
		SCOPED_TRACE(mode);
		consumer = switch_exec_chan_new(0x110 + mode, 2 + mode);
		if (mode < 2)
			words = {{0x100, 1, 0x120}};
		else
			siblings = {{switch_exec_chan_tgm(source), 1, 0}};
		switch_exec_chan_high(consumer, mode == 1);
		const uint64_t own = ioLockTakes().load(), live = switch_exec_mutex_takes();
		ASSERT_EQ(exec(nullptr, 0, kJob | SWITCH_GPU_SUBMIT_FORCE_FENCE), 0);
		EXPECT_EQ(ioLockTakes().load() - own, 0u);
		EXPECT_EQ(switch_exec_mutex_takes() - live, 0u);
		expect_published(false, false);
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
	siblings = {{switch_exec_chan_tgm(second), 2, 0}};
	words = {{0x108, 5, 0x103}};
	switch_exec_chan_high(consumer, 1);
	const uint64_t status = switch_exec_backing::status_reads(),
		       live = switch_exec_mutex_takes();
	const uint32_t parked = switch_exec_parked_promises();
	ASSERT_EQ(exec(&kPush, 1), 0);
	EXPECT_EQ(switch_exec_backing::status_reads() - status, dependency_count);
	EXPECT_EQ(switch_exec_mutex_takes() - live, 0u);
	EXPECT_EQ(switch_exec_parked_promises() - parked, 2u);
	expect_published();
	for (auto *source : {first, second})
		EXPECT_EQ(switch_exec_chan_acquires(consumer, source, 1), 1u);
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
		EXPECT_EQ(ioLockTakes().load() - own, 0u);
		EXPECT_EQ(switch_exec_backing::status_reads() - status, host);
		expect_published(false, false);
		if (host)
			EXPECT_EQ(switch_exec_chan_syncpt_waits(consumer, 7, 3), 1u);
		kicks.clear();
	}
}

TEST_F(ExecCommit, DependenciesAcquireWithoutTakingLiveLockEvenWhileHeld)
{
	auto *a = switch_exec_backing::make();
	auto *source = producer(0x105, 3, a);
	ASSERT_EQ(begin({{a, SWITCH_GPU_BACKING_READ, 0}, {written, SWITCH_GPU_BACKING_WRITE, 0}}),
		  0);
	switch_exec_chan_high(consumer, 1);

	std::mutex gate;
	std::condition_variable cv;
	bool holding = false;
	bool release = false;
	std::thread holder([&] {
		switch_exec_live_lock();
		{
			std::lock_guard<std::mutex> g(gate);
			holding = true;
			cv.notify_all();
		}
		{
			std::unique_lock<std::mutex> g(gate);
			cv.wait(g, [&] {
				return release;
			});
		}
		switch_exec_live_unlock();
	});
	{
		std::unique_lock<std::mutex> g(gate);
		cv.wait(g, [&] {
			return holding;
		});
	}

	const uint64_t live = switch_exec_mutex_takes();
	const int ret = exec(&kPush, 1);
	const uint64_t live_after = switch_exec_mutex_takes();
	{
		std::lock_guard<std::mutex> g(gate);
		release = true;
		cv.notify_all();
	}
	holder.join();

	EXPECT_EQ(ret, 0);
	EXPECT_EQ(live_after - live, 0u);
	EXPECT_EQ(switch_exec_chan_acquires(consumer, source, 1), 1u);
}

TEST_F(ExecCommit, LaterCompositorKicksCountFramesPerFrameMaxAndQueueMax)
{
	switch_exec_chan_high(consumer, 1);
	switch_exec_chan_critical(consumer, 1);

	const auto submit = [&](switch_exec_chan *chan, switch_exec_kick_kind kick, int later,
				uint64_t sub_pt, uint64_t queue_pt) {
		switch_exec_set_kick(kick, later);
		switch_exec_set_ptimer(sub_pt);
		uint32_t out = 0;
		EXPECT_EQ(switch_gpu_chan_exec_events(switch_exec_chan_tgm(chan), &kPush, 1,
						      nullptr, 0, nullptr, 0, nullptr, 0, &out,
						      nullptr, kJob, nullptr, 0, nullptr),
			  0);
		switch_exec_chan_complete_stamp(chan, out, sub_pt + queue_pt,
						sub_pt + queue_pt + 100);
		kicks.clear();
		return out;
	};

	submit(consumer, SWITCH_EXEC_KICK_PREEMPTED, 0, 1000, 8000);
	submit(consumer, SWITCH_EXEC_KICK_CLEAR, 1, 10000, 7000);
	const uint32_t marked_seqno = submit(consumer, SWITCH_EXEC_KICK_PREEMPTED, 1, 20000, 2500);
	submit(consumer, SWITCH_EXEC_KICK_SWITCHING, 1, 30000, 1200);

	submit(consumer, SWITCH_EXEC_KICK_CLEAR, 0, 35000, 500);
	submit(consumer, SWITCH_EXEC_KICK_DEFERRED, 1, 40000, 4000);

	switch_exec_chan *other = switch_exec_chan_new(0x130, 3);
	switch_exec_chan_high(other, 1);
	submit(other, SWITCH_EXEC_KICK_PREEMPTED, 1, 50000, 49000);

	uint64_t pt = 100000;
	uint32_t last = 0;
	for (uint32_t tries = 0; tries < 80 && last != marked_seqno + 64; ++tries) {
		pt += 1000;
		last = submit(consumer, SWITCH_EXEC_KICK_PREEMPTED, 0, pt, 50000);
	}
	EXPECT_EQ(last, marked_seqno + 64);

	switch_exec_later_stats got = {};
	switch_exec_take_later_stats(&got);
	EXPECT_EQ(got.kicks, 3u);
	EXPECT_EQ(got.frames, 2u);
	EXPECT_EQ(got.frame_max, 2u);
	EXPECT_EQ(got.queue_max, 4000u);

	switch_exec_take_later_stats(&got);
	EXPECT_EQ(got.kicks, 0u);
	EXPECT_EQ(got.frames, 0u);
	EXPECT_EQ(got.frame_max, 0u);
	EXPECT_EQ(got.queue_max, 0u);
}

TEST_F(ExecCommit, CompositorForeignWaitsCountFencesPromisesEventsHost1xAndQueueImpact)
{
	switch_exec_chan_high(consumer, 1);
	switch_exec_chan_critical(consumer, 1);

	switch_exec_chan *same_vm = switch_exec_chan_new(0x131, 2);
	switch_exec_chan_share_vm(same_vm, consumer);
	switch_exec_chan_live(same_vm, 1);

	switch_exec_chan *foreign_a = switch_exec_chan_new(0x132, 3);
	switch_exec_chan *foreign_b = switch_exec_chan_new(0x133, 4);
	switch_exec_chan_live(foreign_a, 1);
	switch_exec_chan_live(foreign_b, 1);

	switch_exec_chan *non_comp = switch_exec_chan_new(0x134, 5);
	switch_exec_chan_high(non_comp, 1);

	const auto run_one = [&](switch_exec_chan *chan,
				 const std::vector<switch_gpu_backing_token> &deps,
				 const std::vector<switch_gpu_word_wait> &word_list,
				 uint64_t sub_pt, uint64_t queue_pt) {
		switch_exec_set_ptimer(sub_pt);
		uint32_t out = 0;
		const int ret = switch_gpu_chan_exec_events(
			switch_exec_chan_tgm(chan), &kPush, 1, deps.empty() ? nullptr : deps.data(),
			uint32_t(deps.size()), nullptr, 0,
			word_list.empty() ? nullptr : word_list.data(), uint32_t(word_list.size()),
			&out, nullptr, kJob, nullptr, 0, nullptr);
		if (!ret)
			switch_exec_chan_complete_stamp(chan, out, sub_pt + queue_pt,
							sub_pt + queue_pt + 100);
		kicks.clear();
		return std::pair<int, uint32_t>{ret, out};
	};

	const switch_gpu_backing_token own_fence = switch_exec_backing::token(0x131, 1);
	const switch_gpu_backing_token settled_foreign = switch_exec_backing::token(0x132, 1);
	switch_exec_backing::complete(settled_foreign);
	const switch_gpu_backing_token foreign_fence = switch_exec_backing::token(0x132, 2);
	const switch_gpu_backing_token foreign_fence_b = switch_exec_backing::token(0x133, 2);
	const switch_gpu_backing_token own_promise = {1, 1, SWITCH_GPU_ENGINE_PROMISE,
						      2u * 16384u + 0x100u};
	const switch_gpu_backing_token foreign_promise = {1, 1, SWITCH_GPU_ENGINE_PROMISE,
							  3u * 16384u + 0x100u};
	const switch_gpu_backing_token vic_wait = {3, 1, SWITCH_GPU_ENGINE_VIC, 7};
	const switch_gpu_word_wait own_event = {2u * 16384u + 0x200u, 1, 0x131};
	const switch_gpu_word_wait own_event_no_queue = {2u * 16384u + 0x204u, 1, 0};
	const switch_gpu_word_wait foreign_event = {3u * 16384u + 0x200u, 2, 0x132};
	const switch_gpu_word_wait foreign_event_no_queue = {4u * 16384u + 0x200u, 2, 0};

	EXPECT_EQ(run_one(consumer, {own_fence, settled_foreign, own_promise},
			  {own_event, own_event_no_queue}, 1000, 9000)
			  .first,
		  0);

	const auto [r1, marked_seqno] = run_one(consumer, {foreign_fence}, {}, 20000, 3500);
	EXPECT_EQ(r1, 0);

	EXPECT_EQ(run_one(consumer, {foreign_promise}, {}, 40000, 7000).first, 0);

	EXPECT_EQ(run_one(consumer, {}, {foreign_event, foreign_event_no_queue}, 60000, 2500).first,
		  0);

	EXPECT_EQ(run_one(consumer, {foreign_fence, foreign_fence_b, foreign_promise, vic_wait},
			  {foreign_event}, 80000, 5000)
			  .first,
		  0);

	EXPECT_EQ(run_one(consumer, {vic_wait}, {}, 100000, 12000).first, 0);

	EXPECT_EQ(run_one(non_comp, {foreign_fence, foreign_promise, vic_wait}, {foreign_event},
			  120000, 99000)
			  .first,
		  0);

	const switch_gpu_backing_token missing_source = switch_exec_backing::token(0x999, 1);
	EXPECT_EQ(run_one(consumer, {foreign_fence, missing_source}, {}, 140000, 88000).first,
		  kNoDevice);

	uint64_t pt = 200000;
	uint32_t last = 0;
	for (uint32_t tries = 0; tries < 80 && last != marked_seqno + 64; ++tries) {
		pt += 1000;
		const auto [r, s] = run_one(consumer, {}, {}, pt, 50000);
		EXPECT_EQ(r, 0);
		last = s;
	}
	EXPECT_EQ(last, marked_seqno + 64);

	switch_exec_foreign_stats got = {};
	switch_exec_take_foreign_stats(&got);
	EXPECT_EQ(got.jobs, 4u);
	EXPECT_EQ(got.fences, 3u);
	EXPECT_EQ(got.promises, 2u);
	EXPECT_EQ(got.events, 3u);
	EXPECT_EQ(got.host1x, 2u);
	EXPECT_EQ(got.timed, 4u);
	EXPECT_EQ(got.queue_sum, 3500u + 7000u + 2500u + 5000u);
	EXPECT_EQ(got.queue_max, 7000u);

	switch_exec_take_foreign_stats(&got);
	EXPECT_EQ(got.jobs, 0u);
	EXPECT_EQ(got.fences, 0u);
	EXPECT_EQ(got.promises, 0u);
	EXPECT_EQ(got.events, 0u);
	EXPECT_EQ(got.host1x, 0u);
	EXPECT_EQ(got.timed, 0u);
	EXPECT_EQ(got.queue_sum, 0u);
	EXPECT_EQ(got.queue_max, 0u);

	struct tgm_foreign_waits totals = {};
	tgm_foreign_waits_totals(&totals);
	EXPECT_EQ(totals.jobs, 4u);
	EXPECT_EQ(totals.fences, 3u);
	EXPECT_EQ(totals.promises, 2u);
	EXPECT_EQ(totals.events, 3u);
	EXPECT_EQ(totals.host1x, 2u);

	EXPECT_EQ(run_one(consumer, {foreign_fence}, {}, pt + 1000, 1500).first, 0);
	tgm_foreign_waits_totals(&totals);
	EXPECT_EQ(totals.jobs, 5u);
	EXPECT_EQ(totals.fences, 4u);
	EXPECT_EQ(totals.promises, 2u);
	EXPECT_EQ(totals.events, 3u);
	EXPECT_EQ(totals.host1x, 2u);
}

TEST_F(ExecCommit, L2InvalidateStatsSplitHostWrittenTlbAndKernelPush)
{
	const auto submit = [&](switch_exec_chan *chan, uint32_t visibility) {
		uint32_t out = 0;
		ASSERT_EQ(switch_gpu_chan_exec_events(switch_exec_chan_tgm(chan), &kPush, 1,
						      nullptr, 0, nullptr, 0, nullptr, 0, &out,
						      nullptr, visibility, nullptr, 0, nullptr),
			  0);
		kicks.clear();
	};

	switch_exec_chan *compositor = switch_exec_chan_new(0x140, 3);
	switch_exec_chan_critical(compositor, 1);
	switch_exec_chan *kernel = switch_exec_chan_new(0x141, 4);
	switch_exec_chan_kernel(kernel, 1);

	// Plain explicit job: no L2 invalidate.
	submit(consumer, kJob);
	// App host-written job.
	submit(consumer, kJob | SWITCH_GPU_SUBMIT_HOST_WRITTEN);
	// Compositor host-written job.
	submit(compositor, kJob | SWITCH_GPU_SUBMIT_HOST_WRITTEN);
	// TLB-only job (not host-written).
	switch_exec_chan_dirty_tlb(consumer);
	submit(consumer, kJob);
	// Both host-written and TLB-dirty on one user submission.
	switch_exec_chan_dirty_tlb(consumer);
	submit(consumer, kJob | SWITCH_GPU_SUBMIT_HOST_WRITTEN);
	// Kernel channel host-written push.
	submit(kernel, kJob | SWITCH_GPU_SUBMIT_HOST_WRITTEN);

	switch_exec_l2_inval_stats got = {};
	switch_exec_take_l2_inval_stats(&got);
	EXPECT_EQ(got.total, 5u);
	EXPECT_EQ(got.host_written, 3u);
	EXPECT_EQ(got.compositor, 1u);
	EXPECT_EQ(got.tlb, 2u);
	EXPECT_EQ(got.kernel_push, 1u);

	switch_exec_take_l2_inval_stats(&got);
	EXPECT_EQ(got.total, 0u);
	EXPECT_EQ(got.host_written, 0u);
	EXPECT_EQ(got.compositor, 0u);
	EXPECT_EQ(got.tlb, 0u);
	EXPECT_EQ(got.kernel_push, 0u);
}
