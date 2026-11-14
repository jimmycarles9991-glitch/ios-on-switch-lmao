// SPDX-License-Identifier: MIT
// nvkm's wait notifications (the in-tree core/event.c): only an allowed one is on the event's list.
#include "nvkm_event_wait_fixture.h"
#include <gtest/gtest.h>

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
