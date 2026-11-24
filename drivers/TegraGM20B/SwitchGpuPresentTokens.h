/* SPDX-License-Identifier: MIT */
#pragma once
#include "SwitchGpuBacking.h"

#define SWITCH_GPU_PRESENT_MAX_DEPENDENCIES 32u

struct switch_gpu_present_tokens {
	struct switch_gpu_backing_token tokens[SWITCH_GPU_PRESENT_MAX_DEPENDENCIES];
	uint32_t count;
};

struct switch_gpu_present_pending {
	struct switch_gpu_backing_token token;
	int status, physical;
};

static inline struct switch_gpu_present_pending
switch_gpu_present_tokens_pending(const struct switch_gpu_present_tokens *held)
{
	struct switch_gpu_present_pending pending = {{0, 0, 0, 0}, 1, 1};
	uint32_t i;
	for (i = 0; i < held->count; ++i) {
		const struct switch_gpu_backing_token *token = &held->tokens[i];
		const int status = switch_gpu_backing_token_status(token);
		const int physical = switch_gpu_backing_token_physical_status(token, 0);
		if (status == 1 && physical == 1)
			continue;
		if (!pending.token.engine ||
		    (token->engine == SWITCH_GPU_ENGINE_GM20B && !status && !physical)) {
			pending.token = *token;
			pending.status = status;
			pending.physical = physical;
		}
		if (token->engine == SWITCH_GPU_ENGINE_GM20B && !status && !physical)
			break;
	}
	return pending;
}

static inline int
switch_gpu_present_pending_matches(const struct switch_gpu_present_pending *pending,
				   uint64_t channel_id, uint32_t completed)
{
	const struct switch_gpu_backing_token *token = &pending->token;
	return token->engine == SWITCH_GPU_ENGINE_GM20B && !pending->status && !pending->physical &&
	       token->queue == (uint32_t)channel_id &&
	       token->reset_generation == (channel_id >> 32) + 1 && token->value <= UINT32_MAX &&
	       (int32_t)(completed - (uint32_t)token->value) < 0;
}

/* A writer promise among them still unkept: the present's job parks on its word */
static inline int switch_gpu_present_tokens_promised(const struct switch_gpu_present_tokens *held)
{
	uint32_t i;
	for (i = 0; i < held->count; ++i)
		if (held->tokens[i].engine == SWITCH_GPU_ENGINE_PROMISE &&
		    switch_gpu_backing_token_status(&held->tokens[i]) == 0)
			return 1;
	return 0;
}

static inline int
switch_gpu_present_tokens_need_l2_flush(const struct switch_gpu_present_tokens *held,
					int (*external)(const struct switch_gpu_backing_token *))
{
	uint32_t i;
	for (i = 0; i < held->count; ++i) {
		const struct switch_gpu_backing_token *token = &held->tokens[i];
		/* Promise and CE tokens register no visibility ops and cannot prove DRAM visibility. */
		if (token->engine == SWITCH_GPU_ENGINE_PROMISE ||
		    token->engine == SWITCH_GPU_ENGINE_CE)
			return 1;
		if (token->engine != SWITCH_GPU_ENGINE_GM20B)
			continue;
		if (external(token) != 1)
			return 1;
	}
	return 0;
}

static inline void switch_gpu_present_tokens_drop(struct switch_gpu_present_tokens *held)
{
	while (held->count)
		switch_gpu_backing_token_release(&held->tokens[--held->count]);
}

static inline int switch_gpu_present_tokens_prepare(struct switch_gpu_present_tokens *held,
						    const struct switch_gpu_backing_token *tokens,
						    uint32_t count)
{
	uint32_t i;
	int result = 0;
	if (held->count)
		return -16;
	if (count > SWITCH_GPU_PRESENT_MAX_DEPENDENCIES || (count && !tokens))
		return -22;
	for (i = 0; !result && i < count; ++i) {
		/* A producer that failed before the present left no frame to show: the present is refused with why. */
		if (switch_gpu_backing_token_status(&tokens[i]) < 0)
			result = switch_gpu_backing_token_failure(&tokens[i]);
		else
			result = switch_gpu_backing_token_retain(&tokens[i]);
		if (!result)
			held->tokens[held->count++] = tokens[i];
	}
	if (result)
		switch_gpu_present_tokens_drop(held);
	return result;
}

/* A finished present's token: 0 complete, else why the present ends unshown. One neither complete nor failed is the
 * kernel's own failure: the present's acquire of it on the GPU had passed. */
static inline int switch_gpu_present_token_verdict(const struct switch_gpu_backing_token *token,
						   int status)
{
	return status == 1  ? 0
	       : status < 0 ? switch_gpu_backing_token_failure(token)
			    : SWITCH_GPU_BACKING_WRITER_FAILED;
}

static inline int
switch_gpu_present_tokens_promote(const struct switch_gpu_present_tokens *held,
				  int (*promote)(const struct switch_gpu_backing_token *))
{
	uint32_t i;
	int result = 0;
	for (i = 0; !result && promote && i < held->count; ++i)
		if (held->tokens[i].engine == SWITCH_GPU_ENGINE_GM20B)
			result = switch_gpu_present_token_verdict(&held->tokens[i],
								  promote(&held->tokens[i]));
	return result;
}

/* These callbacks may lock engine timelines; only the deferred completion worker calls this. A result already set (why
 * the present's watched producer failed) stays the present's. */
static inline int
switch_gpu_present_tokens_finish_visible(struct switch_gpu_present_tokens *held, int result,
					 int (*promote)(const struct switch_gpu_backing_token *))
{
	uint32_t i;
	for (i = 0; i < held->count; ++i) {
		int status = switch_gpu_backing_token_status(&held->tokens[i]);
		if (!status)
			status = switch_gpu_backing_token_wait(&held->tokens[i]);
		if (!result)
			result = switch_gpu_present_token_verdict(&held->tokens[i], status);
	}
	if (!result)
		result = switch_gpu_present_tokens_promote(held, promote);
	switch_gpu_present_tokens_drop(held);
	return result;
}

/* The GPU interrupt pass's end (gpu_present.c tgm_present_pass), whose thread no GPU interrupt can reach while it runs:
 * never waits. A token not complete is a failure here, not a sleep only that pass could end. */
static inline int
switch_gpu_present_tokens_finish_ready(struct switch_gpu_present_tokens *held,
				       int (*promote)(const struct switch_gpu_backing_token *))
{
	uint32_t i;
	int result = 0;
	for (i = 0; !result && i < held->count; ++i)
		result = switch_gpu_present_token_verdict(
			&held->tokens[i], switch_gpu_backing_token_status(&held->tokens[i]));
	if (!result)
		result = switch_gpu_present_tokens_promote(held, promote);
	switch_gpu_present_tokens_drop(held);
	return result;
}

static inline int switch_gpu_present_tokens_finish(struct switch_gpu_present_tokens *held,
						   int result)
{
	return switch_gpu_present_tokens_finish_visible(held, result, 0);
}

/* A claimed completion handed to its callback. The claim (`completing`, counted while tgm_present_outstanding is still
 * 1) ends before the callback: the callback's signal lets the display offer its next frame at once on its own thread,
 * which must not find this one outstanding. The display's snapshot stays active until it is called back
 * (SwitchGpuScanoutSnapshot::complete), so the present is outstanding to the display until then all the same. */
static inline void switch_gpu_present_deliver(uint32_t *completing, void (*done)(void *, int),
					      void *arg, int status)
{
	__atomic_fetch_sub(completing, 1u, __ATOMIC_SEQ_CST);
	done(arg, status);
}
