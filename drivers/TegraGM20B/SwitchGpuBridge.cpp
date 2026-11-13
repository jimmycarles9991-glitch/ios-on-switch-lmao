// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include "SwitchGpuBridge.h"
#include "SwitchGpuTimeline.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BSyncpt.h"
#include "SwitchGpuKqueue.h"
#include "../TegraHost1x/TegraHost1x.h"

struct switch_gpu_bridge {
	switch_gpu_backing_token tokens[SWITCH_GPU_BRIDGE_MAX_TOKENS];
	uint32_t count, refs, syncpoint, threshold, sequence, waiters;
	bool active, notified;
	int result;
	void (*completed)(void *, int);
	void *context;
};

namespace
{
constexpr uint32_t kSlots = 8, kHangCheckNs = 1000000000;
constexpr uint64_t kPage = 4096, kPushVa = 1ULL << 20;
constexpr SwitchGpuKqueueShape kShape = {
	"host1x-bridge", TGM_FENCE_KERNEL_BRIDGE, kSlots, kPushVa, 1, 0};

struct BridgeQueue {
	IOLock *lock;
	thread_call_t retirement, hangCheck;
	uint64_t checkAt;
	SwitchGpuKqueue space;
	uint32_t waitingSources;
	switch_gpu_bridge jobs[kSlots];
} queue;

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
		const int result = switch_gpu_backing_token_status(&job.tokens[i]);
		if (result < 0)
			return result;
		pending |= result == 0;
	}
	return pending ? 0 : 1;
}

switch_gpu_bridge *oldestWaited()
{
	switch_gpu_bridge *oldest = nullptr;
	for (auto &job : queue.jobs)
		if (job.active && job.waiters &&
		    (!oldest || int32_t(job.sequence - oldest->sequence) < 0))
			oldest = &job;
	return oldest;
}

void armHangCheck(uint64_t ns)
{
	clock_interval_to_deadline(uint32_t(ns / 1000), 1000, &queue.checkAt);
	thread_call_enter_delayed(queue.hangCheck, queue.checkAt);
}

// The GPU's hang recovery runs only when something waits on a channel, and a wait on a bridge is that waiter for the
// bridge channel: it has recovery look at what a stalled bridge is stuck on, whose kill releases the bridge's acquire.
// One timer does it, armed when a wait begins, cancelled when no waited bridge is left (retire), and re-armed only for
// the next step of tgm_chan_check_progress's rule. The reference keeps the channel across the unlocked check.
void checkHang(thread_call_param_t, thread_call_param_t)
{
	IOLockLock(queue.lock);
	queue.checkAt = 0;
	switch_gpu_bridge *late = queue.space.channel ? oldestWaited() : nullptr;
	if (!late) {
		IOLockUnlock(queue.lock);
		return;
	}
	++late->refs;
	tgm_chan *channel = queue.space.channel;
	const uint32_t sequence = late->sequence;
	IOLockUnlock(queue.lock);
	const uint64_t next = tgm_chan_check_progress(channel, "host1x bridge", sequence);
	IOLockLock(queue.lock);
	if (next && !queue.checkAt && oldestWaited())
		armHangCheck(next);
	releaseLocked(*late);
	IOLockUnlock(queue.lock);
}

void retire(thread_call_param_t, thread_call_param_t)
{
	struct Completion {
		switch_gpu_bridge *job;
		void (*fn)(void *, int);
		void *context;
		int result;
	};

	Completion callbacks[kSlots];
	uint32_t count = 0;
	IOLockLock(queue.lock);
	__atomic_store_n(&queue.waitingSources, 1u, __ATOMIC_RELEASE);
	bool pendingSources = false;
	int failed = 0;
	const uint32_t completed =
		queue.space.channel ? tgm_chan_settled(queue.space.channel, &failed) : 0;
	for (auto &job : queue.jobs) {
		if (!job.active || (!failed && int32_t(completed - job.sequence) < 0))
			continue;
		const int source = producers(job);
		if (!failed && !source) {
			pendingSources = true;
			continue;
		}
		job.result = failed ? -5 : source == 1 ? 1 : source < 0 ? source : -5;
		if (failed) {
			const int released = tegra_host1x_syncpt_release_failed(
				job.syncpoint, job.threshold, TGM_SYNCPT_INCRS);
			// Whatever waits on the old syncpoint fails or times out on its own engine; this slot no longer shares it.
			if (released) {
				IOLog("SwitchGpuBridge: syncpoint %u of a failed job stays short "
				      "of %u "
				      "(%d); its slot takes "
				      "another\n",
				      job.syncpoint, job.threshold, released);
				job.syncpoint = 0;
			}
		}
		if (job.result == 1)
			for (uint32_t i = 0; i < job.count; ++i)
				if (job.tokens[i].engine == SWITCH_GPU_ENGINE_GM20B)
					(void)switch_gpu_timeline_promote(&job.tokens[i]);
		job.active = false;
		if (job.notified) {
			job.notified = false;
			--job.waiters;
		}
		IOLockWakeup(queue.lock, &job, false);
		releaseLocked(job);
	}
	for (auto &job : queue.jobs) {
		if (!job.active && job.completed) {
			callbacks[count++] = {&job, job.completed, job.context, job.result};
			job.completed = nullptr;
		}
	}
	if (queue.checkAt && !oldestWaited()) {
		thread_call_cancel(queue.hangCheck);
		queue.checkAt = 0;
	}
	__atomic_store_n(&queue.waitingSources, pendingSources, __ATOMIC_RELEASE);
	IOLockUnlock(queue.lock);
	for (uint32_t i = 0; i < count; ++i) {
		const auto &callback = callbacks[i];
		callback.fn(callback.context, callback.result == 1 ? 0 : callback.result);
		IOLockLock(queue.lock);
		releaseLocked(*callback.job);
		IOLockUnlock(queue.lock);
	}
}

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
bool openSpace()
{
	for (auto &job : queue.jobs)
		if (!job.syncpoint && tegra_host1x_syncpt_alloc(&job.syncpoint))
			return false;
	return queue.space.openSpace(kShape, &queue);
}

int channel()
{
	if (queue.space.channel && tgm_chan_error(queue.space.channel)) {
		for (const auto &job : queue.jobs)
			if (job.refs)
				return -16;
		tgm_chan_del(&queue.space.channel);
	}
	if (!queue.space.vm && !openSpace()) {
		queue.space.discardSpace(kShape);
		return -12;
	}
	return queue.space.openChannel(kShape, &queue, completed);
}
}

extern "C" int switch_gpu_bridge_submit(const switch_gpu_backing_token *tokens, uint32_t count,
					switch_gpu_bridge **out)
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
	IOLockLock(queue.lock);
	int error = channel();
	uint32_t slot = 0;
	while (slot < kSlots && queue.jobs[slot].refs)
		++slot;
	if (!error && slot < kSlots && !queue.jobs[slot].syncpoint &&
	    tegra_host1x_syncpt_alloc(&queue.jobs[slot].syncpoint))
		error = -12;
	if (error || slot == kSlots) {
		IOLockUnlock(queue.lock);
		return error ? error : -16;
	}
	auto &job = queue.jobs[slot];
	job.refs = 1;
	job.result = 0;
	for (uint32_t i = 0; i < count; ++i) {
		error = switch_gpu_backing_token_retain(&tokens[i]);
		if (error)
			break;
		job.tokens[job.count++] = tokens[i];
	}
	if (!error) {
		const int source = producers(job);
		if (source < 0)
			error = source;
	}
	if (!error) {
		job.threshold = tegra_host1x_syncpt_read(job.syncpoint) + TGM_SYNCPT_INCRS;
		const tgm_push push = {
			kPushVa + slot * kPage,
			tgm_syncpt_incr(queue.space.words + slot * (kPage / 4), job.syncpoint),
			0,
			0,
			0,
			0};
		switch_gpu_chan_submit_enter(queue.space.channel);
		for (;;) {
			// No L2 invalidate ahead of it: the job reads no memory, its push slots are volatile, and the push's own
			// flush (tgm_syncpt_incr) puts the producers' writes in DRAM before host1x sees the increment.
			error = switch_gpu_chan_exec_visibility(
				queue.space.channel, &push, 1, job.tokens, job.count, &job.sequence,
				nullptr,
				SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT |
					SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY);
			if (error != -11)
				break;
			error = switch_gpu_chan_wait_submission_room(queue.space.channel, &push, 1,
								     job.count, nullptr);
			if (error)
				break;
		}
		switch_gpu_chan_submit_leave();
	}
	if (error) {
		releaseLocked(job);
	} else {
		job.active = true;
		++job.refs;
		*out = &job;
		// A check that found the channel idle left the timer down, and a waiter may still be on a bridge whose GPU
		// work is done: this job is new work on the channel that waiter's checks cover.
		if (!queue.checkAt && oldestWaited())
			armHangCheck(kHangCheckNs);
	}
	IOLockUnlock(queue.lock);
	return error;
}

extern "C" void switch_gpu_bridge_syncpoint(const switch_gpu_bridge *bridge, uint32_t *syncpoint,
					    uint32_t *threshold)
{
	*syncpoint = bridge->syncpoint;
	*threshold = bridge->threshold;
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

extern "C" int switch_gpu_bridge_status(const switch_gpu_bridge *bridge)
{
	if (!bridge)
		return -22;
	IOLockLock(queue.lock);
	int result = bridge->result;
	if (!result) {
		result = producers(*bridge);
		if (result == 1 &&
		    int32_t(tegra_host1x_syncpt_read(bridge->syncpoint) - bridge->threshold) < 0)
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
		for (const auto &job : queue.jobs)
			held += job.refs != 0;
		const bool failed = queue.space.channel && tgm_chan_error(queue.space.channel);
		if (failed ? held == 0 : held < kSlots)
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
	const int error = switch_gpu_bridge_submit(tokens, count, &bridge);
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
		result = switch_gpu_bridge_submit(token, 1, &bridge);
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
