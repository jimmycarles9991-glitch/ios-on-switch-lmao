// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"

struct switch_gpu_writer_range {
	uint64_t begin, end;
	switch_gpu_backing_token token;
};

inline bool switch_gpu_writer_token_equal(const switch_gpu_backing_token &a,
					  const switch_gpu_backing_token &b)
{
	return a.value == b.value && a.reset_generation == b.reset_generation &&
	       a.engine == b.engine && a.queue == b.queue;
}

template <class Range, class Emit>
bool switch_gpu_writer_overlay(const switch_gpu_writer_range *old, uint32_t old_count,
			       const Range *writes, uint32_t write_count,
			       const switch_gpu_backing_token &producer, Emit emit)
{
	uint32_t prior = 0;
	uint64_t left = old_count ? old[0].begin : 0;
	for (uint32_t i = 0; i < write_count; ++i) {
		const uint64_t begin = writes[i].offset, end = begin + writes[i].bytes;
		while (prior < old_count && old[prior].end <= begin) {
			if (left < old[prior].end && !emit(left, old[prior].end, old[prior].token))
				return false;
			if (++prior < old_count)
				left = old[prior].begin;
		}
		if (prior < old_count && left < begin) {
			if (!emit(left, begin, old[prior].token))
				return false;
			left = begin;
		}
		if (!emit(begin, end, producer))
			return false;
		while (prior < old_count && old[prior].end <= end)
			if (++prior < old_count)
				left = old[prior].begin;
		if (prior < old_count && left < end)
			left = end;
	}
	while (prior < old_count) {
		if (left < old[prior].end && !emit(left, old[prior].end, old[prior].token))
			return false;
		if (++prior < old_count)
			left = old[prior].begin;
	}
	return true;
}
