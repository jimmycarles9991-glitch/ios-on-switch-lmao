// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOLocks.h>
#include "SwitchGpuBacking.h"

class OSObject;

struct SwitchGpuResourceUse {
	switch_gpu_backing *backing;
	OSObject *surface;
	const volatile uint64_t *generation;
	uint64_t expected;
	uint32_t access;
};

class SwitchGpuResourceClient
{
public:
	using Callback = void (*)(void *, uint64_t, int);
	bool initialize();
	int observe(const SwitchGpuResourceUse *, uint32_t count, uint64_t cookie, Callback,
		    void *context);
	int cancel(uint64_t cookie);
	void stop();
	// The containing UserClient retains itself until every accepted callback has returned.
	void destroy();

private:
	struct Watch;
	IOLock *lock_ = nullptr;
	Watch *watches_ = nullptr;
	uint32_t count_ = 0;
	bool closed_ = false;
	bool unlink(Watch *);
	static void drop(Watch *);
	static int query(void *);
	static void completed(void *, int);
};
