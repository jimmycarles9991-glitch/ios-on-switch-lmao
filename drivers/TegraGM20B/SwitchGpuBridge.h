// SPDX-License-Identifier: MIT
#pragma once
#include "SwitchGpuBacking.h"
#ifdef __cplusplus
extern "C" {
#endif

struct switch_gpu_bridge;

enum {
	SWITCH_GPU_BRIDGE_MAX_TOKENS = 1024
};

/* Call outside backing/engine queue locks. The caller retains each token through this call; the returned bridge
 * retains them independently until its GPU work and the consumer have both retired. No producer is waited on by CPU. */
int switch_gpu_bridge_submit(const struct switch_gpu_backing_token *tokens, uint32_t count,
			     struct switch_gpu_bridge **bridge);
void switch_gpu_bridge_syncpoint(const struct switch_gpu_bridge *bridge, uint32_t *syncpoint,
				 uint32_t *threshold);
/* A transaction may use the bridge only if every pending GM20B dependency is covered. Completed tokens need no wait.
 * status must be checked before publishing the consumer's successful completion, including producer failure. */
int switch_gpu_bridge_covers(const struct switch_gpu_bridge *bridge,
			     const struct switch_gpu_backing_token *token);
void switch_gpu_bridge_completed(void);
int switch_gpu_bridge_status(const struct switch_gpu_bridge *bridge);
/* Retained consumer completion only; the handle remains owned by the caller. */
int switch_gpu_bridge_wait(const struct switch_gpu_bridge *bridge);
/* The same completion without a CPU wait: completed(context, 0 or the producers' failure) runs once, in retirement
 * thread context, never inside this call. It counts as a waiter, so the hang check covers a stalled producer as it does
 * for switch_gpu_bridge_wait. One per bridge (-16 for a second); the caller keeps its own reference. */
int switch_gpu_bridge_notify(const struct switch_gpu_bridge *bridge, void (*completed)(void *, int),
			     void *context);
void switch_gpu_bridge_release(struct switch_gpu_bridge **bridge);
int switch_gpu_bridge_wait_room(void);
/* Explicit CPU acquisition only; waits on the bridge interrupt, never polls a producer. */
int switch_gpu_bridge_wait_external(const struct switch_gpu_backing_token *token);
/* Exact external completion for display/presentation. The callback runs in retirement thread context and receives
 * the retained producers' failure even if recovery released their abandoned hardware semaphores. */
int switch_gpu_bridge_fence(const struct switch_gpu_backing_token *tokens, uint32_t count,
			    void (*completed)(void *, int), void *context);
#ifdef __cplusplus
}
#endif
