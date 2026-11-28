// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <string.h>
#include "SwitchGpuCachedZero.h"
#include "TegraGM20BCopyEmit.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BRelayout.h"
#include "SwitchGpuKqueue.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"

struct switch_gpu_cached_mapping {
	tgm_mem *memory;
	switch_gpu_mapping *mapping;
	uint64_t *pages;
	uint64_t bytes, address, used;
	uint32_t count, slot;
	bool pending, mapped;
};

namespace
{
constexpr uint32_t kJobs = 8;
constexpr uint64_t kPage = 4096, kPushVa = 1ULL << 20, kWindow = 256ULL << 20;
constexpr uint64_t kMappedLimit = 128ULL << 20;

struct Job {
	switch_gpu_cached_mapping *mapping;
	void (*completed)(void *, int);
	void *context;
	void *cpu;
	uint64_t bytes, deadline;
	uint32_t sequence;
	switch_gpu_cached_mapping *srcMapping;
};

constexpr SwitchGpuKqueueShape kShape = {
	"cached-scrub", TGM_FENCE_KERNEL_CACHED_ZERO, 1, kPushVa, 1, 0};

struct QueueSync {
	IOLock *lock;
	thread_call_t retirement, watchdog;
};

struct Queue {
	QueueSync *sync;
	SwitchGpuKqueue space;
	uint64_t mappedBytes, slots, armed, uses;
	switch_gpu_cached_mapping *owners[64];  // by slot: what each mapped window holds
	Job jobs[kJobs];
} queue;

uint64_t earliest()
{
	uint64_t next = 0;
	for (const auto &job : queue.jobs)
		if (job.mapping && job.deadline && (!next || job.deadline < next))
			next = job.deadline;
	return next;
}

void arm()
{
	const uint64_t next = earliest();
	if (next == queue.armed)
		return;
	queue.armed = next;
	if (next)
		thread_call_enter_delayed(queue.sync->watchdog, next);
	else
		thread_call_cancel(queue.sync->watchdog);
}

void retire(thread_call_param_t, thread_call_param_t)
{
	Job finished[kJobs];
	uint32_t count = 0;
	IOLockLock(queue.sync->lock);
	int failed = 0;
	const uint32_t done =
		queue.space.channel ? tgm_chan_settled(queue.space.channel, &failed) : 0;
	for (auto &job : queue.jobs) {
		if (!job.mapping || (!failed && int32_t(done - job.sequence) < 0))
			continue;
		if (!job.srcMapping)
			job.mapping->pending = false;
		finished[count++] = job;
		job = {};
	}
	arm();
	if (count)
		IOLockWakeup(queue.sync->lock, &queue.jobs, false);
	IOLockUnlock(queue.sync->lock);
	for (uint32_t i = 0; i < count; ++i) {
		if (finished[i].cpu)
			switch_dcache_invalidate(finished[i].cpu, size_t(finished[i].bytes));
		finished[i].completed(finished[i].context, failed ? -5 : 0);
	}
}

// A scrub not back a second after it was queued has the GPU's hang recovery look at its channel; later checks come only
// when tgm_chan_check_progress's rule can take its next step. A job with no deadline has no check due.
void timeout(thread_call_param_t, thread_call_param_t)
{
	IOLockLock(queue.sync->lock);
	queue.armed = 0;
	const uint64_t now = mach_absolute_time();
	const uint32_t done = queue.space.channel ? tgm_chan_completed(queue.space.channel) : 0;
	uint32_t late = 0;
	for (const auto &job : queue.jobs)
		if (job.mapping && job.deadline && job.deadline <= now &&
		    int32_t(done - job.sequence) < 0 && (!late || int32_t(job.sequence - late) < 0))
			late = job.sequence;
	const uint64_t next =
		late ? tgm_chan_check_progress(queue.space.channel, "cached scrub", late) : 0;
	uint64_t deadline = 0;
	if (next)
		clock_interval_to_deadline(uint32_t(next / 1000), 1000, &deadline);
	for (auto &job : queue.jobs)
		if (job.mapping && job.deadline && job.deadline <= now)
			job.deadline = int32_t(done - job.sequence) < 0 ? deadline : 0;
	arm();
	IOLockUnlock(queue.sync->lock);
	thread_call_enter(queue.sync->retirement);
}

void fenced(void *)
{
	thread_call_enter(queue.sync->retirement);
}

bool initialize()
{
	if (__atomic_load_n(&queue.sync, __ATOMIC_ACQUIRE))
		return true;
	auto *made = IONew(QueueSync, 1);
	if (!made)
		return false;
	*made = {};
	made->lock = IOLockAlloc();
	made->retirement = thread_call_allocate_with_options(
		retire, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	made->watchdog = thread_call_allocate_with_options(
		timeout, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (!made->lock || !made->retirement || !made->watchdog) {
		if (made->lock)
			IOLockFree(made->lock);
		if (made->retirement)
			thread_call_free(made->retirement);
		if (made->watchdog)
			thread_call_free(made->watchdog);
		IODelete(made, QueueSync, 1);
		return false;
	}
	QueueSync *expected = nullptr;
	if (!__atomic_compare_exchange_n(&queue.sync, &expected, made, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_ACQUIRE)) {
		thread_call_free(made->watchdog);
		thread_call_free(made->retirement);
		IOLockFree(made->lock);
		IODelete(made, QueueSync, 1);
	}
	return true;
}

int channel()
{
	if (queue.space.channel && tgm_chan_error(queue.space.channel)) {
		for (const auto &job : queue.jobs)
			if (job.mapping)
				return -16;
		tgm_chan_del(&queue.space.channel);
	}
	if (!queue.space.vm && !queue.space.openSpace(kShape, &queue)) {
		queue.space.discardSpace(kShape);
		queue.space.fenceCpu = nullptr;
		queue.space.fenceGpu = queue.space.fencePage = queue.space.pushPa = 0;
		return -12;
	}
	return queue.space.openChannel(kShape, &queue, fenced);
}

void unmapWindow(switch_gpu_cached_mapping *mapping)
{
	if (!mapping->mapped)
		return;
	switch_gpu_vm_unmap(queue.space.vm, &mapping->mapping);
	tgm_mem_del(&mapping->memory);
	queue.mappedBytes -= mapping->bytes;
	queue.slots &= ~(1ULL << mapping->slot);
	queue.owners[mapping->slot] = nullptr;
	mapping->mapped = false;
}

void drop(switch_gpu_cached_mapping *mapping)
{
	unmapWindow(mapping);
	IODelete(mapping->pages, uint64_t, mapping->count);
	IODelete(mapping, switch_gpu_cached_mapping, 1);
}

// Spares keep their window between scrubs; a window nothing is scrubbing goes, least recently used first, when a scrub
// needs its room. Its owner maps again at its next scrub.
bool evictIdle(const switch_gpu_cached_mapping *keep)
{
	switch_gpu_cached_mapping *oldest = nullptr;
	for (auto *owner : queue.owners)
		if (owner && owner != keep && !owner->pending &&
		    (!oldest || owner->used < oldest->used))
			oldest = owner;
	if (oldest)
		unmapWindow(oldest);
	return oldest != nullptr;
}

int mapWindow(switch_gpu_cached_mapping *mapping, int readOnly = 0, int vol = 1)
{
	while (queue.slots == UINT64_MAX || mapping->bytes > kMappedLimit - queue.mappedBytes)
		if (!evictIdle(mapping))
			return -28;
	mapping->slot = uint32_t(__builtin_ctzll(~queue.slots));
	mapping->address = (1ULL << 32) + uint64_t(mapping->slot) * kWindow;
	queue.slots |= 1ULL << mapping->slot;
	queue.owners[mapping->slot] = mapping;
	queue.mappedBytes += mapping->bytes;
	mapping->mapped = true;
	int result = tgm_mem_new(queue.space.vm, mapping->pages, mapping->count, &mapping->memory);
	if (!result)
		result = switch_gpu_vm_map_volatile(queue.space.vm, mapping->address,
						    mapping->bytes, mapping->memory, 0, 0, readOnly,
						    vol, &mapping->mapping);
	if (result)
		unmapWindow(mapping);
	return result;
}

bool hasActiveJob()
{
	for (const auto &job : queue.jobs)
		if (job.mapping)
			return true;
	return false;
}

bool hasActiveZeroJob()
{
	for (const auto &job : queue.jobs)
		if (job.mapping && !job.srcMapping)
			return true;
	return false;
}

int prepare(const uint64_t *pages, uint32_t count, switch_gpu_cached_mapping **out)
{
	const uint64_t bytes = uint64_t(count) << 12;
	if (*out) {
		if ((*out)->count != count ||
		    memcmp((*out)->pages, pages, size_t(count) * sizeof(*pages)))
			return -22;
		(*out)->used = ++queue.uses;
		return (*out)->mapped ? 0 : mapWindow(*out);
	}
	if (bytes > kMappedLimit)
		return -28;
	auto *mapping = IONew(switch_gpu_cached_mapping, 1);
	if (!mapping)
		return -12;
	*mapping = {};
	mapping->pages = IONew(uint64_t, count);
	if (!mapping->pages) {
		IODelete(mapping, switch_gpu_cached_mapping, 1);
		return -12;
	}
	memcpy(mapping->pages, pages, size_t(count) * sizeof(*pages));
	mapping->bytes = bytes;
	mapping->count = count;
	mapping->used = ++queue.uses;
	const int result = mapWindow(mapping);
	if (result) {
		drop(mapping);
		return result;
	}
	*out = mapping;
	return 0;
}
}  // namespace

int switch_gpu_cached_zero(const uint64_t *pages, uint32_t count, void *cpu,
			   switch_gpu_cached_mapping **mapping, void (*completed)(void *, int),
			   void *context)
{
	if (!pages || !count || uint64_t(count) * kPage > kWindow || !mapping || !completed)
		return -22;
	if (!initialize())
		return -12;
	IOLockLock(queue.sync->lock);
	int result = 0;
	unsigned slot = 0;
	for (;;) {
		result = channel();
		if (result == -16 && hasActiveJob() && !hasActiveZeroJob()) {
			IOLockSleep(queue.sync->lock, &queue.jobs, THREAD_UNINT);
			continue;
		}
		if (result)
			break;
		slot = 0;
		while (slot < kJobs && queue.jobs[slot].mapping)
			++slot;
		if (slot == kJobs) {
			if (!hasActiveZeroJob()) {
				IOLockSleep(queue.sync->lock, &queue.jobs, THREAD_UNINT);
				continue;
			}
			result = -16;
		}
		break;
	}
	if (!result)
		result = prepare(pages, count, mapping);
	if (!result && (*mapping)->pending)
		result = -16;
	if (!result) {
		// No task or producer can access this allocation. Discard old cached CPU bytes before CE overwrites it.
		if (cpu)
			switch_dcache_invalidate(cpu, size_t(count) * kPage);
		uint32_t *words = queue.space.words + slot * 32;
		const tgm_push push = {
			kPushVa + slot * 128,
			tgm_relayout_clear(words, (*mapping)->address, uint64_t(count) * kPage),
			0,
			0,
			0,
			0};
		uint32_t sequence = 0;
		result = tgm_chan_exec(queue.space.channel, &push, 1, &sequence, nullptr);
		if (!result) {
			uint64_t deadline = 0;
			clock_interval_to_deadline(1, 1000000000, &deadline);
			(*mapping)->pending = true;
			queue.jobs[slot] = {};
			queue.jobs[slot].mapping = *mapping;
			queue.jobs[slot].completed = completed;
			queue.jobs[slot].context = context;
			queue.jobs[slot].cpu = cpu;
			queue.jobs[slot].bytes = uint64_t(count) * kPage;
			queue.jobs[slot].deadline = deadline;
			queue.jobs[slot].sequence = sequence;
			arm();
		}
	}
	IOLockUnlock(queue.sync->lock);
	return result;
}

void switch_gpu_cached_mapping_drop(switch_gpu_cached_mapping **mapping)
{
	if (!mapping || !*mapping)
		return;
	IOLockLock(queue.sync->lock);
	if ((*mapping)->pending)
		panic("SwitchGpu: cached scrub mapping released before retirement");
	drop(*mapping);
	*mapping = nullptr;
	IOLockUnlock(queue.sync->lock);
}

int switch_gpu_cached_copy(const uint64_t *srcPages, const uint64_t *dstPages, uint32_t count)
{
	const uint64_t bytes = uint64_t(count) * kPage;
	if (!srcPages || !dstPages || !count || bytes > kMappedLimit / 2 || count > 0xffffu)
		return -22;
	if (!initialize())
		return -12;
	switch_gpu_cached_mapping srcMap = {};
	switch_gpu_cached_mapping dstMap = {};
	srcMap.pages = const_cast<uint64_t *>(srcPages);
	srcMap.bytes = bytes;
	srcMap.count = count;
	dstMap.pages = const_cast<uint64_t *>(dstPages);
	dstMap.bytes = bytes;
	dstMap.count = count;

	IOLockLock(queue.sync->lock);
	int result = 0;
	unsigned slot = kJobs;
	for (;;) {
		result = channel();
		if (result == -16 && hasActiveJob()) {
			IOLockSleep(queue.sync->lock, &queue.jobs, THREAD_UNINT);
			continue;
		}
		if (result)
			break;
		slot = 0;
		while (slot < kJobs && queue.jobs[slot].mapping)
			++slot;
		if (slot == kJobs) {
			IOLockSleep(queue.sync->lock, &queue.jobs, THREAD_UNINT);
			continue;
		}
		srcMap.used = ++queue.uses;
		result = mapWindow(&srcMap, 1, 1);
		if (result == -28 && hasActiveJob()) {
			IOLockSleep(queue.sync->lock, &queue.jobs, THREAD_UNINT);
			continue;
		}
		if (result)
			break;
		// Hold srcMap non-evictable while mapping dstMap so evictIdle cannot drop srcMap.
		srcMap.pending = true;
		dstMap.used = ++queue.uses;
		result = mapWindow(&dstMap, 0, 0);
		if (result == -28 && hasActiveJob()) {
			srcMap.pending = false;
			unmapWindow(&srcMap);
			IOLockSleep(queue.sync->lock, &queue.jobs, THREAD_UNINT);
			continue;
		}
		break;
	}
	if (!result) {
		uint32_t *words = queue.space.words + slot * 32;
		const uint32_t pushed =
			tgm_copy_rows(words, srcMap.address, uint32_t(kPage), dstMap.address,
				      uint32_t(kPage), uint32_t(kPage), count);
		const tgm_push push = {kPushVa + slot * 128, pushed, 0, 0, 0, 0};
		uint32_t sequence = 0;
		result = tgm_chan_exec(queue.space.channel, &push, 1, &sequence, nullptr);
		if (!result) {
			struct CopyWait {
				bool done;
				int error;
			};

			CopyWait wait = {false, 0};
			const auto completed = [](void *ctx, int err) {
				auto *w = static_cast<CopyWait *>(ctx);
				IOLockLock(queue.sync->lock);
				w->error = err;
				w->done = true;
				IOLockWakeup(queue.sync->lock, w, false);
				IOLockUnlock(queue.sync->lock);
			};
			uint64_t deadline = 0;
			clock_interval_to_deadline(1, 1000000000, &deadline);
			dstMap.pending = true;
			queue.jobs[slot] = {};
			queue.jobs[slot].mapping = &dstMap;
			queue.jobs[slot].completed = completed;
			queue.jobs[slot].context = &wait;
			queue.jobs[slot].bytes = bytes;
			queue.jobs[slot].deadline = deadline;
			queue.jobs[slot].sequence = sequence;
			queue.jobs[slot].srcMapping = &srcMap;
			arm();
			while (!wait.done)
				IOLockSleep(queue.sync->lock, &wait, THREAD_UNINT);
			result = wait.error;
		}
	}
	srcMap.pending = false;
	dstMap.pending = false;
	unmapWindow(&dstMap);
	unmapWindow(&srcMap);
	IOLockWakeup(queue.sync->lock, &queue.jobs, false);
	IOLockUnlock(queue.sync->lock);
	return result;
}
