// SPDX-License-Identifier: MIT
// Writer promises (SwitchGpuWriterPromise.h): their engine's tokens and the refs a surface keeps to them.
#include "SwitchGpuBackingInternal.h"
#include "TegraGM20BGpu.h"

namespace
{
// An abandoned promise is a failed producer: a reader goes on without it, and a present of it is dropped.
int status(const switch_gpu_backing_token *token)
{
	if (tgm_promise_abandoned(token->queue, uint32_t(token->value)))
		return -5;
	const volatile uint32_t *word = tgm_fence_word(token->queue);
	return !word || int32_t(__atomic_load_n(word, __ATOMIC_ACQUIRE) - uint32_t(token->value)) >=
				       0
		       ? 1
		       : 0;
}

// Nothing on the CPU waits for a promise token: GPU users acquire it, and CPU readers wait for the promised job itself
// (switch_gpu_backing_promise_pending_locked).
int wait(const switch_gpu_backing_token *)
{
	return -95;
}

// The word's fence page stays the connection's while a user holds the token, as a channel's does.
int retain(const switch_gpu_backing_token *token)
{
	const volatile uint32_t *word = tgm_fence_word(token->queue);
	if (!word)
		return -19;
	switch_gpu_fence_hold(word);
	return 0;
}

void release(const switch_gpu_backing_token *token)
{
	if (const volatile uint32_t *word = tgm_fence_word(token->queue))
		switch_gpu_fence_put(word);
}
}  // namespace

extern "C" int switch_gpu_promise_engine(void)
{
	static const switch_gpu_backing_engine operations = {status, wait, retain, release, 32};
	if (tgm_promise_open())
		return -12;
	const int result =
		switch_gpu_backing_register_engine(SWITCH_GPU_ENGINE_PROMISE, &operations);
	return result == -16 ? 0 : result;
}

extern "C" int switch_gpu_backing_promise_add(switch_gpu_backing *backing,
					      const switch_gpu_promise_ref *ref)
{
	IOLockLock(backing->lock);
	const int result =
		switch_gpu_promise_ref_add(backing->promises, &backing->promise_count, ref);
	IOLockUnlock(backing->lock);
	return result;
}

// Its job was submitted or it was given back: CPU readers asleep on it go on, to the job's fence if it has one.
extern "C" void switch_gpu_backing_promise_drop(switch_gpu_backing *backing, uint32_t offset)
{
	IOLockLock(backing->lock);
	switch_gpu_promise_ref_drop(backing->promises, &backing->promise_count, offset);
	IOLockWakeup(backing->lock, &backing->cpu_writers, false);
	IOLockUnlock(backing->lock);
}

extern "C" int switch_gpu_backing_promise_tokens(switch_gpu_backing *backing, uint32_t own_first,
						 uint32_t own_end, switch_gpu_backing_token *tokens,
						 uint32_t capacity)
{
	// a promise made later than this read is for a frame its maker had not handed out yet
	if (!__atomic_load_n(&backing->promise_count, __ATOMIC_RELAXED))
		return 0;
	IOLockLock(backing->lock);
	const int pending =
		switch_gpu_backing_promises_locked(backing, own_first, own_end, tokens, capacity);
	IOLockUnlock(backing->lock);
	return pending;
}

extern "C" int switch_gpu_backing_promised(switch_gpu_backing *backing)
{
	return backing ? switch_gpu_backing_promise_tokens(backing, 0, 0, nullptr, 0) : 0;
}
