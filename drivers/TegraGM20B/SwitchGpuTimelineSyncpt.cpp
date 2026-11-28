// SPDX-License-Identifier: MIT
// A GM20B channel's own host1x syncpoint (host1x.md A2). Once a host1x consumer has asked for one, every job whose end
// drains the channel increments it right after that drain and ahead of the job's fence release (gpu_exec.c
// chan_fence_job), so the consumer's stream waits on the producer itself and the bridge makes no GPU job of its own. Only a token inside a draining job is answered: a token of a job that did
// not drain would wait on a later job, which can itself wait on work that comes after the consumer.
#include <IOKit/IOLib.h>
#include "SwitchGpuTimelineInternal.h"
#include "TegraGM20BSyncpt.h"

using namespace switch_gpu_timelines;

namespace
{
struct Mark {
	uint32_t first, last, threshold;
};

// A mark the submitter rewrote meanwhile is skipped: it is a newer job's, not this token's
bool read_mark(const switch_gpu_syncpt_mark &m, Mark &out)
{
	const uint32_t version = __atomic_load_n(&m.version, __ATOMIC_ACQUIRE);
	if (!version || (version & 1))
		return false;
	out.first = __atomic_load_n(&m.first, __ATOMIC_RELAXED);
	out.last = __atomic_load_n(&m.last, __ATOMIC_RELAXED);
	out.threshold = __atomic_load_n(&m.threshold, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	return __atomic_load_n(&m.version, __ATOMIC_RELAXED) == version;
}

bool job_of(const switch_gpu_timeline_syncpt &syncpt, uint32_t value, Mark &found)
{
	for (const switch_gpu_syncpt_mark &slot : syncpt.marks) {
		Mark m{};
		if (read_mark(slot, m) && int32_t(value - m.first) >= 0 &&
		    int32_t(m.last - value) >= 0) {
			found = m;
			return true;
		}
	}
	return false;
}
}  // namespace

extern "C" int switch_gpu_timeline_has_syncpt(const switch_gpu_timeline *timeline)
{
	return timeline && __atomic_load_n(&timeline->syncpt.id, __ATOMIC_ACQUIRE) != 0;
}

extern "C" uint32_t switch_gpu_timeline_syncpt_next(switch_gpu_timeline *timeline, uint32_t first,
						    uint32_t last)
{
	auto &syncpt = timeline->syncpt;
	const uint32_t id = __atomic_load_n(&syncpt.id, __ATOMIC_ACQUIRE);
	if (!id)
		return 0;
	syncpt.increments += TGM_SYNCPT_INCRS;
	switch_gpu_syncpt_mark &m =
		syncpt.marks[syncpt.increments / TGM_SYNCPT_INCRS % kSyncptMarks];
	const uint32_t version = __atomic_load_n(&m.version, __ATOMIC_RELAXED);
	__atomic_store_n(&m.version, version + 1, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&m.first, first, __ATOMIC_RELAXED);
	__atomic_store_n(&m.last, last, __ATOMIC_RELAXED);
	__atomic_store_n(&m.threshold, syncpt.base + syncpt.increments, __ATOMIC_RELAXED);
	__atomic_store_n(&m.version, version + 2, __ATOMIC_RELEASE);
	return id;
}

extern "C" int switch_gpu_timeline_syncpoint(const switch_gpu_backing_token *token, uint32_t *id,
					     uint32_t *threshold, uint32_t *last)
{
	if (!lock)
		return -19;
	IOLockLock(lock);
	const auto *timeline = find(token);
	const uint32_t value = uint32_t(token->value);
	int result = !timeline ? -19 : !timeline->syncpt.id ? -61 : 1;
	Mark job{};
	if (result == 1 && (__atomic_load_n(&timeline->terminal, __ATOMIC_ACQUIRE) ||
			    !job_of(timeline->syncpt, value, job) ||
			    failed_locked(timeline, value) || failed_locked(timeline, job.last)))
		result = 0;
	for (auto *node = result == 1 ? timeline->semantics : nullptr; node; node = node->next)
		if (int32_t(job.last - node->seqno) >= 0)
			result = 0;
	if (result == 1) {
		*id = timeline->syncpt.id;
		*threshold = job.threshold;
		*last = job.last;
	}
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_timeline_covered(const switch_gpu_backing_token *token, uint32_t last,
					   int *stopped)
{
	*stopped = 1;
	if (!lock)
		return -19;
	IOLockLock(lock);
	const auto *timeline = find(token);
	int result = -19;
	if (timeline) {
		// After the status: a kill stores `terminal` (no lock) before anything reads its -5, so a -5 here comes with
		// `stopped`
		result = physical_now(timeline, last, false);
		*stopped = __atomic_load_n(&timeline->terminal, __ATOMIC_ACQUIRE) != 0;
	}
	IOLockUnlock(lock);
	return result;
}

extern "C" int switch_gpu_timeline_syncpt_adopt(const switch_gpu_backing_token *token, uint32_t id,
						uint32_t base)
{
	IOLockLock(lock);
	auto *timeline = find(token);
	const int result = !timeline ? -19 : timeline->syncpt.id ? -16 : 0;
	if (!result) {
		timeline->syncpt.base = base;
		__atomic_store_n(&timeline->syncpt.id, id, __ATOMIC_RELEASE);
	}
	IOLockUnlock(lock);
	return result;
}

extern "C" uint32_t switch_gpu_timeline_syncpt_reclaim(uint32_t *ids, uint32_t *promised,
						       uint32_t max)
{
	IOLockLock(lock);
	uint32_t taken = 0;
	for (; taken < max && syncpt_returned_count; ++taken) {
		const switch_gpu_returned_syncpt returned =
			syncpt_returned[--syncpt_returned_count];
		ids[taken] = returned.id;
		promised[taken] = returned.promised;
	}
	IOLockUnlock(lock);
	return taken;
}
