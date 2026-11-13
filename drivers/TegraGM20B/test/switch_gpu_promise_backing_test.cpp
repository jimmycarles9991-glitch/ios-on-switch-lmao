// SPDX-License-Identifier: MIT
// Writer promises on a backing (SwitchGpuWriterPromise.h): a display snapshot takes them as GPU tokens behind a lease the
// promised job's write admission does not wait for, and a CPU reader (a CPU lock, a waiting snapshot) sleeps until the
// promised job is submitted.
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuAdmission.h"
#include "drivers/TegraGM20B/SwitchGpuPresentTokens.h"
#include "drivers/TegraGM20B/SwitchGpuWriterPromise.h"

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t avoid = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
constexpr uint32_t kOffset = 2048;

uint32_t fence[1024];
int holds;
uint64_t abandoned;  // offset << 32 | value of the one promise given up on
}  // namespace

extern "C" int tgm_promise_abandoned(uint32_t offset, uint32_t value)
{
	return abandoned == (uint64_t(offset) << 32 | value);
}

extern "C" volatile uint32_t *tgm_fence_word(uint32_t offset)
{
	return offset < sizeof(fence) && !(offset & 3) ? &fence[offset / 4] : nullptr;
}

extern "C" void switch_gpu_fence_hold(const volatile uint32_t *)
{
	++holds;
}

extern "C" void switch_gpu_fence_put(const volatile uint32_t *)
{
	--holds;
}

extern "C" int tgm_promise_open(void)
{
	return 0;
}

namespace
{
std::mutex parkLock;
std::vector<std::string> parks;  // "who offset value" of each CPU reader's park
unsigned leaves;
}  // namespace

extern "C" int tgm_promise_park(uint32_t offset, uint32_t value, const char *, const char *who,
				const struct switch_gpu_giveup_job *reader, int display)
{
	std::lock_guard<std::mutex> hold(parkLock);
	EXPECT_EQ(reader, nullptr);
	EXPECT_EQ(display, 0);
	parks.push_back(std::string(who) + " " + std::to_string(offset) + " " +
			std::to_string(value));
	return 1;
}

extern "C" void tgm_promise_leave(uint32_t, uint32_t, int display)
{
	std::lock_guard<std::mutex> hold(parkLock);
	EXPECT_EQ(display, 0);
	++leaves;
}

namespace
{
switch_gpu_backing *promised(uint32_t value = 1)
{
	auto *b = backing();
	fence[kOffset / 4] = value - 1;
	const switch_gpu_promise_ref ref = {tgm_fence_word(kOffset), kOffset, value};
	EXPECT_EQ(switch_gpu_backing_promise_add(b, &ref), 0);
	return b;
}

TEST_F(Backing, ADisplaySnapshotTakesThePromisesBehindALeaseTheWriteAdmissionIgnores)
{
	auto *b = promised();
	switch_gpu_backing_token producer = {}, tokens[4] = {};
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_snapshot_read_promises(b, &producer, &lease, tokens, 4), 1);
	EXPECT_EQ(tokens[0].engine, uint32_t(SWITCH_GPU_ENGINE_PROMISE));
	EXPECT_EQ(tokens[0].queue, kOffset);
	EXPECT_EQ(tokens[0].value, 1u);
	SwitchGpuAdmission admission;
	const switch_gpu_backing_use write{b, kWrite, 0};
	EXPECT_EQ(switch_gpu_backing_admission_ready(b, kWrite), 1);
	ASSERT_EQ(admission.begin(&write, 1, nullptr, 0, nullptr, 0), 0);
	admission.end();
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_promise_drop(b, kOffset);
	ASSERT_EQ(switch_gpu_backing_snapshot_read(b, &producer, &lease), 0);
	EXPECT_EQ(admission.begin(&write, 1, nullptr, 0, nullptr, 0), -16);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(b);
}

// The promised job's own submission (its write ownership, a conversion it reserves) goes through while the display holds
// a lease taken behind the promise, which waits for that job on the GPU; any other lease holds the write off.
TEST_F(Backing, ThePromisedJobsOwnSubmissionGoesThroughAPromisedDisplayLease)
{
	auto *b = promised();
	switch_gpu_backing_token producer = {}, tokens[4] = {};
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_snapshot_read_promises(b, &producer, &lease, tokens, 4), 1);
	EXPECT_EQ(switch_gpu_backing_conversion_ready(b, kWrite), 1);
	switch_gpu_cpu_lease *conversion = nullptr;
	ASSERT_EQ(switch_gpu_backing_conversion_reserve(b, kWrite, 1, &conversion), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&conversion, 0), 0);
	const auto job = token(3, 4);
	EXPECT_EQ(publish(b, kWrite, job), 0);
	complete(job);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_promise_drop(b, kOffset);
	ASSERT_EQ(switch_gpu_backing_snapshot_read(b, &producer, &lease), 0);
	EXPECT_EQ(publish(b, kWrite, token(3, 5)), -16);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, APlainSnapshotOfAPromisedSurfaceIsAPromisedLeaseToo)
{
	auto *b = promised();
	switch_gpu_backing_token producer = {};
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_snapshot_read(b, &producer, &lease), 0);
	EXPECT_EQ(switch_gpu_backing_promised(b), 1);
	SwitchGpuAdmission admission;
	const switch_gpu_backing_use write{b, kWrite, 0};
	EXPECT_EQ(admission.begin(&write, 1, nullptr, 0, nullptr, 0), 0);
	admission.end();
	switch_gpu_backing_token tokens[1] = {};
	switch_gpu_cpu_lease *second = nullptr;
	EXPECT_EQ(switch_gpu_backing_snapshot_read_promises(b, &producer, &second, tokens, 0), -28);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_promise_drop(b, kOffset);
	EXPECT_EQ(switch_gpu_backing_promised(b), 0);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, ACpuLockWaitsForThePromisedJobAndAvoidingASyncIsBusy)
{
	auto *b = promised();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), -16);
	std::atomic<int> locked{1};
	std::thread reader([&] {
		locked = switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_EQ(locked.load(), 1);
	const auto job = token(3, 7);
	ASSERT_EQ(publish(b, kWrite | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, job), 0);
	complete(job);
	switch_gpu_backing_promise_drop(b, kOffset);
	reader.join();
	EXPECT_EQ(locked.load(), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead), 0);
	switch_gpu_backing_release(b);
}

// The display's CPU copy reads through the CPU's snapshot: it sleeps until the promised job is submitted and names it.
TEST_F(Backing, TheCpusSnapshotSleepsUntilThePromisedJobIsSubmittedAndNamesIt)
{
	auto *b = promised();
	std::atomic<int> result{1};
	switch_gpu_backing_token producer = {};
	switch_gpu_cpu_lease *lease = nullptr;
	std::thread copy([&] {
		result = switch_gpu_backing_snapshot_read_cpu(b, &producer, &lease);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_EQ(result.load(), 1);
	ASSERT_EQ(publish(b, kWrite | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION, token(3, 9)), 0);
	switch_gpu_backing_promise_drop(b, kOffset);
	copy.join();
	EXPECT_EQ(result.load(), 0);
	EXPECT_EQ(producer.value, 9u);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_release(b);
}

// A CPU lock and the display's CPU copy asleep behind a promise park on it once each for their whole wait, however
// often they are woken meanwhile, so the give-up's second runs from their first sleep; they leave once their wait is
// over (here at the keep). An engine reader's snapshot orders behind the promise on the GPU and parks on nothing.
TEST_F(Backing, CpuReadersParkOnAPromiseOnceForTheirWholeWait)
{
	auto *b = promised();
	{
		std::lock_guard<std::mutex> hold(parkLock);
		parks.clear();
		leaves = 0;
	}
	switch_gpu_backing_token producer = {}, tokens[4] = {};
	switch_gpu_cpu_lease *engine = nullptr, *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_snapshot_read_promises(b, &producer, &engine, tokens, 4), 1);
	std::atomic<int> copied{1}, locked{1};
	std::thread copy([&] {
		copied = switch_gpu_backing_snapshot_read_cpu(b, &producer, &lease);
	});
	std::thread lock([&] {
		locked = switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	for (int i = 0; i < 3; ++i) {
		switch_gpu_backing_promise_sleepers_wake();
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	EXPECT_EQ(copied.load(), 1);
	EXPECT_EQ(locked.load(), 1);
	{
		std::lock_guard<std::mutex> hold(parkLock);
		std::vector<std::string> sorted = parks;
		std::sort(sorted.begin(), sorted.end());
		EXPECT_EQ(sorted,
			  (std::vector<std::string>{"a CPU copy 2048 1", "a CPU lock 2048 1"}));
		EXPECT_EQ(leaves, 0u);
	}
	switch_gpu_backing_promise_drop(b, kOffset);
	copy.join();
	lock.join();
	EXPECT_EQ(copied.load(), 0);
	EXPECT_EQ(locked.load(), 0);
	{
		std::lock_guard<std::mutex> hold(parkLock);
		EXPECT_EQ(parks.size(), 2u);
		EXPECT_EQ(leaves, 2u);
	}
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&engine, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead), 0);
	switch_gpu_backing_release(b);
}

// A writer that keeps promising the surface holds a CPU lock asleep across more promises than its parked list holds,
// and never keeps the last one. The give-up knows a promise only through a park, so the lock must park on that one too:
// the ones no longer pending are left to make room. The give-up of the last (a second after its park,
// switch_gpu_promise_giveup_test) then lets the lock go.
TEST_F(Backing, ACpuLockBehindAWriterThatKeepsPromisingIsLetGoByTheLastOnesGiveUp)
{
	constexpr uint32_t kWords[2] = {kOffset, kOffset + 4};
	constexpr int kPromises = 2 * SWITCH_GPU_BACKING_PROMISES + 1;
	auto *b = promised();
	fence[kWords[1] / 4] = 0;
	{
		std::lock_guard<std::mutex> hold(parkLock);
		parks.clear();
		leaves = 0;
	}
	std::atomic<int> locked{1};
	std::thread lock([&] {
		locked = switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	for (int k = 1; k < kPromises; ++k) {
		const uint32_t next = kWords[k % 2], kept = kWords[(k - 1) % 2];
		const switch_gpu_promise_ref ref = {tgm_fence_word(next), next,
						    fence[next / 4] + 1};
		ASSERT_EQ(switch_gpu_backing_promise_add(b, &ref), 0);
		fence[kept / 4] += 1;
		switch_gpu_backing_promise_drop(b, kept);
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
	}
	const uint32_t last = kWords[(kPromises - 1) % 2], value = fence[last / 4] + 1;
	bool parkedOnLast = false;
	{
		std::lock_guard<std::mutex> hold(parkLock);
		EXPECT_EQ(parks.size(), size_t(kPromises));
		parkedOnLast =
			!parks.empty() && parks.back() == "a CPU lock " + std::to_string(last) +
								  " " + std::to_string(value);
	}
	EXPECT_EQ(locked.load(), 1);
	if (parkedOnLast)
		abandoned = uint64_t(last) << 32 | value;
	switch_gpu_backing_promise_sleepers_wake();
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	EXPECT_EQ(locked.load(), 0)
		<< "the lock is still asleep behind the promise nobody gives up";
	abandoned = 0;
	fence[last / 4] = value;
	switch_gpu_backing_promise_drop(b, last);
	lock.join();
	{
		std::lock_guard<std::mutex> hold(parkLock);
		EXPECT_EQ(leaves, unsigned(kPromises));
	}
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead), 0);
	switch_gpu_backing_release(b);
}

// The give-up (SwitchGpuPromiseGiveUp.h) gives the promise up: CPU readers asleep behind it, a lock and the display's
// CPU copy, go on at the abandonment's wake with no cancel or close of the promise, and read what the surface held
// before (its last writer). The mark alone wakes nobody: the wake is what lets them go.
TEST_F(Backing, CpuReadersAsleepOnAPromiseGivenUpOnGoOnAtItsWake)
{
	auto *b = promised();
	ASSERT_EQ(publish(b, kWrite, token(3, 2)), 0);
	complete(token(3, 2));
	std::atomic<int> copied{1}, locked{1};
	switch_gpu_backing_token producer = {};
	switch_gpu_cpu_lease *lease = nullptr;
	std::thread copy([&] {
		copied = switch_gpu_backing_snapshot_read_cpu(b, &producer, &lease);
	});
	std::thread lock([&] {
		locked = switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead);
	});
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_EQ(copied.load(), 1);
	EXPECT_EQ(locked.load(), 1);
	abandoned = uint64_t(kOffset) << 32 | 1;
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	EXPECT_EQ(copied.load(), 1);
	EXPECT_EQ(locked.load(), 1);
	const auto woken = std::chrono::steady_clock::now();
	switch_gpu_backing_promise_sleepers_wake();
	copy.join();
	lock.join();
	EXPECT_LT(std::chrono::steady_clock::now() - woken, std::chrono::milliseconds(100));
	EXPECT_EQ(copied.load(), 0);
	EXPECT_EQ(locked.load(), 0);
	EXPECT_EQ(producer.value, 2u) << "the frame before the promised one";
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead), 0);
	abandoned = 0;
	switch_gpu_backing_promise_drop(b, kOffset);
	switch_gpu_backing_release(b);
}

// VIC's scanout source sleeps for a CPU writer only: a writer promise is one of its job's dependencies, which the
// GPU-to-host1x bridge acquires, so the snapshot is taken at once behind a lease the promised job's write ignores.
TEST_F(Backing, AnEngineReadersWaitingSnapshotTakesThePromiseAsADependency)
{
	auto *b = promised();
	switch_gpu_backing_token producer = {};
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_snapshot_read_wait(b, &producer, &lease), 0);
	SwitchGpuAdmission admission;
	const switch_gpu_backing_use write{b, kWrite, 0};
	EXPECT_EQ(admission.begin(&write, 1, nullptr, 0, nullptr, 0), 0);
	admission.end();
	switch_gpu_backing_use read{b, kRead, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto vic = token(160, 3, SWITCH_GPU_ENGINE_VIC);
	ASSERT_EQ(switch_gpu_backing_begin(&read, 1, &vic, &transaction), 0);
	switch_gpu_backing_token tokens[1] = {};
	uint32_t needed = 0;
	ASSERT_EQ(switch_gpu_backing_promise_dependencies(&transaction, tokens, 1, &needed, 0, 0),
		  0);
	EXPECT_EQ(tokens[0].engine, uint32_t(SWITCH_GPU_ENGINE_PROMISE));
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	switch_gpu_backing_promise_drop(b, kOffset);
	switch_gpu_backing_release(b);
}

// Another connection's job reads a promised surface: it takes the promise as a token while its transaction holds the
// surface's lock, as SubmitJob does between begin and commit, and its own connection's promises are left out.
TEST_F(Backing, AJobTakesThePromisesUnderItsOwnTransaction)
{
	auto *b = promised();
	switch_gpu_backing_use read{b, kRead, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto job = token(9, 2);
	ASSERT_EQ(switch_gpu_backing_begin(&read, 1, &job, &transaction), 0);
	switch_gpu_backing_token tokens[2] = {};
	uint32_t needed = 1;
	ASSERT_EQ(switch_gpu_backing_promise_dependencies(&transaction, tokens, 2, &needed, 0, 0),
		  0);
	EXPECT_EQ(needed, 2u);
	EXPECT_EQ(tokens[1].engine, uint32_t(SWITCH_GPU_ENGINE_PROMISE));
	EXPECT_EQ(tokens[1].queue, kOffset);
	needed = 0;
	ASSERT_EQ(switch_gpu_backing_promise_dependencies(&transaction, tokens, 2, &needed, kOffset,
							  kOffset + 4),
		  0);
	EXPECT_EQ(needed, 0u);
	needed = 0;
	EXPECT_EQ(switch_gpu_backing_promise_dependencies(&transaction, tokens, 0, &needed, 0, 0),
		  -28);
	EXPECT_EQ(needed, 1u);
	switch_gpu_backing_abort(&transaction);
	switch_gpu_backing_promise_drop(b, kOffset);
	switch_gpu_backing_release(b);
}

// The surface-level read takes the surface's lock, which the transaction already holds: XNU's lock never returns, and
// the model aborts. This is the read SubmitJob made before, from inside its transaction.
TEST_F(Backing, ASurfacesOwnPromiseReadInsideATransactionTakesItsLockTwice)
{
	auto *b = promised();
	switch_gpu_backing_use read{b, kRead, 0};
	switch_gpu_backing_transaction transaction = {};
	const auto job = token(9, 2);
	ASSERT_EQ(switch_gpu_backing_begin(&read, 1, &job, &transaction), 0);
	switch_gpu_backing_token tokens[1] = {};
	EXPECT_DEATH(switch_gpu_backing_promise_tokens(b, 0, 0, tokens, 1),
		     "already holds this lock");
	switch_gpu_backing_abort(&transaction);
	EXPECT_EQ(switch_gpu_backing_promise_tokens(b, 0, 0, tokens, 1), 1);
	switch_gpu_backing_promise_drop(b, kOffset);
	switch_gpu_backing_release(b);
}

// Which engines' work a write by `job` waits for
std::vector<uint32_t> waitedFor(switch_gpu_backing *b, const switch_gpu_backing_token &job)
{
	switch_gpu_backing_use write{b, kWrite, 0};
	switch_gpu_backing_transaction transaction = {};
	std::vector<uint32_t> engines;
	EXPECT_EQ(switch_gpu_backing_begin(&write, 1, &job, &transaction), 0);
	switch_gpu_backing_token out[4] = {};
	uint32_t needed = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, out, 4, &needed), 0);
	for (uint32_t i = 0; i < needed; ++i)
		engines.push_back(out[i].engine);
	switch_gpu_backing_commit(&transaction);
	return engines;
}

// A read of `job` that acquires the surface's pending promises on the GPU, committed
void readBehindPromises(switch_gpu_backing *b, const switch_gpu_backing_token &job,
			uint32_t own_first = 0, uint32_t own_end = 0)
{
	switch_gpu_backing_use read{b, kRead, 0};
	switch_gpu_backing_transaction transaction = {};
	ASSERT_EQ(switch_gpu_backing_begin(&read, 1, &job, &transaction), 0);
	switch_gpu_backing_token tokens[2] = {};
	uint32_t needed = 0;
	ASSERT_EQ(switch_gpu_backing_promise_dependencies(&transaction, tokens, 2, &needed,
							  own_first, own_end),
		  0);
	switch_gpu_backing_commit(&transaction);
}

bool has(const std::vector<uint32_t> &engines, uint32_t engine)
{
	return std::find(engines.begin(), engines.end(), engine) != engines.end();
}

// Another connection's job and a VIC job both read a promised surface behind its promise, so each waits on the GPU for
// the promised job. That job's write must not wait for them (a cycle the give-up alone would break), while it still
// waits for a reader of what the surface held before; a writer after the promised one waits for both.
TEST_F(Backing, ThePromisedJobDoesNotWaitForReadersOrderedBehindItsPromise)
{
	auto *b = promised();
	ASSERT_EQ(publish(b, kRead, token(7, 1, SWITCH_GPU_ENGINE_NVDEC)), 0);
	readBehindPromises(b, token(9, 2));
	readBehindPromises(b, token(160, 3, SWITCH_GPU_ENGINE_VIC));
	const auto promisedWrite = waitedFor(b, token(3, 4));
	EXPECT_TRUE(has(promisedWrite, SWITCH_GPU_ENGINE_NVDEC));
	EXPECT_FALSE(has(promisedWrite, SWITCH_GPU_ENGINE_VIC));
	EXPECT_FALSE(has(promisedWrite, SWITCH_GPU_ENGINE_GM20B))
		<< "the other connection's reader";
	switch_gpu_backing_promise_drop(b, kOffset);
	const auto next = waitedFor(b, token(5, 1));
	EXPECT_TRUE(has(next, SWITCH_GPU_ENGINE_VIC));
	EXPECT_TRUE(has(next, SWITCH_GPU_ENGINE_GM20B));
	switch_gpu_backing_release(b);
}

// Two promised jobs write one surface: a reader behind both is waited for by neither.
TEST_F(Backing, AReaderBehindTwoPromisesIsWaitedForByNeitherPromisedJob)
{
	auto *b = promised();
	fence[(kOffset + 4) / 4] = 0;
	const switch_gpu_promise_ref second = {tgm_fence_word(kOffset + 4), kOffset + 4, 1};
	ASSERT_EQ(switch_gpu_backing_promise_add(b, &second), 0);
	readBehindPromises(b, token(160, 3, SWITCH_GPU_ENGINE_VIC));
	EXPECT_FALSE(has(waitedFor(b, token(3, 4)), SWITCH_GPU_ENGINE_VIC));
	switch_gpu_backing_promise_drop(b, kOffset);
	EXPECT_FALSE(has(waitedFor(b, token(4, 1)), SWITCH_GPU_ENGINE_VIC));
	switch_gpu_backing_promise_drop(b, kOffset + 4);
	EXPECT_TRUE(has(waitedFor(b, token(5, 1)), SWITCH_GPU_ENGINE_VIC));
	switch_gpu_backing_release(b);
}

// A connection's own reads do not order behind its own promises (its later job keeps them): its writer waits for them.
TEST_F(Backing, AReadOutsideThePromisedRangeIsAnOrdinaryRead)
{
	auto *b = promised();
	readBehindPromises(b, token(9, 2), kOffset, kOffset + 4);
	EXPECT_TRUE(has(waitedFor(b, token(3, 4)), SWITCH_GPU_ENGINE_GM20B));
	switch_gpu_backing_promise_drop(b, kOffset);
	switch_gpu_backing_release(b);
}

// A promise given up on (the give-up's raise, a kill, a close) is a failed producer: the present that waited for it
// finishes TEGRA_SCANOUT_SOURCE_LOST, and the display drops the frame, the panel keeping the one before it. The word
// the release raised does not make it kept.
TEST_F(Backing, AnAbandonedPromiseFailsThePresentThatWaitedForIt)
{
	auto *b = promised(3);
	ASSERT_EQ(switch_gpu_promise_engine(), 0);
	switch_gpu_backing_token tokens[1] = {};
	ASSERT_EQ(switch_gpu_backing_promise_tokens(b, 0, 0, tokens, 1), 1);
	switch_gpu_present_tokens present = {};
	ASSERT_EQ(switch_gpu_present_tokens_prepare(&present, tokens, 1), 0);
	EXPECT_EQ(switch_gpu_backing_token_status(&tokens[0]), 0);
	abandoned = uint64_t(kOffset) << 32 | 3;
	fence[kOffset / 4] = 3;
	EXPECT_EQ(switch_gpu_backing_token_status(&tokens[0]), -5);
	EXPECT_EQ(switch_gpu_present_tokens_finish_ready(&present, nullptr), -5);
	abandoned = 0;
	EXPECT_EQ(switch_gpu_backing_token_status(&tokens[0]), 1)
		<< "a cancel's raise: nothing was promised after all";
	switch_gpu_backing_promise_drop(b, kOffset);
	switch_gpu_backing_release(b);
}

// The give-up raises a word a parked reader waits for without dropping the surface's ref: nothing waits for it after.
TEST_F(Backing, APromiseWhoseWordWasRaisedHoldsNothingOff)
{
	auto *b = promised();
	fence[kOffset / 4] = 1;
	EXPECT_EQ(switch_gpu_backing_promised(b), 0);
	switch_gpu_backing_token producer = {}, tokens[4] = {};
	switch_gpu_cpu_lease *lease = nullptr;
	EXPECT_EQ(switch_gpu_backing_snapshot_read_promises(b, &producer, &lease, tokens, 4), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead | avoid), 0);
	switch_gpu_backing_promise_drop(b, kOffset);
	switch_gpu_backing_release(b);
}

// A connection's own promises are never its jobs' to wait for; the engine's token is the word reaching the value.
TEST_F(Backing, AConnectionsOwnPromisesAreNotItsTokensAndTheTokenIsTheWord)
{
	auto *b = promised(5);
	switch_gpu_backing_token tokens[4] = {};
	EXPECT_EQ(switch_gpu_backing_promise_tokens(b, kOffset, kOffset + 128, tokens, 4), 0);
	ASSERT_EQ(switch_gpu_backing_promise_tokens(b, 0, 0, tokens, 4), 1);
	ASSERT_EQ(switch_gpu_promise_engine(), 0);
	EXPECT_EQ(switch_gpu_backing_token_status(&tokens[0]), 0);
	EXPECT_EQ(switch_gpu_backing_token_wait(&tokens[0]), -95);
	ASSERT_EQ(switch_gpu_backing_token_retain(&tokens[0]), 0);
	EXPECT_EQ(holds, 1);
	switch_gpu_backing_token_release(&tokens[0]);
	EXPECT_EQ(holds, 0);
	fence[kOffset / 4] = 5;
	EXPECT_EQ(switch_gpu_backing_token_status(&tokens[0]), 1);
	switch_gpu_backing_promise_drop(b, kOffset);
	switch_gpu_backing_release(b);
}
}  // namespace
