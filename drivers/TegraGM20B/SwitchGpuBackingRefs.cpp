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
void (*forgets[SWITCH_GPU_ENGINE_COUNT])(uint64_t);
}  // namespace

extern "C" int switch_gpu_backing_register_forget(uint32_t engine, void (*forget)(uint64_t))
{
	if (!engine || engine >= SWITCH_GPU_ENGINE_COUNT || !forget)
		return -22;
	void (*expected)(uint64_t) = nullptr;
	if (!__atomic_compare_exchange_n(&forgets[engine], &expected, forget, false,
					 __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return expected == forget ? 0 : -16;
	return 0;
}

// Once per new mapping, never per use: the miss path of an engine's map cache.
extern "C" void switch_gpu_backing_note_mapped(switch_gpu_backing *backing, uint32_t engine)
{
	if (backing && engine < SWITCH_GPU_ENGINE_COUNT)
		__atomic_fetch_or(&backing->mapped_engines, 1u << engine, __ATOMIC_RELEASE);
}

extern "C" void *switch_gpu_backing_mapping_context(const switch_gpu_backing *backing,
						    uint32_t engine)
{
	return backing->mapping_contexts[engine];
}

extern "C" int switch_gpu_backing_mapping_install(switch_gpu_backing *backing, uint32_t engine,
						  uint64_t generation, void *context,
						  void (*destroy)(void *))
{
	if (!backing || !engine || engine >= SWITCH_GPU_ENGINE_COUNT || !context || !destroy)
		return -22;
	const bool claimed = switch_gpu_backing_enter(backing);
	int result = 0;
	if (backing->generation != generation) {
		result = SWITCH_GPU_BACKING_STALE;
	} else if (backing->mapping_contexts[engine]) {
		result = -16;
	} else {
		backing->mapping_contexts[engine] = context;
		backing->mapping_destroy[engine] = destroy;
		backing->mapping_engines |= 1u << engine;
	}
	switch_gpu_backing_leave(backing, claimed);
	return result;
}

void switch_gpu_backing_forget_mapped(const switch_gpu_backing *backing)
{
	for (uint32_t engine = 1; engine < SWITCH_GPU_ENGINE_COUNT; ++engine) {
		if (backing->mapping_contexts[engine])
			backing->mapping_destroy[engine](backing->mapping_contexts[engine]);
	}
	const uint32_t engines = __atomic_load_n(&backing->mapped_engines, __ATOMIC_ACQUIRE);
	for (uint32_t engine = 1; engines && engine < SWITCH_GPU_ENGINE_COUNT; ++engine) {
		auto *forget = __atomic_load_n(&forgets[engine], __ATOMIC_ACQUIRE);
		if ((engines & (1u << engine)) && forget)
			forget(backing->id);
	}
}

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
