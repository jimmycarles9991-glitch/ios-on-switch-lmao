// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include <string.h>

// A connection's BOs by handle. SubmitJob finds them with no connection lock, inside its channel's submit lock; every
// change is made under the connection lock. Handles are issued in increasing order (next_handle_), so an insert appends
// to the published table (the entry, then its count with one release store) and a lookup is a binary search; an erase
// stores a null BO in its entry. A full or half-erased table is copied into a new one, published with one release
// store; the old one waits in takeRetired() for the caller to free once no reader can still hold it (retireBatch, after
// taking every submit lock once). Memory is the allocation policy: Memory::allocate<T>(n) and Memory::release(items, n).
template <typename Bo, typename Memory> class SwitchGpuBoIndex
{
	struct Entry {
		uint64_t handle;
		Bo *bo;
	};

public:
	struct Table {
		Entry *entries;
		uint32_t count, capacity, erased;
		Table *retired;
	};

	SwitchGpuBoIndex() = default;
	SwitchGpuBoIndex(const SwitchGpuBoIndex &) = delete;
	SwitchGpuBoIndex &operator=(const SwitchGpuBoIndex &) = delete;

	// Room for one more, taken before the BO is made so that insert() cannot fail after it.
	bool reserve()
	{
		const Table *t = table_;
		if (t && t->count < t->capacity)
			return true;
		const uint32_t live = t ? t->count - t->erased : 0;
		if (live > UINT32_MAX / 4)
			return false;
		return rebuild(live * 2 > 64 ? live * 2 : 64);
	}

	void insert(uint64_t handle, Bo *bo)
	{
		Table *t = table_;
		t->entries[t->count].handle = handle;
		__atomic_store_n(&t->entries[t->count].bo, bo, __ATOMIC_RELAXED);
		__atomic_store_n(&t->count, t->count + 1, __ATOMIC_RELEASE);
	}

	Bo *find(uint64_t handle) const
	{
		const Table *t = __atomic_load_n(&table_, __ATOMIC_ACQUIRE);
		if (!t)
			return nullptr;
		const uint32_t count = __atomic_load_n(&t->count, __ATOMIC_ACQUIRE);
		const uint32_t at = lower(t->entries, count, handle);
		return at < count && t->entries[at].handle == handle
			       ? __atomic_load_n(&t->entries[at].bo, __ATOMIC_ACQUIRE)
			       : nullptr;
	}

	void erase(uint64_t handle)
	{
		Table *t = table_;
		if (!t)
			return;
		const uint32_t at = lower(t->entries, t->count, handle);
		if (at == t->count || t->entries[at].handle != handle || !t->entries[at].bo)
			return;
		__atomic_store_n(&t->entries[at].bo, nullptr, __ATOMIC_RELEASE);
		if (++t->erased > 64 && t->erased * 2 > t->count)
			rebuild(t->capacity);
	}

	// No reader is left (the connection's submit gate is closed and drained)
	void clear()
	{
		if (table_) {
			__atomic_store_n(&table_->count, 0u, __ATOMIC_RELEASE);
			table_->erased = 0;
		}
	}

	Table *takeRetired()
	{
		Table *retired = retired_;
		retired_ = nullptr;
		return retired;
	}

	static void releaseRetired(Table *t)
	{
		while (t) {
			Table *next = t->retired;
			Memory::release(t->entries, t->capacity);
			Memory::release(t, 1);
			t = next;
		}
	}

	void destroy()
	{
		releaseRetired(takeRetired());
		if (table_)
			table_->retired = nullptr;
		releaseRetired(table_);
		table_ = nullptr;
	}

	uint32_t size() const
	{
		return table_ ? table_->count - table_->erased : 0;
	}

private:
	static uint32_t lower(const Entry *entries, uint32_t count, uint64_t handle)
	{
		uint32_t first = 0, last = count;
		while (first < last) {
			const uint32_t middle = first + (last - first) / 2;
			if (entries[middle].handle < handle)
				first = middle + 1;
			else
				last = middle;
		}
		return first;
	}

	bool rebuild(uint32_t capacity)
	{
		Table *made = Memory::template allocate<Table>(1);
		Entry *entries = made ? Memory::template allocate<Entry>(capacity) : nullptr;
		if (!entries) {
			if (made)
				Memory::release(made, 1);
			return false;
		}
		uint32_t count = 0;
		for (uint32_t i = 0; table_ && i < table_->count; ++i)
			if (table_->entries[i].bo)
				entries[count++] = table_->entries[i];
		*made = {entries, count, capacity, 0, nullptr};
		if (table_) {
			table_->retired = retired_;
			retired_ = table_;
		}
		__atomic_store_n(&table_, made, __ATOMIC_RELEASE);
		return true;
	}

	Table *table_ = nullptr, *retired_ = nullptr;
};
