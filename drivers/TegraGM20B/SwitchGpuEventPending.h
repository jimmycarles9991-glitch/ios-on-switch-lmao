// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

namespace SwitchGpuEventPending
{
struct Layout {
	uint32_t head, tail, next, previous;
};

inline void **pointer(void *object, uint32_t offset)
{
	return reinterpret_cast<void **>(static_cast<uint8_t *>(object) + offset);
}

// The caller holds the native event lock and retains waiter. Listener dispatch reuses these links only after
// removing the waiter from this list under that same lock; an absent waiter must never have its links touched.
inline bool remove(void *event, void *waiter, Layout layout)
{
	auto **head = pointer(event, layout.head);
	auto **cursor = head;
	while (*cursor && *cursor != waiter)
		cursor = pointer(*cursor, layout.next);
	if (!*cursor)
		return false;
	auto **nextField = pointer(waiter, layout.next);
	auto **previousField = pointer(waiter, layout.previous);
	if (*previousField != cursor)
		return false;
	void *next = *nextField;
	if (next) {
		auto **nextPrevious = pointer(next, layout.previous);
		if (*nextPrevious != nextField)
			return false;
		*nextPrevious = cursor;
	} else {
		auto **tail = pointer(event, layout.tail);
		if (*tail != nextField)
			return false;
		*tail = cursor;
	}
	*cursor = next;
	*nextField = nullptr;
	*previousField = nullptr;
	return true;
}
}
