/* SPDX-License-Identifier: MIT */
/* A fence-only present whose GM20B producers (up to SWITCH_GPU_PRESENT_WATCH_MAX) are still running
 * at the offer, completed with no present job. The offer arms the watch, asks each producer's
 * channel for the interrupt at its seqno (a quiet job raises none unasked, SwitchGpuQuiet.h), then
 * checks each producer once more so a pass that ran before the arm is not missed. Each producer
 * channel's interrupt pass clears its pending bit with plain loads (nvkm's event lock is held
 * there); the last pending bit makes the watch due, and that pass's end delivers it. A kill, a
 * channel's teardown, a semantic wait and a producer that ended with its writes still in the L2 go
 * through the present's watch work item, which may sleep. A frame whose sole pending writer is a
 * writer promise (SwitchGpuWriterPromise.h) waits PROMISED until its job is submitted, then watches
 * that job. */
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"

#define SWITCH_GPU_PRESENT_WATCH_MAX 4u

enum {
	SWITCH_GPU_PRESENT_WATCH_IDLE = 0,
	SWITCH_GPU_PRESENT_WATCH_ARMED = 1,
	SWITCH_GPU_PRESENT_WATCH_DUE = 2,
	SWITCH_GPU_PRESENT_WATCH_CLAIMED = 3,
	SWITCH_GPU_PRESENT_WATCH_PROMISED = 4,
};

enum {
	SWITCH_GPU_PRESENT_WATCH_PENDING = 0,
	SWITCH_GPU_PRESENT_WATCH_VISIBLE = 1,
	SWITCH_GPU_PRESENT_WATCH_HIDDEN = 2,
	SWITCH_GPU_PRESENT_WATCH_FAILED = 3,
};

/* ctrl packs (generation << 8) | (pending_mask << 4) | state so pending clears, ARMED->DUE, and
 * claims are single-word CAS transitions immune to ABA across watch reuse. seq guards count and
 * tokens[] for concurrent lock-free readers (kill/teardown vs claim/reset/arm). */
struct switch_gpu_present_watch {
	uint64_t ctrl;
	uint32_t seq;
	uint32_t count;
	struct switch_gpu_backing_token tokens[SWITCH_GPU_PRESENT_WATCH_MAX];
};

typedef int (*switch_gpu_present_watch_status)(const struct switch_gpu_backing_token *);

static inline uint64_t switch_gpu_present_watch_pack(uint64_t gen, uint32_t pending, uint32_t state)
{
	return (gen << 8) | ((uint64_t)(pending & 0xfu) << 4) | (state & 0xfu);
}

static inline uint32_t switch_gpu_present_watch_state(const struct switch_gpu_present_watch *watch)
{
	return (uint32_t)(__atomic_load_n(&watch->ctrl, __ATOMIC_ACQUIRE) & 0xfu);
}

static inline void switch_gpu_present_watch_store_token(struct switch_gpu_backing_token *dst,
							const struct switch_gpu_backing_token *src)
{
	__atomic_store_n(&dst->value, src->value, __ATOMIC_RELAXED);
	__atomic_store_n(&dst->reset_generation, src->reset_generation, __ATOMIC_RELAXED);
	__atomic_store_n(&dst->engine, src->engine, __ATOMIC_RELAXED);
	__atomic_store_n(&dst->queue, src->queue, __ATOMIC_RELAXED);
}

static inline void switch_gpu_present_watch_load_token(const struct switch_gpu_backing_token *src,
						       struct switch_gpu_backing_token *dst)
{
	dst->value = __atomic_load_n(&src->value, __ATOMIC_RELAXED);
	dst->reset_generation = __atomic_load_n(&src->reset_generation, __ATOMIC_RELAXED);
	dst->engine = __atomic_load_n(&src->engine, __ATOMIC_RELAXED);
	dst->queue = __atomic_load_n(&src->queue, __ATOMIC_RELAXED);
}

static inline void
switch_gpu_present_watch_write_tokens(struct switch_gpu_present_watch *watch,
				      const struct switch_gpu_backing_token *tokens, uint32_t count)
{
	const uint32_t seq = __atomic_load_n(&watch->seq, __ATOMIC_RELAXED);
	uint32_t i;

	__atomic_store_n(&watch->seq, seq + 1u, __ATOMIC_RELAXED);
	__atomic_thread_fence(__ATOMIC_RELEASE);
	__atomic_store_n(&watch->count, count, __ATOMIC_RELAXED);
	for (i = 0; i < count; ++i)
		switch_gpu_present_watch_store_token(&watch->tokens[i], &tokens[i]);
	__atomic_store_n(&watch->seq, seq + 2u, __ATOMIC_RELEASE);
}

static inline int switch_gpu_present_watch_read_tokens(const struct switch_gpu_present_watch *watch,
						       struct switch_gpu_backing_token *out,
						       uint32_t *count_out)
{
	const uint32_t seq = __atomic_load_n(&watch->seq, __ATOMIC_ACQUIRE);
	uint32_t count;
	uint32_t i;

	if (seq & 1u)
		return 0;
	count = __atomic_load_n(&watch->count, __ATOMIC_RELAXED);
	if (!count || count > SWITCH_GPU_PRESENT_WATCH_MAX)
		return 0;
	for (i = 0; i < count; ++i)
		switch_gpu_present_watch_load_token(&watch->tokens[i], &out[i]);
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (__atomic_load_n(&watch->seq, __ATOMIC_RELAXED) != seq)
		return 0;
	*count_out = count;
	return 1;
}

static inline uint32_t
switch_gpu_present_watch_snapshot(const struct switch_gpu_present_watch *watch,
				  struct switch_gpu_backing_token *out, uint32_t *count_out,
				  uint32_t *pending_out)
{
	const uint64_t c = __atomic_load_n(&watch->ctrl, __ATOMIC_ACQUIRE);
	const uint32_t state = (uint32_t)(c & 0xfu);

	*count_out = 0;
	if (pending_out)
		*pending_out = (uint32_t)((c >> 4) & 0xfu);
	if (state == SWITCH_GPU_PRESENT_WATCH_IDLE)
		return state;
	if (!switch_gpu_present_watch_read_tokens(watch, out, count_out))
		return SWITCH_GPU_PRESENT_WATCH_IDLE;
	return state;
}

static inline int switch_gpu_present_watchable(const struct switch_gpu_backing_token *tokens,
					       uint32_t count)
{
	uint32_t i;

	if (!tokens || !count)
		return 0;
	if (count == 1 && tokens[0].engine == SWITCH_GPU_ENGINE_PROMISE)
		return 1;
	if (count > SWITCH_GPU_PRESENT_WATCH_MAX)
		return 0;
	for (i = 0; i < count; ++i)
		if (tokens[i].engine != SWITCH_GPU_ENGINE_GM20B)
			return 0;
	return 1;
}

static inline uint64_t
switch_gpu_present_watch_arm_many(struct switch_gpu_present_watch *watch,
				  const struct switch_gpu_backing_token *tokens, uint32_t count)
{
	const uint64_t gen = (__atomic_load_n(&watch->ctrl, __ATOMIC_RELAXED) >> 8) + 1u;
	const uint32_t mask = (1u << count) - 1u;

	switch_gpu_present_watch_write_tokens(watch, tokens, count);
	__atomic_store_n(&watch->ctrl,
			 switch_gpu_present_watch_pack(gen, mask, SWITCH_GPU_PRESENT_WATCH_ARMED),
			 __ATOMIC_SEQ_CST);
	return gen;
}

static inline uint64_t switch_gpu_present_watch_arm(struct switch_gpu_present_watch *watch,
						    const struct switch_gpu_backing_token *token)
{
	return switch_gpu_present_watch_arm_many(watch, token, 1u);
}

static inline uint64_t
switch_gpu_present_watch_promise(struct switch_gpu_present_watch *watch,
				 const struct switch_gpu_backing_token *token)
{
	const uint64_t gen = (__atomic_load_n(&watch->ctrl, __ATOMIC_RELAXED) >> 8) + 1u;

	switch_gpu_present_watch_write_tokens(watch, token, 1u);
	__atomic_store_n(&watch->ctrl,
			 switch_gpu_present_watch_pack(gen, 1u, SWITCH_GPU_PRESENT_WATCH_PROMISED),
			 __ATOMIC_SEQ_CST);
	return gen;
}

static inline int switch_gpu_present_watch_promised(const struct switch_gpu_present_watch *watch,
						    uint32_t offset, uint32_t value)
{
	struct switch_gpu_backing_token tokens[SWITCH_GPU_PRESENT_WATCH_MAX];
	uint32_t count = 0;

	if (switch_gpu_present_watch_state(watch) != SWITCH_GPU_PRESENT_WATCH_PROMISED ||
	    !switch_gpu_present_watch_read_tokens(watch, tokens, &count) || count != 1u)
		return 0;
	return tokens[0].engine == SWITCH_GPU_ENGINE_PROMISE && tokens[0].queue == offset &&
	       (uint32_t)tokens[0].value == value;
}

static inline int switch_gpu_present_watch_kept(struct switch_gpu_present_watch *watch,
						uint32_t offset, uint32_t value,
						const struct switch_gpu_backing_token *producer)
{
	uint64_t c = __atomic_load_n(&watch->ctrl, __ATOMIC_ACQUIRE);
	struct switch_gpu_backing_token tokens[SWITCH_GPU_PRESENT_WATCH_MAX];
	uint32_t count = 0;

	if ((uint32_t)(c & 0xfu) != SWITCH_GPU_PRESENT_WATCH_PROMISED ||
	    !switch_gpu_present_watch_read_tokens(watch, tokens, &count) || count != 1u ||
	    tokens[0].engine != SWITCH_GPU_ENGINE_PROMISE || tokens[0].queue != offset ||
	    (uint32_t)tokens[0].value != value)
		return 0;
	if (!__atomic_compare_exchange_n(
		    &watch->ctrl, &c,
		    switch_gpu_present_watch_pack(c >> 8, 0u, SWITCH_GPU_PRESENT_WATCH_CLAIMED), 0,
		    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
		return 0;
	switch_gpu_present_watch_write_tokens(watch, producer, 1u);
	return 1;
}

static inline void switch_gpu_present_watch_rearm(struct switch_gpu_present_watch *watch)
{
	const uint64_t gen = (__atomic_load_n(&watch->ctrl, __ATOMIC_RELAXED) >> 8) + 1u;

	__atomic_store_n(&watch->ctrl,
			 switch_gpu_present_watch_pack(gen, 1u, SWITCH_GPU_PRESENT_WATCH_ARMED),
			 __ATOMIC_SEQ_CST);
}

static inline int switch_gpu_present_watch_armed(const struct switch_gpu_present_watch *watch)
{
	return switch_gpu_present_watch_state(watch) != SWITCH_GPU_PRESENT_WATCH_IDLE;
}

static inline int switch_gpu_present_watch_fenced_any(struct switch_gpu_present_watch *watch,
						      uint64_t channel_uid, uint32_t completed,
						      int *matched_out)
{
	uint64_t c = __atomic_load_n(&watch->ctrl, __ATOMIC_ACQUIRE);
	struct switch_gpu_backing_token tokens[SWITCH_GPU_PRESENT_WATCH_MAX];
	uint32_t count = 0;

	if (matched_out)
		*matched_out = 0;
	while ((uint32_t)(c & 0xfu) == SWITCH_GPU_PRESENT_WATCH_ARMED) {
		uint32_t pending = (uint32_t)((c >> 4) & 0xfu);
		uint32_t cleared = 0;
		uint32_t next_pending;
		uint32_t next_state;
		uint64_t next_c;
		uint32_t i;

		if (!switch_gpu_present_watch_read_tokens(watch, tokens, &count))
			return 0;
		for (i = 0; i < count; ++i) {
			if (tokens[i].engine == SWITCH_GPU_ENGINE_GM20B &&
			    tokens[i].queue == (uint32_t)channel_uid &&
			    tokens[i].reset_generation == (channel_uid >> 32) + 1 &&
			    (int32_t)(completed - (uint32_t)tokens[i].value) >= 0) {
				if (matched_out)
					*matched_out = 1;
				if (pending & (1u << i))
					cleared |= 1u << i;
			}
		}
		if (!cleared)
			return 0;
		next_pending = pending & ~cleared;
		next_state = next_pending ? SWITCH_GPU_PRESENT_WATCH_ARMED
					  : SWITCH_GPU_PRESENT_WATCH_DUE;
		next_c = switch_gpu_present_watch_pack(c >> 8, next_pending, next_state);
		if (__atomic_compare_exchange_n(&watch->ctrl, &c, next_c, 0, __ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			return next_state == SWITCH_GPU_PRESENT_WATCH_DUE;
	}
	return 0;
}

static inline int switch_gpu_present_watch_fenced(struct switch_gpu_present_watch *watch,
						  uint64_t channel_uid, uint32_t completed)
{
	return switch_gpu_present_watch_fenced_any(watch, channel_uid, completed, 0);
}

/* Offer's post-ask check: clears any token already physically complete before arm/ask so a quiet
 * or pre-completed producer never stalls pending, and returns 1 when watch_work must run (every
 * token complete, any token failed, or a promised token settled). */
static inline int switch_gpu_present_watch_check(struct switch_gpu_present_watch *watch,
						 uint64_t gen,
						 const struct switch_gpu_backing_token *tokens,
						 uint32_t count,
						 switch_gpu_present_watch_status completed)
{
	uint64_t c = __atomic_load_n(&watch->ctrl, __ATOMIC_ACQUIRE);
	uint32_t state = (uint32_t)(c & 0xfu);
	uint32_t done_mask = 0;
	uint32_t i;

	if ((c >> 8) != gen)
		return 0;
	if (state == SWITCH_GPU_PRESENT_WATCH_PROMISED)
		return count == 1u && completed(&tokens[0]) != 0;
	if (state != SWITCH_GPU_PRESENT_WATCH_ARMED)
		return 0;
	for (i = 0; i < count; ++i) {
		const int st = completed(&tokens[i]);
		if (st < 0)
			return 1;
		if (st > 0)
			done_mask |= 1u << i;
	}
	if (!done_mask)
		return 0;
	while ((c >> 8) == gen && (uint32_t)(c & 0xfu) == SWITCH_GPU_PRESENT_WATCH_ARMED) {
		const uint32_t pending = (uint32_t)((c >> 4) & 0xfu);
		const uint32_t next_pending = pending & ~done_mask;
		const uint32_t next_state = next_pending ? SWITCH_GPU_PRESENT_WATCH_ARMED
							 : SWITCH_GPU_PRESENT_WATCH_DUE;
		const uint64_t next_c =
			switch_gpu_present_watch_pack(gen, next_pending, next_state);
		if (next_pending == pending)
			return 0;
		if (__atomic_compare_exchange_n(&watch->ctrl, &c, next_c, 0, __ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			return next_state == SWITCH_GPU_PRESENT_WATCH_DUE;
	}
	return 0;
}

static inline int switch_gpu_present_watch_take_visible(struct switch_gpu_present_watch *watch,
							switch_gpu_present_watch_status status,
							switch_gpu_present_watch_status external)
{
	uint64_t c = __atomic_load_n(&watch->ctrl, __ATOMIC_ACQUIRE);
	struct switch_gpu_backing_token tokens[SWITCH_GPU_PRESENT_WATCH_MAX];
	uint32_t count = 0;
	uint32_t i;

	if ((uint32_t)(c & 0xfu) != SWITCH_GPU_PRESENT_WATCH_DUE ||
	    !switch_gpu_present_watch_read_tokens(watch, tokens, &count))
		return 0;
	for (i = 0; i < count; ++i)
		if (status(&tokens[i]) != 1 || external(&tokens[i]) != 1)
			return 0;
	return __atomic_compare_exchange_n(
		&watch->ctrl, &c,
		switch_gpu_present_watch_pack(c >> 8, 0u, SWITCH_GPU_PRESENT_WATCH_CLAIMED), 0,
		__ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Completion is read before visibility on each token: an External job releases its visibility word
 * ahead of its fence, so a completion seen first cannot hide a visibility that came with it. Any
 * failed token claims FAILED immediately without waiting for other tokens. */
static inline int switch_gpu_present_watch_claim(struct switch_gpu_present_watch *watch,
						 switch_gpu_present_watch_status completed,
						 switch_gpu_present_watch_status external)
{
	uint64_t c = __atomic_load_n(&watch->ctrl, __ATOMIC_ACQUIRE);
	struct switch_gpu_backing_token tokens[SWITCH_GPU_PRESENT_WATCH_MAX];
	uint32_t count = 0;
	uint32_t state = (uint32_t)(c & 0xfu);
	uint64_t gen = c >> 8;
	int any_pending = 0;
	int any_hidden = 0;
	int any_failed = 0;
	int found;
	uint32_t i;

	if (state != SWITCH_GPU_PRESENT_WATCH_ARMED && state != SWITCH_GPU_PRESENT_WATCH_DUE &&
	    state != SWITCH_GPU_PRESENT_WATCH_PROMISED)
		return SWITCH_GPU_PRESENT_WATCH_PENDING;
	if (!switch_gpu_present_watch_read_tokens(watch, tokens, &count))
		return SWITCH_GPU_PRESENT_WATCH_PENDING;
	for (i = 0; i < count; ++i) {
		const int done = completed(&tokens[i]);
		if (done < 0) {
			any_failed = 1;
			break;
		}
		if (!done) {
			any_pending = 1;
		} else if (tokens[i].engine == SWITCH_GPU_ENGINE_PROMISE) {
			/* A promise fence word does not prove DRAM visibility if an Internal job kept it. */
			any_hidden = 1;
		} else {
			const int visible = external(&tokens[i]);
			if (visible < 0) {
				any_failed = 1;
				break;
			}
			if (!visible)
				any_hidden = 1;
		}
	}
	if (any_failed)
		found = SWITCH_GPU_PRESENT_WATCH_FAILED;
	else if (any_pending)
		return SWITCH_GPU_PRESENT_WATCH_PENDING;
	else if (any_hidden)
		found = SWITCH_GPU_PRESENT_WATCH_HIDDEN;
	else
		found = SWITCH_GPU_PRESENT_WATCH_VISIBLE;
	while ((c >> 8) == gen) {
		state = (uint32_t)(c & 0xfu);
		if (state != SWITCH_GPU_PRESENT_WATCH_ARMED &&
		    state != SWITCH_GPU_PRESENT_WATCH_DUE &&
		    state != SWITCH_GPU_PRESENT_WATCH_PROMISED)
			return SWITCH_GPU_PRESENT_WATCH_PENDING;
		const uint64_t next =
			switch_gpu_present_watch_pack(gen, 0u, SWITCH_GPU_PRESENT_WATCH_CLAIMED);
		if (__atomic_compare_exchange_n(&watch->ctrl, &c, next, 0, __ATOMIC_ACQ_REL,
						__ATOMIC_ACQUIRE))
			return found;
	}
	return SWITCH_GPU_PRESENT_WATCH_PENDING;
}

static inline void switch_gpu_present_watch_reset(struct switch_gpu_present_watch *watch)
{
	const uint64_t gen = __atomic_load_n(&watch->ctrl, __ATOMIC_RELAXED) >> 8;

	__atomic_store_n(&watch->ctrl,
			 switch_gpu_present_watch_pack(gen, 0u, SWITCH_GPU_PRESENT_WATCH_IDLE),
			 __ATOMIC_RELEASE);
}
