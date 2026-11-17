// SPDX-License-Identifier: MIT
// nvkm's wait notifications (the in-tree core/event.c): only an allowed one is on the event's list.
#include "nvkm_event_wait_fixture.h"
#include <gtest/gtest.h>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include <thread>

class EventWait : public testing::Test
{
protected:
	event_wait_fixture *f = event_wait_create();

	void TearDown() override
	{
		event_wait_destroy(f);
	}
};

TEST_F(EventWait, OnlyAllowedNotificationsAreDispatched)
{
	for (unsigned i = 0; i < 64; i++)
		event_wait_add(f, i, 1);
	event_wait_allow(f, 17);
	event_wait_allow(f, 29);
	EXPECT_EQ(event_wait_members(f), 2u);
	EXPECT_EQ(event_wait_refcount(f), 2u);
	event_wait_dispatch(f);
	for (unsigned i = 0; i < 64; i++)
		EXPECT_EQ(event_wait_calls(f, i), unsigned(i == 17 || i == 29));
}

TEST_F(EventWait, SuspendRestoresOnlyAllowedNotification)
{
	event_wait_add(f, 0, 1);
	event_wait_add(f, 1, 1);
	event_wait_allow(f, 0);
	EXPECT_EQ(event_wait_suspend(f, 0), 1);
	EXPECT_EQ(event_wait_suspend(f, 1), 0);
	EXPECT_EQ(event_wait_members(f), 0u);
	event_wait_resume(f, 0);
	event_wait_resume(f, 1);
	EXPECT_EQ(event_wait_members(f), 1u);
	EXPECT_EQ(event_wait_refcount(f), 1u);
}

TEST_F(EventWait, LegacyNotificationsKeepPermanentMembership)
{
	event_wait_add(f, 0, 0);
	event_wait_allow(f, 0);
	event_wait_dispatch(f);
	event_wait_dispatch(f);
	EXPECT_EQ(event_wait_calls(f, 0), 2u);
	EXPECT_EQ(event_wait_members(f), 1u);
	event_wait_remove(f, 0);
	EXPECT_EQ(event_wait_members(f), 0u);
}

TEST_F(EventWait, DeleteSerializesWithDispatch)
{
	event_wait_add(f, 0, 1);
	event_wait_allow(f, 0);
	event_wait_remove(f, 0);
	event_wait_dispatch(f);
	EXPECT_EQ(event_wait_calls(f, 0), 0u);
	EXPECT_EQ(event_wait_refcount(f), 0u);
}

TEST_F(EventWait, DeferredDispatchQueuesOneSetupAllocatedCall)
{
	ASSERT_EQ(switch_event_deferred_add(f, 1, 0), 0);
	switch_event_deferred_allow(f);
	EXPECT_EQ(switch_event_deferred_allocations(f), 1u);
	event_wait_dispatch(f);
	event_wait_dispatch(f);
	EXPECT_EQ(event_wait_calls(f, 0), 0u);
	EXPECT_EQ(switch_event_deferred_queued(f), 1u);
	switch_event_deferred_run(f);
	EXPECT_EQ(event_wait_calls(f, 0), 1u);
	EXPECT_EQ(switch_event_deferred_inside_dispatch(f), 0u);
	EXPECT_EQ(switch_event_deferred_queued(f), 0u);
	switch_event_deferred_run(f);
	EXPECT_EQ(event_wait_calls(f, 0), 1u);
	event_wait_dispatch(f);
	switch_event_deferred_run(f);
	EXPECT_EQ(event_wait_calls(f, 0), 2u);
	EXPECT_EQ(switch_event_deferred_allocations(f), 1u);
}

TEST_F(EventWait, DeferredBlockUnsubscribesBeforeDraining)
{
	ASSERT_EQ(switch_event_deferred_add(f, 0, 0), 0);
	switch_event_deferred_allow(f);
	event_wait_dispatch(f);
	switch_event_deferred_block(f);
	EXPECT_EQ(event_wait_calls(f, 0), 1u);
	EXPECT_EQ(event_wait_members(f), 0u);
	EXPECT_EQ(event_wait_refcount(f), 0u);
	EXPECT_EQ(switch_event_deferred_callback_members(f), 0u);
	EXPECT_EQ(switch_event_deferred_drain_members(f), 0u);
	event_wait_dispatch(f);
	switch_event_deferred_run(f);
	EXPECT_EQ(event_wait_calls(f, 0), 1u);
	switch_event_deferred_allow(f);
	event_wait_dispatch(f);
	switch_event_deferred_run(f);
	EXPECT_EQ(event_wait_calls(f, 0), 2u);
}

TEST_F(EventWait, DeferredFiniDrainsAndInitRestoresSubscription)
{
	ASSERT_EQ(switch_event_deferred_add(f, 1, 0), 0);
	switch_event_deferred_allow(f);
	event_wait_dispatch(f);
	switch_event_deferred_fini(f);
	EXPECT_EQ(event_wait_calls(f, 0), 1u);
	EXPECT_EQ(event_wait_members(f), 0u);
	EXPECT_EQ(switch_event_deferred_callback_members(f), 0u);
	EXPECT_EQ(switch_event_deferred_drain_members(f), 0u);
	event_wait_dispatch(f);
	EXPECT_EQ(switch_event_deferred_queued(f), 0u);
	switch_event_deferred_init(f);
	EXPECT_EQ(event_wait_members(f), 1u);
	event_wait_dispatch(f);
	switch_event_deferred_run(f);
	EXPECT_EQ(event_wait_calls(f, 0), 2u);
}

TEST_F(EventWait, DeferredDestructorDrainsBeforeFree)
{
	ASSERT_EQ(switch_event_deferred_add(f, 1, 0), 0);
	switch_event_deferred_allow(f);
	event_wait_dispatch(f);
	switch_event_deferred_remove(f);
	EXPECT_EQ(event_wait_calls(f, 0), 1u);
	EXPECT_EQ(event_wait_members(f), 0u);
	EXPECT_EQ(switch_event_deferred_callback_members(f), 0u);
	EXPECT_EQ(switch_event_deferred_drain_members(f), 0u);
	EXPECT_EQ(switch_event_deferred_frees(f), 1u);
	EXPECT_EQ(switch_event_deferred_queued(f), 0u);
	event_wait_dispatch(f);
	EXPECT_EQ(event_wait_calls(f, 0), 1u);
}

TEST_F(EventWait, DeferredDestructorJoinsAnActiveCallback)
{
	ASSERT_EQ(switch_event_deferred_add(f, 1, 0), 0);
	switch_event_deferred_allow(f);

	struct Callback {
		std::mutex mutex;
		std::condition_variable changed;
		bool entered = false, release = false;
	} callback;

	switch_event_deferred_hook(
		f,
		[](void *opaque) {
			auto &state = *static_cast<Callback *>(opaque);
			std::unique_lock lock(state.mutex);
			state.entered = true;
			state.changed.notify_all();
			state.changed.wait(lock, [&] {
				return state.release;
			});
		},
		&callback);
	event_wait_dispatch(f);
	std::thread worker([&] {
		switch_event_deferred_run(f);
	});
	{
		std::unique_lock lock(callback.mutex);
		callback.changed.wait(lock, [&] {
			return callback.entered;
		});
	}
	std::atomic<bool> destroyed = false;
	std::thread destructor([&] {
		switch_event_deferred_remove(f);
		destroyed.store(true);
	});
	switch_event_deferred_wait_draining(f);
	EXPECT_FALSE(destroyed.load());
	EXPECT_EQ(event_wait_members(f), 0u);
	EXPECT_EQ(switch_event_deferred_frees(f), 0u);
	{
		std::lock_guard lock(callback.mutex);
		callback.release = true;
		callback.changed.notify_all();
	}
	worker.join();
	destructor.join();
	EXPECT_TRUE(destroyed.load());
	EXPECT_EQ(switch_event_deferred_frees(f), 1u);
	EXPECT_EQ(event_wait_calls(f, 0), 1u);
}

TEST_F(EventWait, DeferredAllocationFailureLeavesNoSubscriber)
{
	EXPECT_EQ(switch_event_deferred_add(f, 1, 1), -ENOMEM);
	EXPECT_EQ(event_wait_members(f), 0u);
	EXPECT_EQ(event_wait_refcount(f), 0u);
	EXPECT_EQ(switch_event_deferred_allocations(f), 0u);
	EXPECT_EQ(switch_event_deferred_frees(f), 0u);
	event_wait_dispatch(f);
	EXPECT_EQ(event_wait_calls(f, 0), 0u);
}
