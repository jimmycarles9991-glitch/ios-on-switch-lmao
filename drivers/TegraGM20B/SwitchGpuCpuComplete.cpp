// SPDX-License-Identifier: MIT
#include "SwitchGpuCpuComplete.h"
#include "SwitchGpuBackingInternal.h"
#include <string.h>

namespace
{
bool valid(const SwitchGpuCpuRange &range)
{
	const uint64_t size = uint64_t(range.backing->pages->count) << 12;
	return range.bytes && range.offset <= size && range.bytes <= size - range.offset;
}

void reset_current(switch_gpu_backing *backing)
{
	if (backing->cpu_current && backing->cpu_current_content != backing->content) {
		memset(backing->cpu_current, 0,
		       uint64_t(backing->cpu_current_pages) * sizeof(uint64_t));
		backing->cpu_current_content = backing->content;
	}
}

bool ready_map(switch_gpu_backing *backing)
{
	if (backing->cpu_current_pages < backing->pages->count) {
		auto *made = IONewZero(uint64_t, backing->pages->count);
		if (!made)
			return false;
		if (backing->cpu_current && backing->cpu_current_content == backing->content)
			memcpy(made, backing->cpu_current,
			       uint64_t(backing->cpu_current_pages) * sizeof(uint64_t));
		IODelete(backing->cpu_current, uint64_t, backing->cpu_current_pages);
		backing->cpu_current = made;
		backing->cpu_current_pages = backing->pages->count;
		backing->cpu_current_content = backing->content;
	}
	reset_current(backing);
	return true;
}

bool current(const switch_gpu_backing *backing, uint64_t line)
{
	return (backing->cpu_current[line / 64] & (uint64_t(1) << (line % 64))) != 0;
}

void mark_current(switch_gpu_backing *backing, uint64_t first, uint64_t end)
{
	while (first < end) {
		const uint32_t bit = uint32_t(first % 64);
		const uint64_t left = end - first;
		const uint32_t count = left < 64 - bit ? uint32_t(left) : 64 - bit;
		const uint64_t mask =
			count == 64 ? UINT64_MAX : ((uint64_t(1) << count) - 1) << bit;
		backing->cpu_current[first / 64] |= mask;
		first += count;
	}
}

int complete(const switch_gpu_backing_token &prefix, const SwitchGpuCpuRange &range)
{
	auto *backing = range.backing;
	IOLockLock(backing->lock);
	switch_gpu_backing_prune_locked(backing);
	int result = 0;
	const switch_gpu_backing_token *writer = nullptr;
	for (uint32_t i = 0; !result && i < backing->access_count; ++i) {
		const auto &access = backing->accesses[i];
		if ((access.access & SWITCH_GPU_BACKING_WRITE) &&
		    access.content == backing->content)
			writer = &access.token;
	}
	const bool qualified =
		backing->cpu_tracking
			? switch_gpu_backing_writer_prefix_locked(backing, prefix, range.offset,
								  range.bytes)
			: writer && switch_gpu_backing_same_timeline(*writer, prefix) &&
				  int32_t(uint32_t(prefix.value - writer->value)) >= 0;
	if (!result && qualified) {
		result = switch_gpu_backing_token_external_status(&prefix);
		if (!result)
			result = -16;
		else if (result == 1 && backing->cpu_writers)
			result = 0;
		if (result == 1 && !ready_map(backing))
			result = -12;
		if (result == 1)
			switch_gpu_backing_cpu_cache_locked(backing, range.offset, range.bytes,
							    SWITCH_GPU_CACHE_CLEAN_INVALIDATE);
	}
	IOLockUnlock(backing->lock);
	return result;
}
}  // namespace

bool switch_gpu_backing_cpu_current_prepare_locked(switch_gpu_backing *backing)
{
	return ready_map(backing);
}

static void cache_gpu_lines(switch_gpu_backing *backing, uint64_t first, uint64_t end,
			    int operation)
{
	if (!backing->cpu_current || backing->cpu_current_pages < backing->pages->count) {
		switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count,
					 first * 64, (end - first) * 64, operation);
		return;
	}
	for (uint64_t line = first; line < end;) {
		if (current(backing, line)) {
			const uint64_t next = (line / 64 + 1) * 64;
			line = backing->cpu_current[line / 64] == UINT64_MAX
				       ? (next < end ? next : end)
				       : line + 1;
			continue;
		}
		const uint64_t begin = line;
		while (line < end && !current(backing, line)) {
			const uint64_t next = (line / 64 + 1) * 64;
			line = !backing->cpu_current[line / 64] ? (next < end ? next : end)
								: line + 1;
		}
		switch_gpu_backing_cache(backing->pages->addresses, backing->pages->count,
					 begin * 64, (line - begin) * 64, operation);
		mark_current(backing, begin, line);
	}
}

void switch_gpu_backing_cpu_cache_locked(switch_gpu_backing *backing, uint64_t offset,
					 uint64_t bytes, int operation)
{
	reset_current(backing);
	const uint64_t first = offset / 64, end = (offset + bytes + 63) / 64;
	if (!backing->cpu_tracking) {
		cache_gpu_lines(backing, first, end, operation);
		return;
	}
	for (uint32_t i = 0; i < backing->writer_count; ++i) {
		const auto &range = backing->writers[i];
		const uint64_t begin = range.begin / 64, finish = range.end / 64;
		if (begin >= end)
			break;
		if (finish > first)
			cache_gpu_lines(backing, begin > first ? begin : first,
					finish < end ? finish : end, operation);
	}
}

void switch_gpu_backing_cpu_written_locked(switch_gpu_backing *backing, uint64_t offset,
					   uint64_t bytes)
{
	uint64_t first = offset / 64, end = (offset + bytes + 63) / 64;
	const uint64_t cached_end = uint64_t(backing->cpu_current_pages) * 64;
	if (end > cached_end)
		end = cached_end;
	while (first < end) {
		const uint32_t bit = uint32_t(first % 64);
		const uint64_t left = end - first;
		const uint32_t count = left < 64 - bit ? uint32_t(left) : 64 - bit;
		const uint64_t mask =
			count == 64 ? UINT64_MAX : ((uint64_t(1) << count) - 1) << bit;
		backing->cpu_current[first / 64] &= ~mask;
		first += count;
	}
}

void switch_gpu_backing_cpu_current_written_locked(switch_gpu_backing *backing, uint64_t offset,
						   uint64_t bytes)
{
	if (!backing->cpu_current || backing->cpu_current_pages < backing->pages->count)
		return;
	reset_current(backing);
	mark_current(backing, offset / 64, (offset + bytes + 63) / 64);
}

int switch_gpu_cpu_complete(const switch_gpu_backing_token &prefix, const SwitchGpuCpuRange *ranges,
			    uint32_t count, TegraGM20BAbiCpuCompleteReply *out)
{
	if (!out || !ranges || !count || count > TegraGM20BMaxJobUses ||
	    prefix.engine != SWITCH_GPU_ENGINE_GM20B || prefix.value > UINT32_MAX)
		return -22;
	*out = {};
	for (uint32_t i = 0; i < count; ++i) {
		if (!ranges[i].backing)
			return -22;
		IOLockLock(ranges[i].backing->lock);
		const bool accepted = valid(ranges[i]);
		IOLockUnlock(ranges[i].backing->lock);
		if (!accepted)
			return -22;
	}
	for (uint32_t i = 0; i < count; ++i) {
		const int result = complete(prefix, ranges[i]);
		if (result == 1)
			++out->current_count;
		else if (!result)
			++out->stale;
		else if (result == -16)
			++out->pending;
		else
			++out->failed;
		if (result < 0 && !out->first_error)
			out->first_error = result;
	}
	return 0;
}
