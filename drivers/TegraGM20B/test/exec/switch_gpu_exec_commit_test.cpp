// SPDX-License-Identifier: MIT
// switch_gpu_chan_exec_events, compiled from gpu_exec.c, against the real backing layer: a job's records go up after
// everything that can still fail it and before its first GPFIFO entry or doorbell.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
#include <IOKit/IOLib.h>
#include "drivers/TegraGM20B/TegraGM20BABI.h"
#include "drivers/TegraGM20B/TegraGM20BGpu.h"
#include "drivers/TegraGM20B/test/exec/switch_gpu_exec_backing.h"
#include "drivers/TegraGM20B/test/exec/switch_gpu_exec_fixture.h"

namespace
{
// gpu_exec.c returns the shim's Linux errno values.
constexpr int kAgain = -11;
constexpr int kNoDevice = -19;
constexpr int kTimedOut = -110;
constexpr uint32_t kGm20b = 1u << SWITCH_GPU_ENGINE_GM20B;
constexpr uint32_t kJob = SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT;
constexpr uint64_t kConsumer = 0x102;
const tgm_push kPush = {0x40000000, 16, 0, 0, 0, 0};

struct Kick {
	bool doorbell, main;
	uint64_t addr;
	uint32_t bytes;
	bool committed, unlocked;
	uint32_t engines;
};

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

	int exec(const tgm_push *pushes, uint32_t count)
	{
		return switch_gpu_chan_exec_events(switch_exec_chan_tgm(consumer), pushes, count,
						   dependencies, dependency_count, nullptr, 0,
						   nullptr, 0, &seqno, nullptr, kJob, nullptr, 0,
						   &transaction);
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

	// What the GPU could see at this kick: the transaction already committed, no backing lock held, and the job's
	// write recorded on the backing it writes. The backing is read only once the transaction let go of it: a thread
	// holding an ordered transaction may not enter a backing (the ownership order lock is not recursive).
	void kicked(bool doorbell, bool main, uint64_t addr, uint32_t bytes)
	{
		Kick kick = {doorbell,
			     main,
			     addr,
			     bytes,
			     !held() && !transaction.uses,
			     ioLocksHeld().empty(),
			     0};
		if (kick.committed)
			kick.engines = switch_exec_backing::pending(written);
		kicks.push_back(kick);
	}

	void expect_every_kick_after_commit() const
	{
		ASSERT_FALSE(kicks.empty());
		for (size_t i = 0; i < kicks.size(); ++i) {
			EXPECT_TRUE(kicks[i].committed) << "kick " << i << " before the commit";
			EXPECT_TRUE(kicks[i].unlocked) << "kick " << i << " under a backing lock";
			EXPECT_EQ(kicks[i].engines & kGm20b, kGm20b)
				<< "kick " << i << " before the job's write was visible";
		}
		EXPECT_TRUE(kicks.back().doorbell);
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
	std::function<void(switch_exec_chan *, bool, uint32_t)> read_get;
};

bool operator==(const switch_exec_chan_pointers &a, const switch_exec_chan_pointers &b)
{
	return a.gpfifo_cur == b.gpfifo_cur && a.gp_put == b.gp_put && a.submitted == b.submitted &&
	       a.gpfifo_free == b.gpfifo_free && a.push_bgn == b.push_bgn &&
	       a.push_cur == b.push_cur && a.push_end == b.push_end && a.defer_kick == b.defer_kick;
}
}  // namespace

TEST_F(ExecCommit, PublishesTheJobsWriteBeforeItsFirstKick)
{
	switch_gpu_backing *read = switch_exec_backing::make();
	switch_exec_chan *source = producer(0x101, 1, read);
	ASSERT_EQ(switch_exec_backing::pending(written), 0u);
	ASSERT_EQ(
		begin({{read, SWITCH_GPU_BACKING_READ, 0}, {written, SWITCH_GPU_BACKING_WRITE, 0}}),
		0);
	ASSERT_EQ(dependency_count, 1u);

	ASSERT_EQ(exec(&kPush, 1), 0);

	expect_every_kick_after_commit();
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

// A use a CPU lock is asleep on (its claim) and CPU-written: the job holds that use's lock until its commit, and lets it go
// with the order before its first kick.
TEST_F(ExecCommit, AClaimedUsesLockIsLetGoBeforeTheFirstKick)
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

	expect_every_kick_after_commit();
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

TEST_F(ExecCommit, ProducerNotLiveAtTheFirstPassPublishesNothing)
{
	switch_gpu_backing *read = switch_exec_backing::make();
	switch_exec_chan *source = producer(0x101, 1, read);
	switch_exec_chan_live(source, 0);
	ASSERT_EQ(
		begin({{read, SWITCH_GPU_BACKING_READ, 0}, {written, SWITCH_GPU_BACKING_WRITE, 0}}),
		0);
	ASSERT_EQ(dependency_count, 1u);
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
	ASSERT_EQ(exec(nullptr, 0), 0);

	EXPECT_EQ(seqno, pointers().submitted);
	EXPECT_TRUE(kicks.empty());
	EXPECT_FALSE(held());
	EXPECT_EQ(switch_exec_backing::pending(written), 0u);
}

TEST_F(ExecCommit, NoPushesCommitsABegunTransaction)
{
	ASSERT_EQ(begin({{written, SWITCH_GPU_BACKING_WRITE, 0}}), 0);

	ASSERT_EQ(exec(nullptr, 0), 0);

	EXPECT_EQ(seqno, pointers().submitted);
	EXPECT_TRUE(kicks.empty());
	EXPECT_FALSE(held());
}

TEST_F(ExecCommit, ProducerGoneBeforeTheSecondPassIsNotAcquired)
{
	switch_gpu_backing *gone_read = switch_exec_backing::make();
	switch_gpu_backing *kept_read = switch_exec_backing::make();
	switch_exec_chan *gone = producer(0x103, 3, gone_read);
	switch_exec_chan *kept = producer(0x104, 4, kept_read);
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

	ASSERT_EQ(exec(&kPush, 1), 0);

	EXPECT_TRUE(left);
	expect_every_kick_after_commit();
	EXPECT_TRUE(pushed_user_entry());
	EXPECT_EQ(switch_exec_chan_acquires(consumer, gone, 1), 0u);
	EXPECT_EQ(switch_exec_chan_acquires(consumer, kept, 1), 1u);
	EXPECT_EQ(pointers().submitted, seqno);
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

	expect_every_kick_after_commit();
	EXPECT_EQ(switch_exec_chan_acquires(consumer, first, 1), 1u);
	EXPECT_EQ(switch_exec_chan_acquires(consumer, second, 1), 1u);
}
