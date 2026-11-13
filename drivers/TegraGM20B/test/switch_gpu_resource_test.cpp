// SPDX-License-Identifier: MIT
#include "switch_gpu_ready_stubs.h"
#include "drivers/TegraGM20B/SwitchGpuResourceClient.h"
#include "drivers/TegraGM20B/SwitchGpuReady.h"
#include <array>
#include <gtest/gtest.h>

extern thread_call_t switch_event_call;

struct switch_gpu_backing {
	unsigned refs = 1, readers = 0, writers = 0;
};

extern "C" void switch_gpu_backing_retain(switch_gpu_backing *b)
{
	++b->refs;
}

extern "C" void switch_gpu_backing_release(switch_gpu_backing *b)
{
	--b->refs;
}

extern "C" int switch_gpu_backing_admission_ready(switch_gpu_backing *b, uint32_t access)
{
	return !b->writers && (!(access & SWITCH_GPU_BACKING_WRITE) || !b->readers);
}

static int nativeReady, nativeChecks;

extern "C" int switch_gpu_surface_gpu_admission(void *, switch_gpu_backing *, uint32_t)
{
	++nativeChecks;
	return nativeReady;
}

namespace
{
struct Notice {
	unsigned calls = 0;
	uint64_t cookie = 0;
	int result = 99;
};

void finished(void *context, uint64_t cookie, int result)
{
	auto &notice = *static_cast<Notice *>(context);
	++notice.calls;
	notice.cookie = cookie;
	notice.result = result;
}

void runReady()
{
	unsigned limit = 0;
	while (switch_event_call && switch_event_call->pending.exchange(false)) {
		ASSERT_LT(++limit, 1000u);
		switch_event_call->fn(switch_event_call->arg, nullptr);
	}
}

class Resource : public testing::Test
{
protected:
	SwitchGpuResourceClient client;
	switch_gpu_backing backing;
	Notice notice;

	void SetUp() override
	{
		ASSERT_TRUE(client.initialize());
		nativeReady = nativeChecks = 0;
	}

	void TearDown() override
	{
		client.destroy();
		runReady();
		EXPECT_EQ(backing.refs, 1u);
	}
};

TEST_F(Resource, ReadAdmissionDoesNotWaitForCompatibleGeometryReader)
{
	backing.readers = 1;
	SwitchGpuResourceUse use{&backing, nullptr, nullptr, 0, SWITCH_GPU_BACKING_READ};
	ASSERT_EQ(client.observe(&use, 1, 9, finished, &notice), 0);
	runReady();
	EXPECT_EQ(notice.calls, 1u);
	EXPECT_EQ(notice.result, 0);
}

TEST_F(Resource, WriteWakeFollowsActualOwnerReleaseWithoutHoldingBo)
{
	backing.readers = 1;
	SwitchGpuResourceUse use{&backing, nullptr, nullptr, 0, SWITCH_GPU_BACKING_WRITE};
	ASSERT_EQ(client.observe(&use, 1, 10, finished, &notice), 0);
	EXPECT_EQ(backing.refs, 2u);
	runReady();
	EXPECT_EQ(notice.calls, 0u);
	backing.readers = 0;
	switch_gpu_admission_changed();
	runReady();
	EXPECT_EQ(notice.calls, 1u);
	EXPECT_EQ(backing.refs, 1u);
}

TEST_F(Resource, NativePreparationWaitPinsSurfaceAndUsesCurrentAuthority)
{
	auto *surface = new OSObject;
	alignas(8) volatile uint64_t generation = 2;
	SwitchGpuResourceUse use{&backing, surface, &generation, 1, SWITCH_GPU_BACKING_READ};
	ASSERT_EQ(client.observe(&use, 1, 11, finished, &notice), 0);
	EXPECT_EQ(surface->refs, 2u);
	runReady();
	EXPECT_EQ(notice.calls, 0u);
	EXPECT_EQ(nativeChecks, 1);
	nativeReady = 1;
	switch_gpu_admission_changed();
	runReady();
	EXPECT_EQ(notice.calls, 1u);
	EXPECT_EQ(nativeChecks, 2);
	EXPECT_EQ(surface->refs, 1u);
	surface->release();
}

TEST_F(Resource, CancelAcknowledgesOnceAndAllowsCookieReuse)
{
	backing.writers = 1;
	SwitchGpuResourceUse use{&backing, nullptr, nullptr, 0, SWITCH_GPU_BACKING_READ};
	ASSERT_EQ(client.observe(&use, 1, 12, finished, &notice), 0);
	EXPECT_EQ(client.cancel(12), 0);
	EXPECT_EQ(client.cancel(12), 0);
	EXPECT_EQ(notice.calls, 1u);
	EXPECT_EQ(notice.result, -125);
	backing.writers = 0;
	ASSERT_EQ(client.observe(&use, 1, 12, finished, &notice), 0);
	runReady();
	EXPECT_EQ(notice.calls, 2u);
	EXPECT_EQ(notice.result, 0);
}

TEST_F(Resource, ClosingCancelsEveryPendingWatchAndRejectsNewArm)
{
	backing.writers = 1;
	SwitchGpuResourceUse use{&backing, nullptr, nullptr, 0, SWITCH_GPU_BACKING_READ};
	std::array<Notice, 64> notices;
	for (unsigned i = 0; i < notices.size(); ++i)
		ASSERT_EQ(client.observe(&use, 1, i + 1, finished, &notices[i]), 0);
	EXPECT_EQ(client.observe(&use, 1, 65, finished, &notice), -28);
	client.stop();
	for (const auto &entry : notices) {
		EXPECT_EQ(entry.calls, 1u);
		EXPECT_EQ(entry.result, -125);
	}
	EXPECT_EQ(client.observe(&use, 1, 65, finished, &notice), -19);
	EXPECT_EQ(notice.calls, 0u);
}
}
