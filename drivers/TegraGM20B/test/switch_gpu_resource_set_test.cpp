// SPDX-License-Identifier: MIT
// SwitchGpuResourceSet.cpp over the backing registry: registration, each cause that ends a generation, the set's jobs
// standing for a member's users when a CPU lease or something outside the set reaches it, and a member that leaves while
// a submission is inside.
#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuBackingInternal.h"
#include "drivers/TegraGM20B/SwitchGpuResourceSet.h"
#include "drivers/TegraGM20B/SwitchGpuJobStats.h"
#include <string>

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;

struct Set {
	volatile uint32_t mirror = 0;
	switch_gpu_resource_set *set = nullptr;

	Set()
	{
		EXPECT_EQ(switch_gpu_set_create(&mirror, &set), 0);
	}

	~Set()
	{
		switch_gpu_set_destroy(set);
	}

	uint32_t attach(std::vector<switch_gpu_set_member> members, int expected = 0)
	{
		uint32_t generation = 0;
		EXPECT_EQ(switch_gpu_set_attach(set, members.data(), uint32_t(members.size()),
						&generation, nullptr),
			  expected);
		return generation;
	}
};

// What an outside GM20B job on queue 9 would wait for before touching `backing`
std::vector<switch_gpu_backing_token> outside_waits(switch_gpu_backing *backing, uint32_t access)
{
	switch_gpu_backing_use use = {backing, access, 0};
	const auto completion = token(9, 1);
	switch_gpu_backing_transaction transaction = {};
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &completion, &transaction), 0);
	switch_gpu_backing_token waits[8] = {};
	uint32_t count = 0;
	EXPECT_EQ(switch_gpu_backing_dependencies(&transaction, waits, 8, &count), 0);
	switch_gpu_backing_abort(&transaction);
	return {waits, waits + count};
}

uint64_t first_page(switch_gpu_backing *backing)
{
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(backing, &info);
	return info.pages[0];
}

class ResourceSet : public Backing
{
protected:
	void SetUp() override
	{
		Backing::SetUp();
		static bool visible;
		if (!visible) {
			const switch_gpu_backing_visibility gm = {external_status, external_wait};
			ASSERT_EQ(switch_gpu_backing_register_visibility(SWITCH_GPU_ENGINE_GM20B,
									 &gm),
				  0);
			visible = true;
		}
		external_completed.clear();
	}
};
}  // namespace

TEST_F(ResourceSet, RegistrationLinksMembersAndPublishesTheGeneration)
{
	auto *a = backing(), *b = backing();
	{
		Set set;
		const uint32_t generation =
			set.attach({{a, kRead, 0}, {b, kWrite, 0}, {a, kWrite, 0}});
		EXPECT_EQ(generation, 1u);
		EXPECT_EQ(set.mirror, generation);
		EXPECT_EQ(switch_gpu_set_generation(set.set), generation);
		EXPECT_EQ(switch_gpu_backing_set(a), set.set);
		EXPECT_EQ(switch_gpu_backing_set(b), set.set);
		EXPECT_EQ(switch_gpu_set_enter(set.set, generation), 1);
		switch_gpu_set_leave(set.set);
		EXPECT_EQ(switch_gpu_set_enter(set.set, generation + 1), 0);
	}
	EXPECT_EQ(switch_gpu_backing_set(a), nullptr);
	EXPECT_EQ(publish(a, kWrite, token(2, 1)), 0);
	EXPECT_EQ(outside_waits(a, kRead).size(), 1u);
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
}

TEST_F(ResourceSet, AJobNamingTheSetLeavesItsMembersRecordsAlone)
{
	auto *member = backing();
	Set set;
	set.attach({{member, kWrite, 0}});
	const auto job = token(2, 7);
	switch_gpu_set_note_job(set.set, 1, &job);
	EXPECT_TRUE(outside_waits(member, kWrite).empty());
	switch_gpu_backing_release(member);
}

TEST_F(ResourceSet, OnlyPrivateBackingsJoin)
{
	auto *mine = backing(), *other = backing();
	const uint64_t page = next_page += 4096;
	auto *surface = switch_gpu_backing_register(&page, 1, 77, nullptr, nullptr);
	auto *found = switch_gpu_backing_find_pa(first_page(other));
	ASSERT_EQ(found, other);
	{
		Set set, second;
		set.attach({{surface, kRead, 0}}, -22);
		set.attach({{other, kRead, 0}}, -22);
		set.attach({{mine, 0, 0}}, -22);
		EXPECT_EQ(set.attach({{mine, kWrite, 0}}), 1u);
		second.attach({{mine, kRead, 0}}, -16);
		EXPECT_EQ(switch_gpu_backing_set(mine), set.set);
	}
	for (auto *b : {mine, other, other, surface})
		switch_gpu_backing_release(b);
}

TEST_F(ResourceSet, ReplacingEndsTheGenerationAndReleasesWhatLeft)
{
	auto *a = backing(), *b = backing();
	Set set;
	const uint32_t first = set.attach({{a, kWrite, 0}, {b, kRead, 0}});
	const uint32_t second = set.attach({{b, kWrite, 0}});
	EXPECT_GT(second, first);
	EXPECT_EQ(set.mirror, second);
	EXPECT_EQ(switch_gpu_set_enter(set.set, first), 0);
	EXPECT_EQ(switch_gpu_backing_set(a), nullptr);
	EXPECT_EQ(switch_gpu_backing_set(b), set.set);
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
}

TEST_F(ResourceSet, AFreedMemberKeepsTheSetsPendingJobsAsItsUsers)
{
	auto *member = backing();
	Set set;
	const uint32_t generation = set.attach({{member, kWrite, 0}});
	const auto job = token(3, 5);
	switch_gpu_set_note_job(set.set, 0, &job);
	switch_gpu_backing_member_freed(member);
	EXPECT_GT(switch_gpu_set_generation(set.set), generation);
	EXPECT_EQ(switch_gpu_backing_set(member), nullptr);
	const auto waits = outside_waits(member, kRead);
	ASSERT_EQ(waits.size(), 1u);
	EXPECT_EQ(waits[0].queue, 3u);
	EXPECT_EQ(waits[0].value, 5u);
	switch_gpu_backing_release(member);
}

TEST_F(ResourceSet, ALayoutChangeEndsTheGenerationAndWaitsForTheSetsJobs)
{
	auto *member = backing();
	Set set;
	const uint32_t generation = set.attach({{member, kRead, 0}});
	const auto job = token(4, 2);
	switch_gpu_set_note_job(set.set, 1, &job);
	switch_gpu_backing_layout layout = {};
	layout.plane_count = 1;
	layout.planes[0] = {0, 4096, 16, 16, 64, 4, SWITCH_GPU_BACKING_PITCH, 0};
	EXPECT_EQ(switch_gpu_backing_set_layout(member, &layout), -16);
	EXPECT_GT(switch_gpu_set_generation(set.set), generation);
	complete(job);
	EXPECT_EQ(switch_gpu_backing_set_layout(member, &layout), 0);
	switch_gpu_backing_release(member);
}

TEST_F(ResourceSet, AMemberReachedFromOutsideOrdersItsNewUserAfterTheSetsJobs)
{
	auto *member = backing();
	Set set;
	const uint32_t generation = set.attach({{member, kRead, 0}});
	const auto job = token(5, 8);
	switch_gpu_set_note_job(set.set, 2, &job);
	auto *found = switch_gpu_backing_find_pa(first_page(member));
	ASSERT_EQ(found, member);
	EXPECT_GT(switch_gpu_set_generation(set.set), generation);
	EXPECT_EQ(switch_gpu_backing_set(member), nullptr);
	EXPECT_TRUE(outside_waits(member, kRead).empty());
	const auto waits = outside_waits(member, kWrite);
	ASSERT_EQ(waits.size(), 1u);
	EXPECT_EQ(waits[0].queue, 5u);
	uint32_t next = 0;
	EXPECT_EQ(switch_gpu_set_attach(set.set, (switch_gpu_set_member[]){{member, kRead, 0}}, 1,
					&next, nullptr),
		  -22);
	switch_gpu_backing_release(member);
	switch_gpu_backing_release(member);
}

TEST_F(ResourceSet, AKillEndsTheGenerationOnlyOfSetsTheChannelUsed)
{
	auto *member = backing();
	Set set;
	const uint32_t generation = set.attach({{member, kWrite, 0}});
	switch_gpu_set_killed(set.set, 3);
	EXPECT_EQ(switch_gpu_set_generation(set.set), generation);
	const auto job = token(6, 1);
	switch_gpu_set_note_job(set.set, 3, &job);
	EXPECT_EQ(switch_gpu_set_used(set.set), 1u << 3);
	EXPECT_EQ(switch_gpu_set_seqno(set.set, 3), 1u);
	switch_gpu_set_killed(set.set, 3);
	EXPECT_GT(switch_gpu_set_generation(set.set), generation);
	EXPECT_EQ(switch_gpu_set_used(set.set), 0u);
	switch_gpu_backing_release(member);
}

TEST_F(ResourceSet, ACpuLeaseWaitsOnlyForSetJobsThatConflict)
{
	auto *readOnly = backing(), *written = backing();
	Set set;
	set.attach({{readOnly, kRead, 0}, {written, kWrite, 0}});
	const auto job = token(7, 3);
	switch_gpu_set_note_job(set.set, 0, &job);
	constexpr uint32_t avoid = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
	switch_gpu_backing_member_cpu(readOnly);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(readOnly, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(readOnly, 0, 4096, kRead | avoid), 0);
	switch_gpu_backing_member_cpu(written);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(written, 0, 4096, kRead | avoid), -16);
	switch_gpu_backing_member_cpu(readOnly);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(readOnly, 0, 4096, kWrite | avoid), -16);
	complete(job);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(written, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(written, 0, 4096, kRead | avoid), 0);
	switch_gpu_backing_release(readOnly);
	switch_gpu_backing_release(written);
}

TEST_F(ResourceSet, HostWritesAreTakenOnce)
{
	Set set;
	EXPECT_EQ(switch_gpu_set_take_host_written(set.set), 0);
	switch_gpu_set_host_written(set.set);
	EXPECT_EQ(switch_gpu_set_take_host_written(set.set), 1);
	EXPECT_EQ(switch_gpu_set_take_host_written(set.set), 0);
}

// A member reached from outside while a submission is inside the set: its users must include the job that submission
// records, so the outside user never runs beside it.
TEST_F(ResourceSet, AMemberLeavingWaitsForTheSubmissionInside)
{
	auto *member = backing();
	Set set;
	const uint32_t generation = set.attach({{member, kWrite, 0}});
	ASSERT_EQ(switch_gpu_set_enter(set.set, generation), 1);
	std::promise<void> reaching;
	auto outside = std::async(std::launch::async, [&] {
		reaching.set_value();
		return switch_gpu_backing_find_pa(first_page(member));
	});
	reaching.get_future().wait();
	EXPECT_EQ(outside.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
	const auto job = token(8, 4);
	switch_gpu_set_note_job(set.set, 4, &job);
	switch_gpu_set_leave(set.set);
	ASSERT_EQ(outside.get(), member);
	const auto waits = outside_waits(member, kRead);
	ASSERT_EQ(waits.size(), 1u);
	EXPECT_EQ(waits[0].queue, 8u);
	EXPECT_EQ(waits[0].value, 4u);
	switch_gpu_backing_release(member);
	switch_gpu_backing_release(member);
}

// A job follows each other channel's latest job on the set, only while that job is unfinished and its channel lives.
TEST_F(ResourceSet, AJobFollowsTheOtherChannelsLatestSetJob)
{
	auto *member = backing();
	Set set;
	set.attach({{member, kWrite, 0}});
	const auto copy = token(1, 5), render = token(2, 9);
	switch_gpu_set_note_job(set.set, 3, &copy);
	switch_gpu_set_note_job(set.set, 0, &render);
	uint32_t completed[SWITCH_GPU_SET_SLOTS] = {};
	completed[3] = 4;
	completed[0] = 9;
	switch_gpu_set_wait waits[SWITCH_GPU_SET_SLOTS];
	ASSERT_EQ(switch_gpu_set_waits(set.set, 0, 0xff, completed, waits), 1u);
	EXPECT_EQ(waits[0].slot, 3u);
	EXPECT_EQ(waits[0].seqno, 5u);
	completed[0] = 8;
	ASSERT_EQ(switch_gpu_set_waits(set.set, 3, 0xff, completed, waits), 1u);
	EXPECT_EQ(waits[0].slot, 0u);
	EXPECT_EQ(waits[0].seqno, 9u);
	EXPECT_EQ(switch_gpu_set_waits(set.set, 0, 0xff & ~(1u << 3), completed, waits), 0u);
	completed[3] = 5;
	EXPECT_EQ(switch_gpu_set_waits(set.set, 0, 0xff, completed, waits), 0u);
	const auto later = token(1, 0x80000002);
	switch_gpu_set_note_job(set.set, 3, &later);
	completed[3] = 0x7ffffff0;
	EXPECT_EQ(switch_gpu_set_waits(set.set, 0, 0xff, completed, waits), 1u);
	switch_gpu_backing_release(member);
}

// A channel killed while its set job runs: its partner's acquire passes once the kill releases the fence, the set's
// generation ends at the kill, and the next generation's jobs no longer follow the dead channel.
TEST_F(ResourceSet, AKillMidSetEndsTheGenerationAndDropsTheDeadChannel)
{
	auto *member = backing();
	Set set;
	const uint32_t generation = set.attach({{member, kWrite, 0}});
	const auto doomed = token(1, 7);
	switch_gpu_set_note_job(set.set, 2, &doomed);
	ASSERT_EQ(switch_gpu_set_enter(set.set, generation), 1);
	uint32_t completed[SWITCH_GPU_SET_SLOTS] = {};
	switch_gpu_set_wait waits[SWITCH_GPU_SET_SLOTS];
	EXPECT_EQ(switch_gpu_set_waits(set.set, 5, 0xff, completed, waits), 1u);
	switch_gpu_set_leave(set.set);
	switch_gpu_set_killed(set.set, 2);
	EXPECT_EQ(switch_gpu_set_enter(set.set, generation), 0);
	const uint32_t next = set.attach({{member, kWrite, 0}});
	ASSERT_EQ(switch_gpu_set_enter(set.set, next), 1);
	EXPECT_EQ(switch_gpu_set_waits(set.set, 5, 0xff, completed, waits), 0u);
	switch_gpu_set_leave(set.set);
	switch_gpu_backing_release(member);
}

// A shared use is never a member, so a CPU lock of an IOSurface keeps excluding and ordering GPU work exactly.
TEST_F(ResourceSet, ACpuLockOnASharedUseKeepsItsExclusion)
{
	const uint64_t page = next_page += 4096;
	auto *surface = switch_gpu_backing_register(&page, 1, 41, nullptr, nullptr);
	auto *member = backing();
	{
		Set set;
		set.attach({{surface, kWrite, 0}}, -22);
		set.attach({{member, kWrite, 0}});
		EXPECT_EQ(switch_gpu_backing_set(surface), nullptr);
		EXPECT_EQ(switch_gpu_backing_set(member), set.set);
	}
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(surface, 0, 4096, kWrite), 0);
	switch_gpu_backing_use use = {surface, kWrite, 0};
	const auto job = token(3, 2);
	switch_gpu_backing_transaction transaction = {};
	EXPECT_EQ(switch_gpu_backing_begin(&use, 1, &job, &transaction), -16);
	use.access = kWrite | SWITCH_GPU_BACKING_NO_CPU_EXCLUSION;
	ASSERT_EQ(switch_gpu_backing_begin(&use, 1, &job, &transaction), 0);
	switch_gpu_backing_commit(&transaction);
	ASSERT_EQ(switch_gpu_backing_cpu_release(surface, 0, 4096, kWrite), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(surface, 0, 4096,
						 kRead | SWITCH_GPU_BACKING_CPU_AVOID_SYNC),
		  -16);
	complete(job);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(surface, 0, 4096,
						 kRead | SWITCH_GPU_BACKING_CPU_AVOID_SYNC),
		  0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(surface, 0, 4096,
						 kRead | SWITCH_GPU_BACKING_CPU_AVOID_SYNC),
		  0);
	switch_gpu_backing_release(surface);
	switch_gpu_backing_release(member);
}

// An Internal set job leaves its writes in the GPU L2 when it completes: a CPU reader of a member it may have written
// still waits for them to reach DRAM.
// A CPU lease or CpuComplete takes a member out for good: its generation ends, and making the set again refuses it by name
TEST_F(ResourceSet, ACpuAccessTakesAMemberOutForGood)
{
	auto *member = backing(), *other = backing();
	Set set;
	const uint32_t generation = set.attach({{member, kWrite, 0}, {other, kRead, 0}});
	const auto job = token(3, 2);
	switch_gpu_set_note_job(set.set, 0, &job);
	switch_gpu_backing_member_cpu(member);
	EXPECT_GT(switch_gpu_set_generation(set.set), generation);
	EXPECT_EQ(switch_gpu_backing_set(member), nullptr);
	EXPECT_EQ(switch_gpu_backing_set(other), set.set);
	EXPECT_EQ(outside_waits(member, kRead).size(), 1u);
	uint32_t next = 0;
	switch_gpu_backing *refused = nullptr;
	const switch_gpu_set_member again[] = {{member, kWrite, 0}, {other, kRead, 0}};
	EXPECT_EQ(switch_gpu_set_attach(set.set, again, 2, &next, &refused), -22);
	EXPECT_EQ(refused, member);
	EXPECT_EQ(switch_gpu_backing_set(other), set.set) << "a refused attach changed the set";
	switch_gpu_backing_release(member);
	switch_gpu_backing_release(other);
}

// A member a replace or a release lets go keeps the set's pending jobs as its users
TEST_F(ResourceSet, AMemberLetGoKeepsTheSetsPendingJobs)
{
	auto *dropped = backing(), *kept = backing();
	{
		Set set;
		set.attach({{dropped, kWrite, 0}, {kept, kWrite, 0}});
		const auto job = token(5, 4);
		switch_gpu_set_note_job(set.set, 1, &job);
		set.attach({{kept, kWrite, 0}});
		EXPECT_EQ(switch_gpu_backing_set(dropped), nullptr);
		EXPECT_EQ(outside_waits(dropped, kRead).size(), 1u)
			<< "the replace forgot the set's job";
	}
	EXPECT_EQ(switch_gpu_backing_set(kept), nullptr);
	EXPECT_EQ(outside_waits(kept, kRead).size(), 1u) << "the release forgot the set's job";
	switch_gpu_backing_release(dropped);
	switch_gpu_backing_release(kept);
}

// A CPU-tracked import may join; leaving, its writers cover it whole, so CpuComplete maintains what the set's jobs wrote
TEST_F(ResourceSet, ACpuTrackedMemberLeavesWithWholeWriterRanges)
{
	auto *member = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_track(member), 0);
	Set set;
	set.attach({{member, kWrite, 0}});
	const auto job = token(6, 9);
	switch_gpu_set_note_job(set.set, 0, &job);
	switch_gpu_backing_member_cpu(member);
	ASSERT_EQ(member->writer_count, 1u);
	EXPECT_EQ(member->writers[0].begin, 0u);
	EXPECT_EQ(member->writers[0].end, uint64_t(member->pages->count) << 12);
	switch_gpu_backing_release(member);
}

TEST_F(ResourceSet, AFinishedSetJobStillOwesAMembersReaderItsVisibility)
{
	auto *member = backing();
	Set set;
	set.attach({{member, kWrite, 0}});
	const auto job = token(4, 6);
	switch_gpu_set_note_job(set.set, 0, &job);
	complete(job);
	external_completed[{job.engine, job.queue, job.reset_generation}] = 5;
	constexpr uint32_t avoid = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;
	switch_gpu_backing_member_cpu(member);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(member, 0, 4096, kRead | avoid), -16);
	external_completed[{job.engine, job.queue, job.reset_generation}] = 6;
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(member, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(member, 0, 4096, kRead | avoid), 0);
	switch_gpu_backing_release(member);
}

// A freed member that cannot take the set's jobs as its users (no memory for their records) stays the set's, listed and
// held, until the set lets it go: its set pointer never outlives the set it names.
TEST_F(ResourceSet, AFreedMemberThatCannotAdoptStaysUntilTheSetLetsItGo)
{
	auto *member = backing();
	{
		Set set;
		const uint32_t generation = set.attach({{member, kWrite, 0}});
		const auto job = token(3, 6);
		switch_gpu_set_note_job(set.set, 0, &job);
		failAllocation() = true;
		switch_gpu_backing_member_freed(member);
		failAllocation() = false;
		EXPECT_GT(switch_gpu_set_generation(set.set), generation);
		EXPECT_EQ(switch_gpu_backing_set(member), set.set);
	}
	EXPECT_EQ(switch_gpu_backing_set(member), nullptr);
	switch_gpu_backing_member_cpu(member);
	switch_gpu_backing_release(member);
}

// A freed member leaves the set's list too: its pages go back once its users are done, not at the next registration.
TEST_F(ResourceSet, AFreedMembersMemoryIsNotHeldByTheSet)
{
	static bool released;
	released = false;
	const uint64_t page = next_page += 4096;
	auto *member = switch_gpu_backing_register(&page, 1, 0, &released, [](void *flag) {
		*static_cast<bool *>(flag) = true;
	});
	Set set;
	set.attach({{member, kWrite, 0}});
	switch_gpu_backing_member_freed(member);
	switch_gpu_backing_release(member);
	do {
		retirement_scheduled = false;
		switch_gpu_backing_retire();
	} while (retirement_scheduled);
	EXPECT_TRUE(released);
}

// One writer per slot, readers anywhere: a reader never sees half of one job and half of the next.
TEST_F(ResourceSet, ASlotsJobIsReadWhole)
{
	Set set;
	std::atomic<bool> done{false};
	std::atomic<uint32_t> torn{0}, reads{0};
	std::vector<std::thread> readers;
	for (int r = 0; r < 3; ++r)
		readers.emplace_back([&] {
			switch_gpu_backing_token jobs[SWITCH_GPU_SET_SLOTS];
			while (!done.load(std::memory_order_relaxed))
				for (uint32_t i = 0, n = switch_gpu_set_jobs(set.set, jobs); i < n;
				     ++i) {
					reads.fetch_add(1, std::memory_order_relaxed);
					if (jobs[i].value != jobs[i].queue ||
					    jobs[i].reset_generation != jobs[i].queue)
						torn.fetch_add(1, std::memory_order_relaxed);
				}
		});
	for (uint32_t k = 1; k < 3000000 && !torn.load(std::memory_order_relaxed); ++k) {
		const switch_gpu_backing_token job = {k, k, SWITCH_GPU_ENGINE_GM20B, k};
		switch_gpu_set_note_job(set.set, 1, &job);
	}
	done = true;
	for (auto &reader : readers)
		reader.join();
	EXPECT_GT(reads.load(), 0u);
	EXPECT_EQ(torn.load(), 0u);
}

TEST_F(ResourceSet, TheJobsLineCountsEndedGenerationsByCause)
{
	char line[512];
	switch_gpu_job_window(line, sizeof(line));
	auto *member = backing();
	{
		Set set;
		set.attach({{member, kWrite, 0}});
		set.attach({{member, kRead, 0}});
		switch_gpu_set_note_job(set.set, 0,
					(const switch_gpu_backing_token[]){token(2, 1)});
		switch_gpu_set_killed(set.set, 0);
		switch_gpu_job_stale();
		const uint64_t ns[4] = {1000, 2000, 30000, 4000};
		switch_gpu_job_submitted(ns, 1, 3, 1);
	}
	switch_gpu_job_window(line, sizeof(line));
	EXPECT_NE(std::string(line).find(
			  "SubmitJob 1 x mean 37 us (resolve 1, lock 2, exec 30, commit 4)"),
		  std::string::npos)
		<< line;
	EXPECT_NE(std::string(line).find("set 1 hit 1 stale, shared 3.0 per job max 3, siblings 1"),
		  std::string::npos)
		<< line;
	EXPECT_NE(std::string(line).find("free 0 layout 0 shared 0 kill 1 replace 1"),
		  std::string::npos)
		<< line;
	switch_gpu_job_window(line, sizeof(line));
	EXPECT_EQ(line[0], 0);
	switch_gpu_backing_release(member);
}
