// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

template <typename Binding>
static inline bool switch_gpu_binding_contains(const Binding &binding, uint64_t address,
					       uint64_t bytes)
{
	return bytes && address >= binding.va && address - binding.va < binding.range &&
	       bytes <= binding.range - (address - binding.va);
}

template <typename Binding>
static inline uint32_t switch_gpu_binding_upper(const Binding *bindings, uint32_t count,
						uint64_t address)
{
	uint32_t low = 0, high = count;
	while (low < high) {
		const uint32_t middle = low + (high - low) / 2;
		if (bindings[middle].va <= address)
			low = middle + 1;
		else
			high = middle;
	}
	return low;
}

template <typename Binding>
static inline const Binding *switch_gpu_binding_find(const Binding *bindings, uint32_t count,
						     uint64_t address, uint64_t bytes)
{
	const uint32_t after = switch_gpu_binding_upper(bindings, count, address);
	return after && switch_gpu_binding_contains(bindings[after - 1], address, bytes)
		       ? &bindings[after - 1]
		       : nullptr;
}

template <typename Binding>
static inline bool switch_gpu_binding_offset(const Binding &binding, uint64_t address,
					     uint64_t bytes, uint64_t &offset)
{
	if (!switch_gpu_binding_contains(binding, address, bytes) || !binding.bo ||
	    binding.bo_offset > binding.bo->bytes)
		return false;
	const uint64_t relative = address - binding.va;
	if (relative > binding.bo->bytes - binding.bo_offset)
		return false;
	offset = binding.bo_offset + relative;
	return bytes <= binding.bo->bytes - offset;
}
