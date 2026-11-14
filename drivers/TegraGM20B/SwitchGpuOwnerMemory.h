// SPDX-License-Identifier: MIT
// Debug kernels' "memory by process" stats line: what each GPU connection holds (its BoAllocs of pool and other memory,
// its imports of a task's memory), summed by process (TegraGM20BOwnerMemory.cpp). Each connection's counts change only
// under its own lock_, where its BOs are made and become ready to release, so they are one writer's plain values; the
// stats tick reads them with relaxed loads and takes no lock of the connection's. C++, no kernel headers: the host test
// runs it, with its own snprintf declared first.
#pragma once
#include <stdint.h>
#include <string.h>

/* Whether connections count their BOs: debug kernels only (TegraGM20BGpu.h TGM_STATS, expanded where it is used); a
 * host model of a connection sets it */
#ifndef SWITCH_GPU_OWNER_COUNTS
#define SWITCH_GPU_OWNER_COUNTS TGM_STATS
#endif

enum SwitchGpuOwnerKind : uint8_t {
	SWITCH_GPU_OWNER_IMPORTED,
	SWITCH_GPU_OWNER_POOL,
	SWITCH_GPU_OWNER_ALLOCATED,
	SWITCH_GPU_OWNER_KINDS
};

struct SwitchGpuOwnerMemory {
	uint64_t bytes[SWITCH_GPU_OWNER_KINDS];
	uint32_t count[SWITCH_GPU_OWNER_KINDS];

	// The connection's lock_ held
	void change(SwitchGpuOwnerKind kind, uint64_t delta, bool add)
	{
		const uint64_t b = bytes[kind], n = count[kind];
		__atomic_store_n(&bytes[kind], add ? b + delta : (b > delta ? b - delta : 0),
				 __ATOMIC_RELAXED);
		__atomic_store_n(&count[kind], uint32_t(add ? n + 1 : (n ? n - 1 : 0)),
				 __ATOMIC_RELAXED);
	}

	SwitchGpuOwnerMemory snapshot() const
	{
		SwitchGpuOwnerMemory out = {};
		for (uint32_t k = 0; k < SWITCH_GPU_OWNER_KINDS; k++) {
			out.bytes[k] = __atomic_load_n(&bytes[k], __ATOMIC_RELAXED);
			out.count[k] = __atomic_load_n(&count[k], __ATOMIC_RELAXED);
		}
		return out;
	}

	uint64_t total() const
	{
		uint64_t sum = 0;
		for (const uint64_t b : bytes)
			sum += b;
		return sum;
	}
};

struct SwitchGpuOwnerEntry {
	int32_t pid;
	char name[17];
	SwitchGpuOwnerMemory memory;
};

// Folds the connections of one process into its first entry: the processes, in place, `n` of them after
inline uint32_t switch_gpu_owner_fold(SwitchGpuOwnerEntry *entries, uint32_t n)
{
	uint32_t out = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t j = 0;
		while (j < out && entries[j].pid != entries[i].pid)
			j++;
		if (j == out) {
			entries[out++] = entries[i];
			continue;
		}
		for (uint32_t k = 0; k < SWITCH_GPU_OWNER_KINDS; k++) {
			entries[j].memory.bytes[k] += entries[i].memory.bytes[k];
			entries[j].memory.count[k] += entries[i].memory.count[k];
		}
	}
	return out;
}

// "name[pid] T KiB (imports B KiB in N, pool B KiB in N, allocated B KiB in N); ..." for the `most` processes holding the
// most, largest first, then "others T KiB in P processes"; entries are folded and reordered. The bytes written.
inline unsigned long switch_gpu_owner_line(SwitchGpuOwnerEntry *entries, uint32_t n, uint32_t most,
					   char *out, unsigned long size)
{
	if (!size)
		return 0;
	out[0] = 0;
	n = switch_gpu_owner_fold(entries, n);
	for (uint32_t i = 1; i < n; i++) {
		const SwitchGpuOwnerEntry e = entries[i];
		uint32_t j = i;
		for (; j > 0 && entries[j - 1].memory.total() < e.memory.total(); j--)
			entries[j] = entries[j - 1];
		entries[j] = e;
	}
	unsigned long at = 0;
	const auto put = [&](int written) {
		if (written > 0)
			at = at + unsigned(written) < size ? at + unsigned(written) : size - 1;
	};
	uint64_t restBytes = 0;
	uint32_t rest = 0;
	for (uint32_t i = 0; i < n; i++) {
		const SwitchGpuOwnerMemory &m = entries[i].memory;
		if (!m.total())
			continue;
		if (i >= most) {
			restBytes += m.total();
			rest++;
			continue;
		}
		put(snprintf(out + at, size - at,
			     "%s%s[%d] %llu KiB (imports %llu KiB in %u, pool %llu KiB in %u, "
			     "allocated %llu KiB in %u)",
			     at ? "; " : "", entries[i].name, entries[i].pid,
			     (unsigned long long)(m.total() >> 10),
			     (unsigned long long)(m.bytes[SWITCH_GPU_OWNER_IMPORTED] >> 10),
			     m.count[SWITCH_GPU_OWNER_IMPORTED],
			     (unsigned long long)(m.bytes[SWITCH_GPU_OWNER_POOL] >> 10),
			     m.count[SWITCH_GPU_OWNER_POOL],
			     (unsigned long long)(m.bytes[SWITCH_GPU_OWNER_ALLOCATED] >> 10),
			     m.count[SWITCH_GPU_OWNER_ALLOCATED]));
	}
	if (rest)
		put(snprintf(out + at, size - at, "%sothers %llu KiB in %u processes",
			     at ? "; " : "", (unsigned long long)(restBytes >> 10), rest));
	return at;
}
