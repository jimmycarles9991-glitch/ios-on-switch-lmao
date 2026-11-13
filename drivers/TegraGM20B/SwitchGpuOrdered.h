// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

class SwitchGpuOrdered
{
public:
	static constexpr unsigned kSlots = 64;

	unsigned enter(uintptr_t thread)
	{
		if (!thread)
			return 0;
		const unsigned first = static_cast<unsigned>((thread >> 4) % kSlots);
		for (unsigned n = 0; n < kSlots; ++n) {
			const unsigned slot = (first + n) % kSlots;
			uintptr_t empty = 0;
			if (__atomic_compare_exchange_n(&owners_[slot], &empty, thread, false,
							__ATOMIC_RELAXED, __ATOMIC_RELAXED))
				return slot + 1;
		}
		// Saturation retains synchronous visibility; it must never borrow another thread's scope.
		return 0;
	}

	void leave(unsigned scope)
	{
		if (scope && scope <= kSlots)
			__atomic_store_n(&owners_[scope - 1], uintptr_t{0}, __ATOMIC_RELAXED);
	}

	bool active(uintptr_t thread) const
	{
		if (!thread)
			return false;
		const unsigned first = static_cast<unsigned>((thread >> 4) % kSlots);
		for (unsigned n = 0; n < kSlots; ++n)
			if (__atomic_load_n(&owners_[(first + n) % kSlots], __ATOMIC_RELAXED) ==
			    thread)
				return true;
		return false;
	}

private:
	uintptr_t owners_[kSlots] = {};
};

static_assert(__atomic_always_lock_free(sizeof(uintptr_t), nullptr));
