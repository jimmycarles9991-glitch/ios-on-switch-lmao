// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <string.h>

// A connection's BOs by handle, for the lookups of every submission's uses. Handles are issued in increasing order
// (next_handle_), so an insert appends and a lookup is a binary search. Memory is the allocation policy:
// Memory::allocate<Entry>(n) and Memory::release(entries, n).
template <typename Bo, typename Memory> class SwitchGpuBoIndex
{
public:
	SwitchGpuBoIndex() = default;
	SwitchGpuBoIndex(const SwitchGpuBoIndex &) = delete;
	SwitchGpuBoIndex &operator=(const SwitchGpuBoIndex &) = delete;

	// Room for one more, taken before the BO is made so that insert() cannot fail after it.
	bool reserve()
	{
		if (count_ < capacity_)
			return true;
		if (capacity_ > UINT32_MAX / 2)
			return false;
		const uint32_t capacity = capacity_ ? capacity_ * 2 : 64;
		auto *grown = Memory::template allocate<Entry>(capacity);
		if (!grown)
			return false;
		if (count_)
			memcpy(grown, entries_, sizeof(Entry) * count_);
		Memory::release(entries_, capacity_);
		entries_ = grown;
		capacity_ = capacity;
		return true;
	}

	void insert(uint64_t handle, Bo *bo)
	{
		uint32_t at = count_;
		while (at && entries_[at - 1].handle > handle)
			--at;
		memmove(entries_ + at + 1, entries_ + at, sizeof(Entry) * (count_ - at));
		entries_[at] = {handle, bo};
		++count_;
	}

	Bo *find(uint64_t handle) const
	{
		const uint32_t at = lower(handle);
		return at < count_ && entries_[at].handle == handle ? entries_[at].bo : nullptr;
	}

	void erase(uint64_t handle)
	{
		const uint32_t at = lower(handle);
		if (at == count_ || entries_[at].handle != handle)
			return;
		memmove(entries_ + at, entries_ + at + 1, sizeof(Entry) * (count_ - at - 1));
		--count_;
	}

	void clear()
	{
		count_ = 0;
	}

	void destroy()
	{
		Memory::release(entries_, capacity_);
		entries_ = nullptr;
		count_ = capacity_ = 0;
	}

	uint32_t size() const
	{
		return count_;
	}

private:
	struct Entry {
		uint64_t handle;
		Bo *bo;
	};

	uint32_t lower(uint64_t handle) const
	{
		uint32_t first = 0, last = count_;
		while (first < last) {
			const uint32_t middle = first + (last - first) / 2;
			if (entries_[middle].handle < handle)
				first = middle + 1;
			else
				last = middle;
		}
		return first;
	}

	Entry *entries_ = nullptr;
	uint32_t count_ = 0, capacity_ = 0;
};
