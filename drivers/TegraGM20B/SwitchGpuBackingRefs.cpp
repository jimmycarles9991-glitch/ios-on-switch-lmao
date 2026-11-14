// SPDX-License-Identifier: MIT
#include "SwitchGpuBackingInternal.h"

namespace
{
enum : uint32_t {
	kFree,
	kWriting,
	kReady
};

switch_gpu_backing_timeline_refs refs[SWITCH_GPU_ENGINE_COUNT];
uint32_t state[SWITCH_GPU_ENGINE_COUNT];
}  // namespace

extern "C" int
switch_gpu_backing_register_timeline_refs(uint32_t engine,
					  const switch_gpu_backing_timeline_refs *operations)
{
	if (!engine || engine >= SWITCH_GPU_ENGINE_COUNT || !operations || !operations->retain ||
	    !operations->release || !operations->status)
		return -22;
	uint32_t expected = kFree;
	if (!__atomic_compare_exchange_n(&state[engine], &expected, kWriting, false,
					 __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
		return -16;
	refs[engine] = *operations;
	__atomic_store_n(&state[engine], kReady, __ATOMIC_RELEASE);
	return 0;
}

const switch_gpu_backing_timeline_refs *switch_gpu_backing_timeline_ops(uint32_t engine)
{
	return engine < SWITCH_GPU_ENGINE_COUNT &&
			       __atomic_load_n(&state[engine], __ATOMIC_ACQUIRE) == kReady
		       ? &refs[engine]
		       : nullptr;
}

int switch_gpu_backing_held_status(const switch_gpu_backing_token &token, const void *handle)
{
	if (handle)
		if (const auto *ops = switch_gpu_backing_timeline_ops(token.engine))
			return ops->status(handle, &token);
	return switch_gpu_backing_token_status(&token);
}

int switch_gpu_backing_access_status(const switch_gpu_backing_access &access)
{
	return switch_gpu_backing_held_status(access.token, access.handle);
}
