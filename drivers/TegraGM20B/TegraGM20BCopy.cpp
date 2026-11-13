// SPDX-License-Identifier: MIT
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread_call.h>
#include "TegraGM20BCopyPush.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BTiled.h"
#include "SwitchGpuCopyMappings.h"
#include "SwitchGpuEventClient.h"
#include "SwitchGpuKqueue.h"

namespace
{
constexpr uint32_t Jobs = 8;
constexpr uint64_t Page = 4096, PushVa = 1ULL << 20;
constexpr SwitchGpuKqueueShape kShape = {"ce-copy", TGM_FENCE_KERNEL_COPY, Jobs, PushVa, 1, 0};

struct Job {
	bool busy;
	uint32_t dwords, order;
	SwitchGpuCopyMapping *source, *destination;
	switch_gpu_backing_token completion;
	tegra_gm20b_ce_done done;
};

struct Copier {
	IOLock *lock;
	SwitchGpuKqueue space;
	thread_call_t retirement;
	bool semanticFailed, replaceChannel;
	uint32_t waitingSources, order;
	Job jobs[Jobs];
	SwitchGpuCopyMappings mappings;
	SwitchGpuEventChannel authority;
	SwitchGpuEventJob events;
	switch_gpu_backing_token *dependencies;
} queue;

void fenced(void *)
{
	thread_call_enter(queue.retirement);
}

void discardSpace()
{
	queue.space.discardSpace(kShape);
	IODelete(queue.dependencies, switch_gpu_backing_token, TegraGM20BMaxJobDependencies);
	queue.dependencies = nullptr;
	queue.authority.destroy();
}

bool openSpace()
{
	queue.dependencies = IONew(switch_gpu_backing_token, TegraGM20BMaxJobDependencies);
	if (!queue.dependencies || !queue.authority.initialize() ||
	    !queue.space.openSpace(kShape, &queue)) {
		discardSpace();
		return false;
	}
	return true;
}

// A copy whose own work is done while its event waits are not. Restarting the event order (authority.reset) would let
// the copies after it complete first, and the order cannot put a copy behind it without also handing that copy the
// failure that ended the old order, so a new copy is refused until it retires.
bool awaitingEvents()
{
	for (const auto &job : queue.jobs)
		if (job.busy && switch_gpu_backing_token_physical_status(&job.completion, 0) &&
		    !switch_gpu_backing_token_external_status(&job.completion))
			return true;
	return false;
}

bool idle()
{
	for (const auto &job : queue.jobs)
		if (job.busy)
			return false;
	return true;
}

// A failure restarts the event order at once; copies of an earlier order that fail later (queue.order) do not restart
// the new one. A killed channel's copies are settled (fence released, timeline failed: tgm_chan_killed has run once
// tgm_chan_del returns) and retire through the tokens they hold, so it is replaced at once. A live channel whose order
// failed keeps running its copies, and is replaced at its first idle moment, when deleting it waits for nothing: its
// timeline keeps the failed seqno, and while any timeline does, every event job takes the semantic path
// (switch_gpu_backing_has_semantics).
int channel()
{
	const bool killed = queue.space.channel && tgm_chan_error(queue.space.channel);
	if (killed || queue.semanticFailed) {
		if (awaitingEvents())
			return -16;
		queue.authority.reset();
		queue.semanticFailed = false;
		queue.replaceChannel = true;
		++queue.order;
	}
	if (queue.replaceChannel && (killed || idle())) {
		tgm_chan_del(&queue.space.channel);
		queue.replaceChannel = false;
	}
	if (!queue.space.vm && !openSpace())
		return -12;
	return queue.space.openChannel(kShape, &queue, fenced);
}

#if DEVELOPMENT || DEBUG
// What the CPU wrote for a failed job, beside the PBDMA's view of it in a PBENTRY report
void logFailedPush(const Job &job, int result)
{
	const auto slot = uint32_t(&job - queue.jobs);
	const uint32_t *push = queue.space.words + slot * (Page / 4);
	char words[9 * 32 + 1] = "";
	for (uint32_t i = 0, at = 0; i < job.dwords && i < 32; ++i)
		at += uint32_t(snprintf(words + at, sizeof(words) - at, " %08x", push[i]));
	IOLog("SwitchGpu: ce-copy job %llu failed (%d): slot %u at %#llx, %u words:%s\n",
	      job.completion.value, result, slot, PushVa + slot * Page, job.dwords, words);
}
#endif

void retire(thread_call_param_t, thread_call_param_t)
{
	tegra_gm20b_ce_done callbacks[Jobs];
	int results[Jobs];
	uint32_t count = 0;
	IOLockLock(queue.lock);
	__atomic_store_n(&queue.waitingSources, 1u, __ATOMIC_RELEASE);
	bool pending = false;
	for (auto &job : queue.jobs) {
		if (!job.busy)
			continue;
		const int result = switch_gpu_backing_token_external_status(&job.completion);
		if (!result) {
			pending = true;
			continue;
		}
		if (result < 0) {
			queue.semanticFailed |= job.order == queue.order;
#if DEVELOPMENT || DEBUG
			logFailedPush(job, result);
#endif
		}
		switch_gpu_copy_mapping_put(job.source);
		switch_gpu_copy_mapping_put(job.destination);
		switch_gpu_backing_token_release(&job.completion);
		if (job.done.fn) {
			callbacks[count] = job.done;
			results[count++] = result < 0 ? result : 0;
		}
		job = {};
	}
	__atomic_store_n(&queue.waitingSources, pending, __ATOMIC_RELEASE);
	IOLockUnlock(queue.lock);
	for (uint32_t i = 0; i < count; ++i)
		callbacks[i].fn(callbacks[i].arg, results[i]);
}

bool initialize()
{
	if (__atomic_load_n(&queue.lock, __ATOMIC_ACQUIRE))
		return true;
	auto retirement = thread_call_allocate_with_options(
		retire, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	IOLock *lock = retirement ? IOLockAlloc() : nullptr;
	if (!lock) {
		if (retirement)
			thread_call_free(retirement);
		return false;
	}
	thread_call_t noCall = nullptr;
	if (!__atomic_compare_exchange_n(&queue.retirement, &noCall, retirement, false,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		thread_call_free(retirement);
	IOLock *noLock = nullptr;
	if (!__atomic_compare_exchange_n(&queue.lock, &noLock, lock, false, __ATOMIC_RELEASE,
					 __ATOMIC_ACQUIRE))
		IOLockFree(lock);
	return true;
}

struct Layout {
	tgm_layout value{};
	bool tiled = false;
};

int layout(switch_gpu_backing *backing, uint64_t offset, uint32_t rowBytes, uint32_t rows,
	   Layout &out)
{
	switch_gpu_backing_info info{};
	switch_gpu_backing_describe(backing, &info);
	for (uint32_t i = 0; i < info.layout.plane_count; ++i) {
		const auto &plane = info.layout.planes[i];
		if (plane.offset != offset || plane.layout != SWITCH_GPU_BACKING_BLOCK_LINEAR)
			continue;
		if (!plane.bytes_per_element ||
		    uint64_t(plane.width) * plane.bytes_per_element != rowBytes ||
		    plane.height != rows || plane.block_height_log2 > 5 || (plane.pitch & 63) ||
		    plane.pitch < rowBytes)
			return -22;
		out.tiled = true;
		out.value = {plane.width,
			     plane.height,
			     plane.bytes_per_element,
			     plane.pitch,
			     (plane.height + (8u << plane.block_height_log2) - 1) &
				     ~((8u << plane.block_height_log2) - 1),
			     uint8_t(plane.block_height_log2),
			     Page,
			     plane.bytes};
		return 0;
	}
	auto *coherency = info.page_count ? tgm_coh_find_pa(info.pages[0]) : nullptr;
	if (coherency) {
		tgm_coh_lock(coherency);
		const auto *tiled = __atomic_load_n(tgm_coh_tiled(coherency), __ATOMIC_ACQUIRE);
		if (tiled && tiled->tiled) {
			out.tiled = true;
			out.value = tiled->layout;
		}
		tgm_coh_unlock(coherency);
		tgm_coherency_put(coherency);
	}
	return out.tiled && (offset || uint64_t(out.value.width) * out.value.bpp != rowBytes ||
			     out.value.height != rows)
		       ? -22
		       : 0;
}

int submit(Job &job, const tgm_push &push, switch_gpu_backing *source,
	   switch_gpu_backing *destination, const switch_gpu_copy_ownership *ownership,
	   const switch_gpu_event_submission *events)
{
	int result = queue.events.prepareKernel(events);
	if (!result)
		result = queue.events.sequence(queue.authority);
	switch_gpu_backing_transaction transaction{};
	switch_gpu_chan_submit_enter(queue.space.channel);
	for (; !result;) {
		switch_gpu_chan_next_token(queue.space.channel, &job.completion);
		switch_gpu_backing_use uses[] = {{source, SWITCH_GPU_BACKING_READ, 0},
						 {destination, SWITCH_GPU_BACKING_WRITE, 0}};
		const auto *guards = ownership ? ownership->guards : nullptr;
		const uint32_t guardCount = ownership ? ownership->guard_count : 0;
		result = ownership && ownership->lease_count
				 ? switch_gpu_backing_begin_conversion_checked(
					   uses, 2, ownership->leases, ownership->lease_count,
					   guards, guardCount, &job.completion, &transaction)
				 : switch_gpu_backing_begin_checked(uses, 2, guards, guardCount,
								    &job.completion, &transaction);
		uint32_t count = 0;
		if (!result)
			result = switch_gpu_backing_dependencies(&transaction, queue.dependencies,
								 TegraGM20BMaxJobDependencies,
								 &count);
		if (!result)
			result = queue.events.append(queue.dependencies,
						     TegraGM20BMaxJobDependencies, &count);
		if (!result) {
			const int captured =
				queue.events.captureDependencies(queue.dependencies, count);
			if (captured > 0) {
				switch_gpu_backing_abort(&transaction);
				result = queue.events.inheritDependencies();
				continue;
			}
			result = captured;
		}
		if (!result)
			result = queue.events.bind(&job.completion);
		bool retained = false;
		if (!result) {
			result = switch_gpu_backing_token_retain(&job.completion);
			retained = !result;
		}
		uint32_t sequence = 0;
		if (!result)
			result = switch_gpu_chan_exec_visibility(
				queue.space.channel, &push, 1, queue.dependencies, count, &sequence,
				nullptr,
				SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT |
					SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY |
					(transaction.host_written ? SWITCH_GPU_SUBMIT_HOST_WRITTEN
								  : 0u));
		if (!result) {
			switch_gpu_backing_commit(&transaction);
			queue.events.commit();
			break;
		}
		if (retained)
			switch_gpu_backing_token_release(&job.completion);
		switch_gpu_backing_abort(&transaction);
		if (result != -11)
			break;
		result = switch_gpu_chan_wait_submission_room(queue.space.channel, &push, 1, count,
							      nullptr);
	}
	switch_gpu_backing_abort(&transaction);
	queue.events.clear();
	switch_gpu_chan_submit_leave();
	return result;
}
}

extern "C" void switch_gpu_copy_completed(void)
{
	if (__atomic_load_n(&queue.waitingSources, __ATOMIC_ACQUIRE))
		thread_call_enter(queue.retirement);
}

extern "C" int tegra_gm20b_ce_copy_plane_checked(
	const tegra_gm20b_ce_plane *src, const tegra_gm20b_ce_plane *dst, uint32_t rowBytes,
	uint32_t rows, const tegra_gm20b_ce_wait *wait, const tegra_gm20b_ce_done *done,
	switch_gpu_backing *source, uint64_t sourceOffset, switch_gpu_backing *destination,
	uint64_t destinationOffset, const switch_gpu_copy_ownership *ownership,
	const switch_gpu_event_submission *events)
{
	if (!src || !dst || !source || !destination || !src->memory || !dst->memory || !rowBytes ||
	    !rows || ((src->offset | dst->offset | sourceOffset | destinationOffset) & 15) ||
	    (ownership && (ownership->lease_count > 4 || ownership->guard_count > 8 ||
			   (ownership->lease_count && !ownership->leases) ||
			   (ownership->guard_count && !ownership->guards))))
		return -22;
	if (!initialize())
		return -12;
	IOLockLock(queue.lock);
	uint32_t slot = 0;
	while (slot < Jobs && queue.jobs[slot].busy)
		++slot;
	int result = slot == Jobs ? -16 : channel();
	if (result) {
		IOLockUnlock(queue.lock);
		return result;
	}
	auto &job = queue.jobs[slot];
	Layout from, to;
	result = layout(source, sourceOffset, rowBytes, rows, from);
	if (!result)
		result = layout(destination, destinationOffset, rowBytes, rows, to);
	if (!result && ((from.tiled && to.tiled) || (!from.tiled && src->pitch < rowBytes) ||
			(!to.tiled && dst->pitch < rowBytes)))
		result = -22;
	const uint64_t sourceBytes =
		from.tiled ? from.value.bytes : uint64_t(src->pitch) * (rows - 1) + rowBytes;
	const uint64_t destinationBytes =
		to.tiled ? to.value.bytes : uint64_t(dst->pitch) * (rows - 1) + rowBytes;
	if (!result)
		result = switch_gpu_copy_mapping(queue.space.vm, &queue.mappings,
						 static_cast<IOMemoryDescriptor *>(src->memory),
						 src->offset, source, sourceOffset, sourceBytes,
						 from.tiled, &job.source);
	if (!result)
		result = switch_gpu_copy_mapping(queue.space.vm, &queue.mappings,
						 static_cast<IOMemoryDescriptor *>(dst->memory),
						 dst->offset, destination, destinationOffset,
						 destinationBytes, to.tiled, &job.destination);
	if (!result) {
		const auto *a = job.source->mapping, *b = job.destination->mapping;
		const tgm_copy_job copy = {(from.tiled ? a->tiled : a->pitch) + sourceOffset,
					   (to.tiled ? b->tiled : b->pitch) + destinationOffset,
					   src->pitch,
					   dst->pitch,
					   rowBytes,
					   rows,
					   from.tiled ? &from.value : nullptr,
					   to.tiled ? &to.value : nullptr,
					   wait ? wait->syncpt : TEGRA_GM20B_CE_NO_SYNCPT,
					   wait ? wait->threshold : 0,
					   done ? done->syncpt : TEGRA_GM20B_CE_NO_SYNCPT};
		const tgm_push push = {
			PushVa + slot * Page,
			tgm_copy_push(queue.space.words + slot * (Page / 4), &copy, nullptr, 0),
			0,
			0,
			0,
			0};
		result = submit(job, push, source, destination, ownership, events);
		job.dwords = push.dwords;
	}
	if (!result) {
		job.busy = true;
		job.order = queue.order;
		job.done = done ? *done
				: tegra_gm20b_ce_done{TEGRA_GM20B_CE_NO_SYNCPT, nullptr, nullptr};
	} else {
		switch_gpu_copy_mapping_put(job.source);
		switch_gpu_copy_mapping_put(job.destination);
		job = {};
	}
	IOLockUnlock(queue.lock);
	return result;
}
