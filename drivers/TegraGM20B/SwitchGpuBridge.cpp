// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include "SwitchGpuBridgeQueue.h"
#include "SwitchGpuBridgeProducer.h"
#include "SwitchGpuTimeline.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BSyncpt.h"
#include "../TegraHost1x/TegraHost1x.h"

namespace switch_gpu_bridge_queue
{
BridgeQueue queue;

void releaseLocked(switch_gpu_bridge &job)
{
	if (--job.refs)
		return;
	for (uint32_t i = 0; i < job.count; ++i)
		switch_gpu_backing_token_release(&job.tokens[i]);
	job.count = 0;
	IOLockWakeup(queue.lock, &queue.jobs, false);
}

int producers(const switch_gpu_bridge &job)
{
	bool pending = false;
	for (uint32_t i = 0; i < job.count; ++i) {
		const auto &token = job.tokens[i];
		const int result = switch_gpu_backing_token_status(&token);
		if (result < 0)
			return result;
		if (!result) {
			// VIC and NVDEC publish physical cancellation before retirement sets status.
			if (token.engine == SWITCH_GPU_ENGINE_VIC ||
			    token.engine == SWITCH_GPU_ENGINE_NVDEC) {
				const int physical =
					switch_gpu_backing_token_physical_status(&token, 0);
				if (physical < 0)
					return physical;
			}
			pending = true;
		}
	}
	return pending ? 0 : 1;
}

// Read after the caller's read of `cover`, so a covering job seen dead is judged by the increments it made before it
// died (settleJobless has why they decide it)
bool coveringDrained(const switch_gpu_bridge &job, int source, int cover)
{
	return cover == 1 || (source == 1 &&
			      int32_t(tegra_host1x_syncpt_read(job.producer) - job.threshold) >= 0);
}
}  // namespace switch_gpu_bridge_queue

using namespace switch_gpu_bridge_queue;

namespace
{
void completed(void *)
{
	thread_call_enter(queue.retirement);
}

bool initialize()
{
	if (__atomic_load_n(&queue.lock, __ATOMIC_ACQUIRE))
		return true;
	thread_call_t retirement = thread_call_allocate_with_options(
		retire, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	thread_call_t hangCheck = thread_call_allocate_with_options(
		checkHang, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	IOLock *lock = retirement && hangCheck ? IOLockAlloc() : nullptr;
	if (!lock) {
		if (retirement)
			thread_call_free(retirement);
		if (hangCheck)
			thread_call_free(hangCheck);
		return false;
	}
	thread_call_t noCall = nullptr;
	if (!__atomic_compare_exchange_n(&queue.retirement, &noCall, retirement, false,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		thread_call_free(retirement);
	noCall = nullptr;
	if (!__atomic_compare_exchange_n(&queue.hangCheck, &noCall, hangCheck, false,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		thread_call_free(hangCheck);
	IOLock *noLock = nullptr;
	if (!__atomic_compare_exchange_n(&queue.lock, &noLock, lock, false, __ATOMIC_RELEASE,
					 __ATOMIC_ACQUIRE))
		IOLockFree(lock);
	return true;
}

// A syncpoint stays with its slot across a failed open, as host1x hands none back.
// Releases queue.lock via queue.turn across tgm_chan_del / openSpace / openChannel (each enters tgm_sc7_gate) so
// gatePass and retirement can still acquire queue.lock during sleep or rail-down drain.
int channel()
{
	if (queue.space.channel && !tgm_chan_error(queue.space.channel))
		return 0;
	if (queue.space.channel) {
		for (const auto &job : queue.jobs)
			if (job.refs && !job.producer)
				return -16;
	}
	if (!queue.space.vm) {
		for (auto &job : queue.jobs)
			if (!job.syncpoint && tegra_host1x_syncpt_alloc(&job.syncpoint))
				return -12;
	}
	tgm_chan *oldChannel = queue.space.channel;
	queue.space.channel = nullptr;
	tgm_chan *newChannel = nullptr;
	const int error = queue.turn.waitForRoom(queue.lock, [&] {
		if (oldChannel)
			tgm_chan_del(&oldChannel);
		if (!queue.space.vm &&
		    (!queue.space.openSpace(kShape, &queue) || !gateOpen(queue.space.vm))) {
			queue.space.discardSpace(kShape);
			return -12;
		}
		return queue.space.openChannel(kShape, &queue, completed, &newChannel);
	});
	if (!error)
		queue.space.channel = newChannel;
	return error;
}
}  // namespace

// `assigns`: a host1x consumer's submit, which gives a channel without a syncpoint one for its next jobs; a CPU wait
// or the display's fence never waits on a syncpoint, so it takes none from them
// `watched`: switch_gpu_bridge_submit_watched's, taken under this lock
static int submitBridge(const switch_gpu_backing_token *tokens, uint32_t count,
			switch_gpu_bridge **out, bool assigns, bool watched = false)
{
	if (!out || !tokens || !count || count > SWITCH_GPU_BRIDGE_MAX_TOKENS)
		return -22;
	*out = nullptr;
	for (uint32_t i = 0; i < count; ++i) {
		const bool promise = tokens[i].engine == SWITCH_GPU_ENGINE_PROMISE;
		if ((tokens[i].engine != SWITCH_GPU_ENGINE_GM20B &&
		     tokens[i].engine != SWITCH_GPU_ENGINE_VIC &&
		     tokens[i].engine != SWITCH_GPU_ENGINE_NVDEC &&
		     tokens[i].engine != SWITCH_GPU_ENGINE_DISPLAY &&
		     tokens[i].engine != SWITCH_GPU_ENGINE_NVENC && !promise) ||
		    (!tokens[i].reset_generation && !promise) ||
		    ((tokens[i].engine == SWITCH_GPU_ENGINE_GM20B || promise) &&
		     tokens[i].value > UINT32_MAX))
			return -22;
	}
	if (!initialize())
		return -12;
	SwitchGpuProducerWait wait;
	const bool jobless = switch_gpu_producer_wait(tokens, count, wait);
	if (jobless)
		tgm_chan_need_tokens(&wait.covering, 1);
	IOLockLock(queue.lock);
	if (assigns)
		switch_gpu_producer_assign(wait);
	if (!jobless)
		queue.turn.enter(queue.lock);
	int error = jobless ? 0 : channel();
	uint32_t slot = 0;
	while (slot < kSlots && queue.jobs[slot].refs)
		++slot;
	if (!error && !jobless && slot < kSlots && !queue.jobs[slot].syncpoint &&
	    tegra_host1x_syncpt_alloc(&queue.jobs[slot].syncpoint))
		error = -12;
	if (error || slot == kSlots) {
		IOLockUnlock(queue.lock);
		return error ? error : -16;
	}
	auto &job = queue.jobs[slot];
	job.refs = 1;
	__atomic_store_n(&job.result, 0, __ATOMIC_RELAXED);
	job.producer = jobless ? wait.syncpoint : 0;
	bool producerDone = false;
	for (uint32_t i = 0; i < count; ++i) {
		error = switch_gpu_backing_token_retain(&tokens[i]);
		if (error)
			break;
		job.tokens[job.count++] = tokens[i];
	}
	if (!error) {
		// Ahead of the check: a producer that completes after it finds the flag and has retirement look
		if (jobless)
			__atomic_store_n(&queue.waitingSources, 1u, __ATOMIC_SEQ_CST);
		const int source = producers(job);
		if (source < 0)
			error = source;
	}
	bool abandoned = false;
	if (!error && jobless) {
		job.threshold = wait.threshold;
		job.covering = wait.covering;
		int stopped = 0;
		producerDone = switch_gpu_producer_covered(job.covering, &stopped) != 0;
	} else if (!error) {
		tgm_chan *const chan = queue.space.channel;
		queue.turn.waitForRoom(queue.lock, [&] {
			switch_gpu_chan_submit_enter(chan);
			return 0;
		});
		const int entered = producers(job);
		if (entered < 0) {
			error = entered;
		} else {
			job.threshold = tegra_host1x_syncpt_read(job.syncpoint) + TGM_SYNCPT_INCRS;
			const tgm_push push = {
				kPushVa + slot * kPage, slotPush(job, slot), 0, 0, 0, 0};
			for (;;) {
				// No L2 invalidate ahead of it: the job reads no memory, its push slots are volatile, and the push's own
				// flush (tgm_syncpt_incr) puts the producers' writes in DRAM before host1x sees the increment.
				error = switch_gpu_chan_exec_visibility(
					chan, &push, 1, job.tokens, job.count, &job.sequence,
					nullptr,
					SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT |
						SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY);
				if (error != -11)
					break;
				error = queue.turn.waitForRoom(queue.lock, [&] {
					return switch_gpu_chan_wait_submission_room(
						chan, &push, 1, job.count, nullptr);
				});
				if (error)
					break;
				// waitForRoom dropped queue.lock; gatePass may have already reopened the gate for a producer
				// that failed meanwhile.
				const int source = producers(job);
				if (source < 0) {
					error = source;
					break;
				}
			}
		}
		switch_gpu_chan_submit_leave();
		// If a producer failed while exec_visibility ran, backing_unsettled may have skipped its acquire before
		// gate_close closed the gate. Keep the slot until the kicked GPU job retires, and hand no bridge out.
		if (!error) {
			const int source = producers(job);
			if (source < 0) {
				error = source;
				abandoned = true;
			}
		}
	}
#if TGM_DEBUG_PROBES
	const bool consumerJobless = !error && jobless && assigns;
#endif
	if (abandoned) {
		__atomic_store_n(&job.result, error, __ATOMIC_RELEASE);
		job.active = true;
		thread_call_enter(queue.retirement);
	} else if (error) {
		releaseLocked(job);
	} else {
		job.active = true;
		++job.refs;
		if (watched) {
			job.watched = true;
			++job.waiters;
		}
		*out = &job;
		// A check that found the channel idle left the timer down, and a waiter may still be on a bridge whose GPU
		// work is done: this job is new work on the channel that waiter's checks cover.
		if (!queue.checkAt && anyWaited())
			armHangCheck(kHangCheckNs);
		if (jobless && producerDone)
			thread_call_enter(queue.retirement);
	}
	IOLockUnlock(queue.lock);
#if TGM_DEBUG_PROBES
	if (consumerJobless)
		tgm_chan_note_jobless(wait.covering.queue);
#endif
	return error;
}

extern "C" int switch_gpu_bridge_submit(const switch_gpu_backing_token *tokens, uint32_t count,
					switch_gpu_bridge **out)
{
	return submitBridge(tokens, count, out, true);
}

extern "C" int switch_gpu_bridge_submit_watched(const switch_gpu_backing_token *tokens,
						uint32_t count, switch_gpu_bridge **out)
{
	return submitBridge(tokens, count, out, true, true);
}

extern "C" void switch_gpu_bridge_syncpoint(const switch_gpu_bridge *bridge, uint32_t *syncpoint,
					    uint32_t *threshold)
{
	*syncpoint = bridge->producer ? bridge->producer : bridge->syncpoint;
	*threshold = bridge->threshold;
}

extern "C" int switch_gpu_bridge_jobless(const switch_gpu_bridge *bridge)
{
	return bridge && bridge->producer;
}

extern "C" int switch_gpu_bridge_failed(const switch_gpu_bridge *bridge)
{
	const int result = bridge ? __atomic_load_n(&bridge->result, __ATOMIC_ACQUIRE) : -22;
	return result < 0 ? result : 0;
}

extern "C" int switch_gpu_bridge_covers(const switch_gpu_bridge *bridge,
					const switch_gpu_backing_token *token)
{
	if (!bridge || !token)
		return -22;
	const int source = switch_gpu_backing_token_external_status(token);
	if (source)
		return source;
	for (uint32_t i = 0; i < bridge->count; ++i) {
		const auto &held = bridge->tokens[i];
		if (token->engine == held.engine && token->queue == held.queue &&
		    token->reset_generation == held.reset_generation &&
		    (held.engine == SWITCH_GPU_ENGINE_GM20B ||
				     held.engine == SWITCH_GPU_ENGINE_PROMISE
			     ? int32_t(uint32_t(held.value - token->value)) >= 0
			     : held.value >= token->value))
			return 1;
	}
	return 0;
}

extern "C" void switch_gpu_bridge_completed(void)
{
	if (__atomic_load_n(&queue.waitingSources, __ATOMIC_ACQUIRE))
		thread_call_enter(queue.retirement);
}

extern "C" int switch_gpu_bridge_source_failure(const switch_gpu_bridge *bridge)
{
	for (uint32_t i = 0; i < bridge->count; ++i) {
		const auto &token = bridge->tokens[i];
		if (token.engine != SWITCH_GPU_ENGINE_GM20B &&
		    token.engine != SWITCH_GPU_ENGINE_PROMISE)
			continue;
		const int result = switch_gpu_backing_token_physical_status(&token, 0);
		if (result < 0)
			return result;
	}
	return 0;
}

extern "C" int switch_gpu_bridge_why(const switch_gpu_bridge *bridge)
{
	const int why = switch_gpu_backing_tokens_failure(bridge->tokens, bridge->count);
	// A jobless bridge also fails when every token completed and its covering job died before its drain
	const bool coveringFailed =
		bridge->producer && __atomic_load_n(&bridge->result, __ATOMIC_ACQUIRE) < 0;
	return why || !coveringFailed ? why
				      : switch_gpu_backing_tokens_failure(&bridge->covering, 1);
}

extern "C" int switch_gpu_bridge_status(const switch_gpu_bridge *bridge)
{
	if (!bridge)
		return -22;
	IOLockLock(queue.lock);
	int result = bridge->result;
	if (!result) {
		result = producers(*bridge);
		if (bridge->producer && result >= 0) {
			int stopped = 0;
			const int cover = switch_gpu_producer_covered(bridge->covering, &stopped);
			if (cover == 0)
				result = 0;
			else if (!coveringDrained(*bridge, result, cover))
				result = cover;
		}
		const uint32_t syncpoint = bridge->producer ? bridge->producer : bridge->syncpoint;
		if (result == 1 &&
		    int32_t(tegra_host1x_syncpt_read(syncpoint) - bridge->threshold) < 0)
			result = 0;
	}
	IOLockUnlock(queue.lock);
	return result;
}

extern "C" int switch_gpu_bridge_wait(const switch_gpu_bridge *bridge)
{
	if (!bridge)
		return -22;
	auto *job = const_cast<switch_gpu_bridge *>(bridge);
	IOLockLock(queue.lock);
	if (job->active) {
		++job->waiters;
		if (!queue.checkAt)
			armHangCheck(kHangCheckNs);
		while (job->active)
			IOLockSleep(queue.lock, job, THREAD_UNINT);
		--job->waiters;
	}
	const int result = job->result;
	IOLockUnlock(queue.lock);
	return result;
}

extern "C" int switch_gpu_bridge_notify(const switch_gpu_bridge *bridge,
					void (*completed)(void *, int), void *context)
{
	if (!bridge || !completed)
		return -22;
	auto *job = const_cast<switch_gpu_bridge *>(bridge);
	IOLockLock(queue.lock);
	if (job->completed) {
		IOLockUnlock(queue.lock);
		return -16;
	}
	job->completed = completed;
	job->context = context;
	++job->refs;
	if (job->active) {
		++job->waiters;
		job->notified = true;
		if (!queue.checkAt)
			armHangCheck(kHangCheckNs);
	} else {
		thread_call_enter(queue.retirement);
	}
	IOLockUnlock(queue.lock);
	return 0;
}

extern "C" int switch_gpu_bridge_watch(const switch_gpu_bridge *bridge)
{
	if (!bridge)
		return -22;
	auto *job = const_cast<switch_gpu_bridge *>(bridge);
	IOLockLock(queue.lock);
	if (job->active && !job->watched) {
		++job->waiters;
		job->watched = true;
		if (!queue.checkAt)
			armHangCheck(kHangCheckNs);
	}
	IOLockUnlock(queue.lock);
	return 0;
}

extern "C" void switch_gpu_bridge_release(switch_gpu_bridge **bridge)
{
	if (!bridge || !*bridge)
		return;
	IOLockLock(queue.lock);
	releaseLocked(**bridge);
	*bridge = nullptr;
	IOLockUnlock(queue.lock);
}

extern "C" int switch_gpu_bridge_wait_room(void)
{
	if (!initialize())
		return -12;
	IOLockLock(queue.lock);
	for (;;) {
		uint32_t held = 0;
		uint32_t heldJobs = 0;
		for (const auto &job : queue.jobs) {
			held += job.refs != 0;
			heldJobs += job.refs != 0 && !job.producer;
		}
		const bool failed = queue.space.channel && tgm_chan_error(queue.space.channel);
		if (failed ? heldJobs == 0 : held < kSlots)
			break;
		IOLockSleep(queue.lock, &queue.jobs, THREAD_UNINT);
	}
	IOLockUnlock(queue.lock);
	return 0;
}

extern "C" int switch_gpu_bridge_fence(const switch_gpu_backing_token *tokens, uint32_t count,
				       void (*callback)(void *, int), void *context)
{
	if (!callback)
		return -22;
	switch_gpu_bridge *bridge = nullptr;
	const int error = submitBridge(tokens, count, &bridge, false);
	if (error)
		return error;
	IOLockLock(queue.lock);
	bridge->completed = callback;
	bridge->context = context;
	++bridge->refs;
	thread_call_enter(queue.retirement);
	IOLockUnlock(queue.lock);
	switch_gpu_bridge_release(&bridge);
	return 0;
}

extern "C" int switch_gpu_bridge_wait_external(const switch_gpu_backing_token *token)
{
	switch_gpu_bridge *bridge = nullptr;
	int result;
	for (;;) {
		result = submitBridge(token, 1, &bridge, false);
		if (result != -16)
			break;
		result = switch_gpu_bridge_wait_room();
		if (result)
			return result;
	}
	if (result)
		return result;
	result = switch_gpu_bridge_wait(bridge);
	switch_gpu_bridge_release(&bridge);
	return result;
}
