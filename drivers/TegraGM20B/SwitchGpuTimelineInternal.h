// SPDX-License-Identifier: MIT
// A GM20B channel's timeline (SwitchGpuTimeline.cpp) as its reads under a held reference see it (SwitchGpuTimelineRefs.cpp).
#pragma once
#include <IOKit/IOLocks.h>
#include "SwitchGpuTimeline.h"
#include "TegraGM20BABI.h"

struct switch_gpu_timeline_semantic {
	switch_gpu_timeline_semantic *next;
	switch_gpu_timeline *timeline;
	switch_gpu_event_waits *authority;
	uint32_t seqno;
};

// The newest jobs whose own event waits failed. A failure belongs to that job alone: later jobs on the channel do not
// depend on its wait. An older failure pushed out by newer ones reads as its physical status.
constexpr uint32_t kFailedKept = 8;

// The host1x syncpoint the channel's draining jobs end with (SwitchGpuTimelineSyncpt.cpp). id is 0 until a consumer
// asks, then set once under the lock with base before it. increments and marks belong to the channel's submitter alone;
// a mark is one draining job's seqnos and threshold, the newest kSyncptMarks kept. Its version is odd while the
// submitter rewrites it; a reader that sees it change skips the mark.
constexpr uint32_t kSyncptMarks = 8;

struct switch_gpu_syncpt_mark {
	uint32_t version;
	uint32_t first, last, threshold;
};

struct switch_gpu_timeline_syncpt {
	uint32_t id, base, increments;
	switch_gpu_syncpt_mark marks[kSyncptMarks];
};

// semantics and failed_count change under the lock and are stored atomically: a read under a held reference finds the
// status the physical one with neither (switch_gpu_timeline_held_status).
struct switch_gpu_timeline {
	switch_gpu_timeline *next;
	uint64_t uid;
	const volatile uint32_t *fence;
	const volatile uint32_t *allocation;
	const volatile uint32_t *visible;
	uint32_t terminal_visible, promoted;
	uint64_t terminal;
	uint32_t refs;
	switch_gpu_timeline_semantic *semantics;
	volatile TegraGM20BAbiFenceSemantic *semantic_public;
	uint32_t failed[kFailedKept];
	uint32_t failed_count;
	switch_gpu_timeline_syncpt syncpt;
};

namespace switch_gpu_timelines
{
extern IOLock *lock;

// lock held
switch_gpu_timeline *find(const switch_gpu_backing_token *token);
bool failed_locked(const switch_gpu_timeline *timeline, uint32_t value);
int status_locked(const switch_gpu_timeline *timeline, uint32_t value, bool external);
void put_locked(switch_gpu_timeline *timeline);

// Syncpoints of timelines whose last reference went (put_locked), for the bridge to hand to new ones
// (switch_gpu_timeline_syncpt_reclaim), each with the value its channel's increments promised. The timeline closes after
// the channel's hardware is gone (chan_free), so none of its increments can land afterwards: the bridge makes the ones
// that never ran before the next owner reads its base.
struct switch_gpu_returned_syncpt {
	uint32_t id, promised;
};

inline switch_gpu_returned_syncpt syncpt_returned[SWITCH_GPU_TIMELINE_SYNCPTS];
inline uint32_t syncpt_returned_count;

inline void syncpt_return_locked(const switch_gpu_timeline *timeline)
{
	if (timeline->syncpt.id && syncpt_returned_count < SWITCH_GPU_TIMELINE_SYNCPTS)
		syncpt_returned[syncpt_returned_count++] = {
			timeline->syncpt.id, timeline->syncpt.base + timeline->syncpt.increments};
}

int physical_now(const switch_gpu_timeline *timeline, uint32_t value, bool external);
}  // namespace switch_gpu_timelines

// The engine's switch_gpu_backing_timeline_refs (SwitchGpuTimelineRefs.cpp)
int switch_gpu_timeline_retain_refs(const switch_gpu_backing_token *token, uint32_t count,
				    const void **handle);
void switch_gpu_timeline_release_refs(const switch_gpu_backing_token *token, uint32_t count);
int switch_gpu_timeline_held_status(const void *handle, const switch_gpu_backing_token *token);
