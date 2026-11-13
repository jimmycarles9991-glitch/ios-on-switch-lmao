// SPDX-License-Identifier: MIT
#include "SwitchGpuBackingInternal.h"

namespace
{
constexpr uint32_t maxIntervals = 4096;

bool reserve_next(switch_gpu_backing *backing, uint32_t count)
{
	if (count <= backing->writer_next_capacity)
		return true;
	if (count > maxIntervals)
		return false;
	uint32_t capacity = backing->writer_next_capacity ? backing->writer_next_capacity : 16;
	while (capacity < count)
		capacity *= 2;
	auto *made = IONew(switch_gpu_writer_range, capacity);
	if (!made)
		return false;
	IODelete(backing->writer_next, switch_gpu_writer_range, backing->writer_next_capacity);
	backing->writer_next = made;
	backing->writer_next_capacity = capacity;
	return true;
}

void sort_ranges(switch_gpu_backing_write_range *ranges, uint32_t count)
{
	const auto less = [&](uint32_t a, uint32_t b) {
		return ranges[a].backing->id < ranges[b].backing->id ||
		       (ranges[a].backing == ranges[b].backing &&
			ranges[a].offset < ranges[b].offset);
	};
	const auto swap = [&](uint32_t a, uint32_t b) {
		const auto range = ranges[a];
		ranges[a] = ranges[b];
		ranges[b] = range;
	};
	const auto sift = [&](uint32_t root, uint32_t end) {
		while (root < end / 2) {
			uint32_t child = root * 2 + 1;
			if (child + 1 < end && less(child, child + 1))
				++child;
			if (!less(root, child))
				break;
			swap(root, child);
			root = child;
		}
	};
	for (uint32_t root = count / 2; root; --root)
		sift(root - 1, count);
	for (uint32_t end = count; end > 1; --end) {
		swap(0, end - 1);
		sift(0, end - 1);
	}
}

int prepare_ranges(switch_gpu_backing *backing, const switch_gpu_backing_token &producer,
		   const switch_gpu_backing_write_range *ranges, uint32_t count)
{
	if (!backing->cpu_tracking)
		return 0;
	if (!ranges[0].bytes)
		return 0;
	uint32_t needed = 0;
	switch_gpu_writer_range last = {};
	switch_gpu_writer_overlay(backing->writers, backing->writer_count, ranges, count, producer,
				  [&](uint64_t begin, uint64_t end, const auto &token) {
					  if (!needed || last.end != begin ||
					      !switch_gpu_writer_token_equal(last.token, token))
						  ++needed;
					  last = {begin, end, token};
					  return true;
				  });
	if (needed > maxIntervals)
		return -28;
	switch_gpu_backing_writer_abort_locked(backing);
	if (!reserve_next(backing, needed))
		return -12;
	uint32_t built = 0;
	switch_gpu_writer_overlay(
		backing->writers, backing->writer_count, ranges, count, producer,
		[&](uint64_t begin, uint64_t end, const auto &token) {
			if (built && backing->writer_next[built - 1].end == begin &&
			    switch_gpu_writer_token_equal(backing->writer_next[built - 1].token,
							  token))
				backing->writer_next[built - 1].end = end;
			else
				backing->writer_next[built++] = {begin, end, token};
			return true;
		});
	backing->writers_pending = true;
	for (uint32_t i = 0; i < built; ++i) {
		const int result = switch_gpu_backing_token_retain(&backing->writer_next[i].token);
		if (result)
			return result;
		++backing->writer_next_count;
	}
	backing->writers_precise = true;
	return 0;
}
}  // namespace

extern "C" int switch_gpu_backing_cpu_track(switch_gpu_backing *backing)
{
	if (!backing)
		return -22;
	IOLockLock(backing->lock);
	int result = 0;
	if (!backing->cpu_tracking) {
		if (!reserve_next(backing, 16))
			result = -12;
		if (!result && !backing->writer_capacity) {
			backing->writers = IONew(switch_gpu_writer_range, 16);
			if (backing->writers)
				backing->writer_capacity = 16;
			else
				result = -12;
		}
		for (uint32_t i = 0; !result && i < backing->access_count; ++i) {
			const auto &access = backing->accesses[i];
			if (!(access.access & SWITCH_GPU_BACKING_WRITE) ||
			    access.content != backing->content)
				continue;
			result = switch_gpu_backing_token_retain(&access.token);
			if (!result) {
				backing->writers[0] = {0, uint64_t(backing->pages->count) << 12,
						       access.token};
				backing->writer_count = 1;
			}
			break;
		}
		if (!result &&
		    (backing->writer_count || !(backing->dirty & SWITCH_GPU_BACKING_GPU_DIRTY)))
			backing->cpu_tracking = true;
	}
	IOLockUnlock(backing->lock);
	return result;
}

int switch_gpu_backing_writer_prepare_locked(switch_gpu_backing *backing,
					     const switch_gpu_backing_token &producer)
{
	if (!backing->cpu_tracking)
		return 0;
	if (backing->writers_pending || !reserve_next(backing, 1))
		return -12;
	const int result = switch_gpu_backing_token_retain(&producer);
	if (result)
		return result;
	backing->writer_next[0] = {0, uint64_t(backing->pages->count) << 12, producer};
	backing->writer_next_count = 1;
	backing->writers_pending = true;
	backing->writers_precise = false;
	return 0;
}

void switch_gpu_backing_writer_abort_locked(switch_gpu_backing *backing)
{
	while (backing->writer_next_count)
		switch_gpu_backing_token_release(
			&backing->writer_next[--backing->writer_next_count].token);
	backing->writers_pending = false;
	backing->writers_precise = false;
}

void switch_gpu_backing_writer_commit_locked(switch_gpu_backing *backing,
					     const switch_gpu_backing_token &producer)
{
	if (!backing->cpu_tracking || !backing->writers_pending)
		return;
	if (backing->writers_precise && backing->cpu_current &&
	    backing->cpu_current_content == backing->content) {
		for (uint32_t i = 0; i < backing->writer_next_count; ++i) {
			const auto &range = backing->writer_next[i];
			if (switch_gpu_writer_token_equal(range.token, producer))
				switch_gpu_backing_cpu_written_locked(backing, range.begin,
								      range.end - range.begin);
		}
		backing->cpu_current_content = backing->content + 1;
	}
	while (backing->writer_count)
		switch_gpu_backing_token_release(&backing->writers[--backing->writer_count].token);
	auto *old = backing->writers;
	const uint32_t capacity = backing->writer_capacity;
	backing->writers = backing->writer_next;
	backing->writer_capacity = backing->writer_next_capacity;
	backing->writer_count = backing->writer_next_count;
	backing->writer_next = old;
	backing->writer_next_capacity = capacity;
	backing->writer_next_count = 0;
	backing->writers_pending = false;
	backing->writers_precise = false;
}

void switch_gpu_backing_writer_free_locked(switch_gpu_backing *backing)
{
	switch_gpu_backing_writer_abort_locked(backing);
	while (backing->writer_count)
		switch_gpu_backing_token_release(&backing->writers[--backing->writer_count].token);
	IODelete(backing->writers, switch_gpu_writer_range, backing->writer_capacity);
	IODelete(backing->writer_next, switch_gpu_writer_range, backing->writer_next_capacity);
}

bool switch_gpu_backing_writer_prefix_locked(const switch_gpu_backing *backing,
					     const switch_gpu_backing_token &prefix,
					     uint64_t offset, uint64_t bytes)
{
	const uint64_t begin = offset & ~uint64_t(63), end = (offset + bytes + 63) & ~uint64_t(63);
	for (uint32_t i = 0; i < backing->writer_count; ++i) {
		const auto &range = backing->writers[i];
		if (range.begin >= end)
			break;
		if (range.end > begin && (!switch_gpu_backing_same_timeline(range.token, prefix) ||
					  switch_gpu_backing_later(range.token, prefix)))
			return false;
	}
	return true;
}

extern "C" int switch_gpu_backing_write_ranges(switch_gpu_backing_transaction *transaction,
					       switch_gpu_backing_write_range *ranges,
					       uint32_t count)
{
	if (!transaction || transaction->locked != transaction->count ||
	    transaction->prepared != transaction->count || (count && !ranges))
		return -22;
	for (uint32_t i = 0; i < count; ++i) {
		const auto &range = ranges[i];
		if (!range.backing || (!range.bytes && range.offset))
			return -22;
		uint32_t first = 0, last = transaction->count;
		while (first < last) {
			const uint32_t middle = first + (last - first) / 2;
			if (transaction->uses[middle].backing->id < range.backing->id)
				first = middle + 1;
			else
				last = middle;
		}
		if (first == transaction->count ||
		    transaction->uses[first].backing != range.backing ||
		    !(transaction->uses[first].access & SWITCH_GPU_BACKING_WRITE))
			return -22;
		const uint64_t size = uint64_t(range.backing->pages->count) << 12;
		if (range.offset > size || range.bytes > size - range.offset)
			return -22;
	}
	sort_ranges(ranges, count);
	uint32_t at = 0, total = 0;
	for (uint32_t i = 0; i < transaction->count; ++i) {
		const auto &use = transaction->uses[i];
		if (!(use.access & SWITCH_GPU_BACKING_WRITE))
			continue;
		if (at == count || ranges[at].backing != use.backing)
			return -22;
		const uint32_t start = total;
		bool unknown = false;
		while (at < count && ranges[at].backing == use.backing) {
			auto range = ranges[at++];
			unknown |= !range.bytes;
			if (!range.bytes)
				continue;
			const uint64_t end = (range.offset + range.bytes + 63) & ~uint64_t(63);
			range.offset &= ~uint64_t(63);
			range.bytes = end - range.offset;
			if (total > start &&
			    range.offset <= ranges[total - 1].offset + ranges[total - 1].bytes) {
				auto &previous = ranges[total - 1];
				if (end > previous.offset + previous.bytes)
					previous.bytes = end - previous.offset;
			} else {
				ranges[total++] = range;
			}
		}
		if (unknown) {
			total = start;
			ranges[total++] = {use.backing, 0, 0};
		}
	}
	for (uint32_t i = 0; i < total;) {
		const uint32_t begin = i;
		while (i < total && ranges[i].backing == ranges[begin].backing)
			++i;
		const int result = prepare_ranges(ranges[begin].backing, transaction->completion,
						  ranges + begin, i - begin);
		if (result)
			return result;
	}
	return 0;
}
