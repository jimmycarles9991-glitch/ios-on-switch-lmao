// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <array>
#include <cstring>
#include "../SwitchGpuEventPending.h"

namespace
{
using namespace SwitchGpuEventPending;

struct alignas(8) Object {
	unsigned char bytes[128] = {};
};

struct Pending {
	Object event, a, b, c;
	Layout layout;

	explicit Pending(bool newer)
	    : layout{newer ? 0x40u : 0x48u, newer ? 0x48u : 0x50u, 0x28, 0x30}
	{
		*pointer(&event, layout.head) = &a;
		*pointer(&a, layout.previous) = pointer(&event, layout.head);
		*pointer(&a, layout.next) = &b;
		*pointer(&b, layout.previous) = pointer(&a, layout.next);
		*pointer(&b, layout.next) = &c;
		*pointer(&c, layout.previous) = pointer(&b, layout.next);
		*pointer(&event, layout.tail) = pointer(&c, layout.next);
	}

	bool erase(Object &object)
	{
		return remove(&event, &object, layout);
	}
};
}

TEST(SharedEventPending, RemovesHeadWithoutChangingRemainingTail)
{
	for (bool newer : {false, true}) {
		Pending list(newer);
		ASSERT_TRUE(list.erase(list.a));
		EXPECT_EQ(*pointer(&list.event, list.layout.head), &list.b);
		EXPECT_EQ(*pointer(&list.b, list.layout.previous),
			  pointer(&list.event, list.layout.head));
		EXPECT_EQ(*pointer(&list.event, list.layout.tail),
			  pointer(&list.c, list.layout.next));
	}
}

TEST(SharedEventPending, RemovesMiddleWithoutTouchingUnrelatedPayload)
{
	Pending list(true);
	list.b.bytes[0x10] = 0x7d;
	ASSERT_TRUE(list.erase(list.b));
	EXPECT_EQ(*pointer(&list.a, list.layout.next), &list.c);
	EXPECT_EQ(*pointer(&list.c, list.layout.previous), pointer(&list.a, list.layout.next));
	EXPECT_EQ(list.b.bytes[0x10], 0x7d);
}

TEST(SharedEventPending, RemovesTailAndLastEntry)
{
	for (bool newer : {false, true}) {
		Pending list(newer);
		ASSERT_TRUE(list.erase(list.c));
		EXPECT_EQ(*pointer(&list.event, list.layout.tail),
			  pointer(&list.b, list.layout.next));
		ASSERT_TRUE(list.erase(list.a));
		ASSERT_TRUE(list.erase(list.b));
		EXPECT_EQ(*pointer(&list.event, list.layout.head), nullptr);
		EXPECT_EQ(*pointer(&list.event, list.layout.tail),
			  pointer(&list.event, list.layout.head));
	}
}

TEST(SharedEventPending, CancelAfterNativeDequeueDoesNotTouchListenerLinks)
{
	Pending list(true);
	ASSERT_TRUE(list.erase(list.b));
	Object listener;
	*pointer(&list.b, list.layout.next) = &listener;
	*pointer(&list.b, list.layout.previous) = pointer(&listener, 8);
	const auto before = list.b;
	EXPECT_FALSE(list.erase(list.b));
	EXPECT_EQ(std::memcmp(&before, &list.b, sizeof(before)), 0);
}

TEST(SharedEventPending, RepeatedCancelDoesNotConsumeAnotherListReference)
{
	Pending list(false);
	unsigned releases = 0;
	if (list.erase(list.a))
		++releases;
	if (list.erase(list.a))
		++releases;
	EXPECT_EQ(releases, 1u);
}

TEST(SharedEventPending, RefusesWrongPreviousLinkWithoutMutation)
{
	Pending list(true);
	*pointer(&list.b, list.layout.previous) = &list.event;
	const auto a = list.a, b = list.b, c = list.c;
	EXPECT_FALSE(list.erase(list.b));
	EXPECT_EQ(std::memcmp(&a, &list.a, sizeof(a)), 0);
	EXPECT_EQ(std::memcmp(&b, &list.b, sizeof(b)), 0);
	EXPECT_EQ(std::memcmp(&c, &list.c, sizeof(c)), 0);
}

TEST(SharedEventPending, RefusesWrongNextLinkWithoutMutation)
{
	Pending list(false);
	*pointer(&list.c, list.layout.previous) = &list.a;
	const auto a = list.a, b = list.b, c = list.c;
	EXPECT_FALSE(list.erase(list.b));
	EXPECT_EQ(std::memcmp(&a, &list.a, sizeof(a)), 0);
	EXPECT_EQ(std::memcmp(&b, &list.b, sizeof(b)), 0);
	EXPECT_EQ(std::memcmp(&c, &list.c, sizeof(c)), 0);
}

TEST(SharedEventPending, RefusesWrongTailWithoutMutation)
{
	Pending list(true);
	*pointer(&list.event, list.layout.tail) = &list.a;
	const auto event = list.event, b = list.b, c = list.c;
	EXPECT_FALSE(list.erase(list.c));
	EXPECT_EQ(std::memcmp(&event, &list.event, sizeof(event)), 0);
	EXPECT_EQ(std::memcmp(&b, &list.b, sizeof(b)), 0);
	EXPECT_EQ(std::memcmp(&c, &list.c, sizeof(c)), 0);
}
