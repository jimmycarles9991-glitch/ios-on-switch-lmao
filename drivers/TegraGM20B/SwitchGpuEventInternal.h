// SPDX-License-Identifier: MIT
#pragma once
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSObject.h>
#include "SwitchGpuSharedEvent.h"
#include "SwitchGpuSharedEventNative.h"

struct SwitchGpuEventRecord {
	SwitchGpuEventRecord *disposeNext;
	uint32_t refs, kind;
};

enum {
	SwitchEventSignal = 1,
	SwitchEventUse = 2,
	SwitchEventWaits = 3
};

struct switch_gpu_event_signal : SwitchGpuEventRecord {
	switch_gpu_event_signal *next;
	switch_gpu_event *event;
	switch_gpu_backing_token token;
	uint64_t value;
	bool bound;
	int result;
	switch_gpu_event_waits *dependencies;
	struct switch_gpu_event_word *word;  // released mid-job into it (SwitchGpuEventWord.h)
	uint64_t wordBase;
	uint32_t payload, wordPrevious;
};

struct switch_gpu_event {
	switch_gpu_event *next;
	OSObject *native;
	switch_gpu_event_signal *signals;
	struct switch_gpu_event_watch *watches;
	uint64_t readyThrough, failedThrough;
	uint32_t refs, reservations, observers;
	int failure;
};

struct switch_gpu_event_watch final : OSObject {
OSDeclareDefaultStructors(switch_gpu_event_watch) public : bool initialize();
	void free() override;
	switch_gpu_event_watch *next = nullptr;
	switch_gpu_event *event = nullptr;
	OSObject *waiter = nullptr;
	switch_gpu_event_signal *authority = nullptr;
	uint64_t value = 0, cookie = 0;
	void (*completed)(void *, uint64_t, int) = nullptr;
	void *context = nullptr;
	int result = 0, terminalStatus = 0;
	bool arming = true, done = false, terminal = false;
};

struct switch_gpu_event_use : SwitchGpuEventRecord {
	switch_gpu_backing_token token;
	switch_gpu_event_signal *authority;
	switch_gpu_event_waits *batchAuthority;
};

struct switch_gpu_event_waits : SwitchGpuEventRecord {
	switch_gpu_event_waits *next;
	switch_gpu_backing_token consumer;
	switch_gpu_event_use **uses, *inlineUses[SWITCH_GPU_EVENT_MAX_JOB_USES];
	uint32_t count, capacity;
	bool committed;
	switch_gpu_event_waits *previous;
	int result;
	bool bound;
	void (*completed)(void *, int);
	void *context;
};

struct SwitchGpuEvents {
	IOLock *lock;
	thread_call_t work;
	OSObject *listener;
	switch_gpu_event *events;
	switch_gpu_event_waits *waits, *waitsTail;
	uint32_t pendingSignals;
};

SwitchGpuEvents *switch_gpu_events_state();
void switch_gpu_events_schedule();
int switch_gpu_event_resolve_locked(switch_gpu_event *, uint64_t, switch_gpu_backing_token *);
void switch_gpu_event_watches_retire();
void switch_gpu_event_watches_bind_locked(switch_gpu_event *);
void switch_gpu_event_signal_drop(switch_gpu_event_signal *);
void switch_gpu_event_waits_drop(switch_gpu_event_waits *);
int switch_gpu_event_waits_query_locked(switch_gpu_event_waits *);
int switch_gpu_event_waits_producers_locked(const switch_gpu_event_waits *);
void switch_gpu_event_dispose(SwitchGpuEventRecord *);
void switch_gpu_events_dispose();
bool switch_gpu_events_reserve();
void switch_gpu_events_unreserve();
