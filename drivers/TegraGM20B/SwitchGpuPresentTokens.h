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
	for (i = 0; i < count; ++i) {
		/* A producer that failed before the present leaves nothing to wait for (its bytes are undefined, the
		 * present goes on); one that fails while the present waits makes it TEGRA_SCANOUT_SOURCE_LOST. */
		if (switch_gpu_backing_token_status(&tokens[i]) < 0)
			continue;
		result = switch_gpu_backing_token_retain(&tokens[i]);
		if (result)
			break;
		held->tokens[held->count++] = tokens[i];
	}
	if (result)
		switch_gpu_present_tokens_drop(held);
	return result;
}

/* These callbacks may lock engine timelines; only the deferred completion worker calls this. */
static inline int
switch_gpu_present_tokens_finish_visible(struct switch_gpu_present_tokens *held, int result,
					 int (*promote)(const struct switch_gpu_backing_token *))
{
	uint32_t i;
	for (i = 0; i < held->count; ++i) {
		int status = switch_gpu_backing_token_status(&held->tokens[i]);
		if (!status)
			status = switch_gpu_backing_token_wait(&held->tokens[i]);
		if (status != 1)
			result = -5;
	}
	if (!result && promote) {
		for (i = 0; i < held->count; ++i)
			if (held->tokens[i].engine == SWITCH_GPU_ENGINE_GM20B &&
			    promote(&held->tokens[i]) != 1)
				result = -5;
	}
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
	for (i = 0; i < held->count; ++i)
		if (switch_gpu_backing_token_status(&held->tokens[i]) != 1)
			result = -5;
	if (!result && promote) {
		for (i = 0; i < held->count; ++i)
			if (held->tokens[i].engine == SWITCH_GPU_ENGINE_GM20B &&
			    promote(&held->tokens[i]) != 1)
				result = -5;
	}
	switch_gpu_present_tokens_drop(held);
	return result;
}

static inline int switch_gpu_present_tokens_finish(struct switch_gpu_present_tokens *held,
						   int result)
{
	return switch_gpu_present_tokens_finish_visible(held, result, 0);
}
