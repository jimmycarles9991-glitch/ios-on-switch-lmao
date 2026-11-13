// SPDX-License-Identifier: MIT
#include "switch_gpu_backing_fixture.h"
#include "drivers/TegraGM20B/SwitchGpuAdmission.h"

namespace
{
constexpr uint32_t kRead = SWITCH_GPU_BACKING_READ, kWrite = SWITCH_GPU_BACKING_WRITE;
constexpr uint32_t avoid = SWITCH_GPU_BACKING_CPU_AVOID_SYNC;

TEST_F(Backing, AdmissionReadKeepsExistingCpuReadersAndBlocksNewWriter)
{
	auto *b = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), 0);
	SwitchGpuAdmission admission;
	switch_gpu_backing_use use{b, kRead, 0};
	ASSERT_EQ(admission.begin(&use, 1, nullptr, 0, nullptr, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kWrite | avoid), -16);
	ASSERT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead | avoid), 0);
	admission.end();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kWrite | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kWrite | avoid), 0);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionWriterBlocksCpuButLeavesGpuDependencyChainingAvailable)
{
	auto *b = backing();
	SwitchGpuAdmission admission;
	switch_gpu_backing_use use{b, kWrite, 0};
	ASSERT_EQ(admission.begin(&use, 1, nullptr, 0, nullptr, 0), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), -16);
	EXPECT_EQ(publish(b, kWrite, token(1)), 0);
	EXPECT_EQ(publish(b, kRead, token(2)), 0);
	admission.end();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), -16);
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead), 0);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionFailureDoesNotReserveEarlierResources)
{
	auto *a = backing(), *b = backing();
	ASSERT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), 0);
	SwitchGpuAdmission admission;
	switch_gpu_backing_use uses[]{{a, kWrite, 0}, {b, kWrite, 0}};
	EXPECT_EQ(admission.begin(uses, 2, nullptr, 0, nullptr, 0), -16);
	EXPECT_FALSE(admission.active());
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(a, 0, 4096, kWrite | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(a, 0, 4096, kWrite | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead | avoid), 0);
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionTransfersPreparedConversionWithoutAnOwnershipGap)
{
	auto *b = backing();
	switch_gpu_cpu_lease *lease = nullptr;
	ASSERT_EQ(switch_gpu_backing_conversion_reserve(b, kWrite, true, &lease), 0);
	SwitchGpuAdmission admission;
	switch_gpu_backing_use use{b, kWrite, 0};
	ASSERT_EQ(admission.begin(&use, 1, nullptr, 0, &lease, 1), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release_owned(&lease, 0), 0);
	EXPECT_EQ(switch_gpu_backing_conversion_ready(b, kRead), 0);
	EXPECT_EQ(switch_gpu_backing_admission_ready(b, kWrite), 1);
	EXPECT_EQ(switch_gpu_backing_conversion_reserve(b, kRead, true, &lease), -16);
	EXPECT_EQ(publish(b, kWrite, token(1)), 0);
	admission.end();
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionAliasUnionAndContinuationCoverageAreExact)
{
	auto *a = backing(), *b = backing();
	SwitchGpuAdmission admission;
	switch_gpu_backing_use uses[]{{a, kRead, 0}, {a, kWrite, 0}};
	ASSERT_EQ(admission.begin(uses, 2, nullptr, 0, nullptr, 0), 0);
	EXPECT_TRUE(admission.covers(uses, 2));
	switch_gpu_backing_use other{b, kRead, 0};
	EXPECT_FALSE(admission.covers(&other, 1));
	admission.end();
	ASSERT_EQ(admission.begin(uses, 1, nullptr, 0, nullptr, 0), 0);
	EXPECT_FALSE(admission.covers(uses + 1, 1));
	admission.end();
	switch_gpu_backing_release(a);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionRetainsIndependentQueueCountsUntilBothEnd)
{
	auto *b = backing();
	SwitchGpuAdmission first, second;
	switch_gpu_backing_use use{b, kWrite, 0};
	ASSERT_EQ(first.begin(&use, 1, nullptr, 0, nullptr, 0), 0);
	ASSERT_EQ(second.begin(&use, 1, nullptr, 0, nullptr, 0), 0);
	first.end();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), -16);
	second.end();
	EXPECT_EQ(switch_gpu_backing_cpu_acquire(b, 0, 4096, kRead | avoid), 0);
	EXPECT_EQ(switch_gpu_backing_cpu_release(b, 0, 4096, kRead | avoid), 0);
	switch_gpu_backing_release(b);
}

TEST_F(Backing, AdmissionStaleGuardRefusesBeforeReservationAndFailedContentDoesNot)
{
	auto *b = backing();
	SwitchGpuAdmission admission;
	switch_gpu_backing_use use{b, kWrite, 0};
	alignas(8) volatile uint64_t generation = 3;
	switch_gpu_backing_guard guard{b, &generation, 2};
	EXPECT_EQ(admission.begin(&use, 1, &guard, 1, nullptr, 0), SWITCH_GPU_BACKING_STALE);
	EXPECT_FALSE(admission.active());
	ASSERT_EQ(publish(b, kWrite, token(1)), 0);
	const auto failed = token(1);
	timelines[key(&failed)].error = -5;
	EXPECT_EQ(admission.begin(&use, 1, nullptr, 0, nullptr, 0), 0);
	EXPECT_TRUE(admission.active());
	admission.end();
	switch_gpu_backing_release(b);
}
}

TEST_F(Backing, AdmissionOrdersLargeAliasedUseSetAndRejectsGrowthWhileHeld)
{
	std::vector<switch_gpu_backing *> allocations;
	for (unsigned i = 0; i < 256; ++i)
		allocations.push_back(backing());
	std::vector<switch_gpu_backing_use> uses;
	for (unsigned i = 0; i < 1024; ++i)
		uses.push_back({allocations[(1023 - i) % 256], i & 1 ? kRead : kWrite, 0});
	SwitchGpuAdmission admission;
	ASSERT_EQ(admission.begin(uses.data(), uint32_t(uses.size()), nullptr, 0, nullptr, 0), 0);
	EXPECT_TRUE(admission.covers(uses.data(), uint32_t(uses.size())));
	std::array<uint64_t, 2> pages;
	switch_gpu_backing_info info = {};
	switch_gpu_backing_describe(allocations.front(), &info);
	pages[0] = info.pages[0];
	pages[1] = pages[0] + 4096;
	EXPECT_EQ(switch_gpu_backing_register(pages.data(), 2, 0, nullptr, nullptr), nullptr);
	admission.end();
	for (auto *allocation : allocations)
		switch_gpu_backing_release(allocation);
}
