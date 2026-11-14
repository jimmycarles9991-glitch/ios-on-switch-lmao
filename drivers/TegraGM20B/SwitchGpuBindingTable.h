// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>
#include "SwitchGpuBindings.h"

// A connection's bindings as SubmitJob reads them with no connection lock, inside its channel's submit lock: a copy of
// the sorted list the connection lock keeps, made whole after each change and published with one release store. The
// copy a change replaces waits in takeRetired() until no reader can still hold it (retireBatch, after taking every
// submit lock once). A change takes its copy's room before it changes anything (prepare), so publishing cannot fail.
template <typename Bo, typename Memory> class SwitchGpuBindingTable
{
public:
	struct Entry {
		uint64_t va, range;
		Bo *bo;
		uint64_t bo_offset;
	};

	struct Snapshot {
		Entry *entries;
		uint32_t count, capacity;
		Snapshot *retired;
	};

	SwitchGpuBindingTable() = default;
	SwitchGpuBindingTable(const SwitchGpuBindingTable &) = delete;
	SwitchGpuBindingTable &operator=(const SwitchGpuBindingTable &) = delete;

	static Snapshot *prepare(uint32_t capacity)
	{
		Snapshot *made = Memory::template allocate<Snapshot>(1);
		Entry *entries =
			made && capacity ? Memory::template allocate<Entry>(capacity) : nullptr;
		if (made && capacity && !entries) {
			Memory::release(made, 1);
			return nullptr;
		}
		if (made)
			*made = {entries, 0, capacity, nullptr};
		return made;
	}

	static void discard(Snapshot *made)
	{
		if (!made)
			return;
		Memory::release(made->entries, made->capacity);
		Memory::release(made, 1);
	}

	// `made` from prepare() with room for `count` of `bindings` (anything with va, range, bo and bo_offset)
	template <typename Binding>
	void publish(Snapshot *made, const Binding *bindings, uint32_t count)
	{
		for (uint32_t i = 0; i < count; ++i)
			made->entries[i] = {bindings[i].va, bindings[i].range, bindings[i].bo,
					    bindings[i].bo_offset};
		made->count = count;
		Snapshot *old = published_;
		__atomic_store_n(&published_, made, __ATOMIC_RELEASE);
		if (old) {
			old->retired = retired_;
			retired_ = old;
		}
	}

	const Entry *find(uint64_t address, uint64_t bytes) const
	{
		const Snapshot *s = __atomic_load_n(&published_, __ATOMIC_ACQUIRE);
		return s ? switch_gpu_binding_find(s->entries, s->count, address, bytes) : nullptr;
	}

	Snapshot *takeRetired()
	{
		Snapshot *retired = retired_;
		retired_ = nullptr;
		return retired;
	}

	static void releaseRetired(Snapshot *s)
	{
		while (s) {
			Snapshot *next = s->retired;
			discard(s);
			s = next;
		}
	}

	void destroy()
	{
		releaseRetired(takeRetired());
		discard(published_);
		published_ = nullptr;
	}

private:
	Snapshot *published_ = nullptr, *retired_ = nullptr;
};
