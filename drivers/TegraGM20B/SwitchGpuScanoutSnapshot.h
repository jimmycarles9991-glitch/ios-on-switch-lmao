// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"
#include "SwitchGpuPresentTokens.h"
#include "SwitchGpuPathStats.h"

struct SwitchGpuScanoutSnapshot {
	switch_gpu_backing_token tokens[SWITCH_GPU_PRESENT_MAX_DEPENDENCIES];
	switch_gpu_cpu_lease *leases[2];
	uint32_t count, leaseCount, active;
	void (*done)(void *, int);
	void *display;

	int begin(const switch_gpu_backing_token *dependencies, uint32_t n)
	{
		if (__atomic_load_n(&active, __ATOMIC_ACQUIRE))
			return -16;
		if (n > SWITCH_GPU_PRESENT_MAX_DEPENDENCIES || (n && !dependencies))
			return -22;
		count = n;
		leaseCount = 0;
		for (uint32_t i = 0; i < n; ++i)
			tokens[i] = dependencies[i];
		__atomic_store_n(&active, 1u, __ATOMIC_RELEASE);
		return 0;
	}

	// A surface with writer promises pending: the present acquires them on the GPU, behind a lease their jobs'
	// admissions do not wait for (SwitchGpuWriterPromise.h).
	int add(switch_gpu_backing *backing)
	{
		if (leaseCount == 2)
			return -28;
		switch_gpu_backing_token producer = {};
		const int promised = switch_gpu_backing_snapshot_read_promises(
			backing, &producer, &leases[leaseCount], tokens + count,
			SWITCH_GPU_PRESENT_MAX_DEPENDENCIES - count);
		if (promised < 0)
			return promised;
		++leaseCount;
		count += uint32_t(promised);
		switch_gpu_path_taken(SWITCH_GPU_PATH_PROMISE_WAIT, promised ? 1 : 0);
		if (producer.engine) {
			if (count == SWITCH_GPU_PRESENT_MAX_DEPENDENCIES)
				return -28;
			tokens[count++] = producer;
		}
		return 0;
	}

	void cancel()
	{
		while (leaseCount)
			switch_gpu_backing_cpu_release_owned(&leases[--leaseCount], 0);
		count = 0;
		__atomic_store_n(&active, 0u, __ATOMIC_RELEASE);
	}

	static void complete(void *context, int status)
	{
		auto *snapshot = static_cast<SwitchGpuScanoutSnapshot *>(context);
		const auto fn = snapshot->done;
		void *arg = snapshot->display;
		snapshot->cancel();
		fn(arg, status);
	}
};
