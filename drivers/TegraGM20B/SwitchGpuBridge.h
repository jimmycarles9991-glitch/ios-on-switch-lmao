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
/* 1 complete, 0 not yet, <0 failed, by retirement's rule before it stores the answer: a jobless bridge whose covering
 * job died after its increments is complete. Before the slot retires, 1 waits for its syncpoint to reach the threshold
 * too. After, the stored 1 means the producers' writes are in DRAM, and an increment may still be on its way to
 * host1x. */
int switch_gpu_bridge_status(const struct switch_gpu_bridge *bridge);
/* Exceptional inspection of retained original GM20B/promise producers; no bridge queue lock or retirement. */
int switch_gpu_bridge_source_failure(const struct switch_gpu_bridge *bridge);
/* Why the first of the bridge's producers that failed failed (switch_gpu_backing_tokens_failure); for a jobless bridge
 * whose tokens completed, its covering job's cause once retirement stored the failure; 0 when none did. */
int switch_gpu_bridge_why(const struct switch_gpu_bridge *bridge);
/* Retained consumer completion only; the handle remains owned by the caller. */
int switch_gpu_bridge_wait(const struct switch_gpu_bridge *bridge);
/* The same completion without a CPU wait: completed(context, 0 or the failure) runs once, in retirement thread
 * context, never inside this call. The failure is switch_gpu_bridge_why's cause in place of the bridge's own -5 when it
 * has one. It counts as a waiter, so the hang check covers a stalled producer as it does for switch_gpu_bridge_wait.
 * One per bridge (-16 for a second); the caller keeps its own reference. */
int switch_gpu_bridge_notify(const struct switch_gpu_bridge *bridge, void (*completed)(void *, int),
			     void *context);
/* A host1x stream waits on the bridge's syncpoint in hardware with no completion callback (the display's plane stream,
 * NVENC's direct input): the hang check covers its producers as it does a waiter's, from the submit until the bridge
 * retires. switch_gpu_bridge_submit_watched takes the watch under the submit's own lock, the per-frame route;
 * switch_gpu_bridge_watch watches a bridge already submitted, one more lock round trip (a second call does
 * nothing). */
int switch_gpu_bridge_submit_watched(const struct switch_gpu_backing_token *tokens, uint32_t count,
				     struct switch_gpu_bridge **bridge);
int switch_gpu_bridge_watch(const struct switch_gpu_bridge *bridge);
/* 1 when the bridge has no GPU job of its own: its syncpoint is its producer channel's, passed only by increments that
 * follow that channel's drain. Either kind cancels the host1x consumers queued on its syncpoint before a failed
 * producer's unfinished writes can pass it: a jobless bridge when its channel stopped short of the increments
 * (switch_gpu_producer_release, tegra_host1x_syncpt_release_failed), one with its own job at its gate
 * (switch_gpu_bridge_gate_close). A consumer queued after that cancel must find it through switch_gpu_bridge_failed,
 * read under host1x's channel lock before it queues. A failure dated after the writes reached DRAM (a kill between a
 * jobless bridge's increments and its fence) leaves the syncpoint passed over complete writes, and status reports it.
 * Fixed for the bridge's life; no lock. */
int switch_gpu_bridge_jobless(const struct switch_gpu_bridge *bridge);
/* Before a producer's word is raised over writes that never came (a kill's forward store, a promise given up or
 * abandoned, host1x's release of a failed engine's syncpoint), once that producer reads failed: every bridge job with
 * its own GPU job stops at its gate until the bridge has cancelled the host1x consumers of each one whose producer
 * failed (SwitchGpuBridgeGate.cpp). Any context, interrupts off included; takes no sleeping lock. */
void switch_gpu_bridge_gate_close(void);
/* The bridge's stored failure (<0), else 0. Stored before any release of its syncpoint, so a consumer that reads it under
 * host1x's channel lock (gChannelLock, where queue.lock can't be taken) and queues only on 0 is either queued before a
 * release, which cancels it, or reads the failure. No lock. */
int switch_gpu_bridge_failed(const struct switch_gpu_bridge *bridge);
void switch_gpu_bridge_release(struct switch_gpu_bridge **bridge);
int switch_gpu_bridge_wait_room(void);
/* Explicit CPU acquisition only; waits on the bridge interrupt, never polls a producer. */
int switch_gpu_bridge_wait_external(const struct switch_gpu_backing_token *token);
/* Exact external completion for display/presentation. The callback runs in retirement thread context and receives
 * notify's status: the retained producers' failure even if recovery released their abandoned hardware semaphores. */
int switch_gpu_bridge_fence(const struct switch_gpu_backing_token *tokens, uint32_t count,
			    void (*completed)(void *, int), void *context);
#ifdef __cplusplus
}
#endif
