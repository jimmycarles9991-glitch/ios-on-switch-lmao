// SPDX-License-Identifier: MIT
// Reads of a timeline a reference is held on, and references taken many at a time: a job's records on its uses cost the
// timeline lock once a job, not once a use.
#include <IOKit/IOLib.h>
#include "SwitchGpuTimelineInternal.h"

using namespace switch_gpu_timelines;

int switch_gpu_timeline_retain_refs(const switch_gpu_backing_token *token, uint32_t count,
				    const void **handle)
{
	IOLockLock(lock);
	auto *timeline = find(token);
	if (timeline)
		timeline->refs += count;
	IOLockUnlock(lock);
	if (!timeline)
		return -19;
	*handle = timeline;
	return 0;
}

void switch_gpu_timeline_release_refs(const switch_gpu_backing_token *token, uint32_t count)
{
	if (!count)
		return;
	IOLockLock(lock);
	if (auto *timeline = find(token)) {
		timeline->refs -= count - 1;
		put_locked(timeline);
	}
	IOLockUnlock(lock);
}

// Semantic nodes are reserved before a job's token is committed to its writes (SwitchGpuTimeline.h), so with none and no
// failure on the timeline the status is its physical one for every token a record holds.
int switch_gpu_timeline_held_status(const void *handle, const switch_gpu_backing_token *token)
{
	const auto *timeline = static_cast<const switch_gpu_timeline *>(handle);
	const uint32_t value = uint32_t(token->value);
	if (!__atomic_load_n(&timeline->semantics, __ATOMIC_ACQUIRE) &&
	    !__atomic_load_n(&timeline->failed_count, __ATOMIC_ACQUIRE))
		return physical_now(timeline, value, false);
	IOLockLock(lock);
	const int result = status_locked(timeline, value, false);
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_timeline_view_open(const switch_gpu_backing_token *token,
					     switch_gpu_timeline_view *view)
{
	*view = {};
	if (!lock)
		return -19;
	IOLockLock(lock);
	const auto *timeline = find(token);
	const uint32_t value = timeline ? uint32_t(token->value) : 0;
	int result = !timeline ? -19 : failed_locked(timeline, value) ? -5 : 0;
	for (auto *node = result ? nullptr : timeline->semantics; node; node = node->next)
		if (int32_t(value - node->seqno) >= 0)
			result = -16;
	if (!result)
		*view = {timeline, value};
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_timeline_view_visible(const switch_gpu_timeline_view *view)
{
	return view->timeline ? physical_now(view->timeline, view->value, true) : -19;
}
