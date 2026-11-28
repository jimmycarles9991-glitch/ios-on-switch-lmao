/* SPDX-License-Identifier: MIT */
#pragma once
#include <stdint.h>
#include "SwitchGpuBacking.h"
#ifdef __cplusplus
extern "C" {
#endif
struct switch_gpu_timeline;
struct switch_gpu_timeline_semantic;
int switch_gpu_timeline_has_semantics(void);
int switch_gpu_timeline_semantic_snapshot(const struct switch_gpu_backing_token *,
					  struct switch_gpu_event_waits **);
int switch_gpu_timeline_physical_status(const struct switch_gpu_backing_token *, int external);
int switch_gpu_timeline_semantic_allocate(struct switch_gpu_timeline_semantic **);
int switch_gpu_timeline_semantic_reserve(const struct switch_gpu_backing_token *,
					 struct switch_gpu_event_waits *,
					 struct switch_gpu_timeline_semantic **);
void switch_gpu_timeline_semantic_finish(struct switch_gpu_timeline_semantic **, int result);
int switch_gpu_timeline_init(void);
struct switch_gpu_timeline *switch_gpu_timeline_create(uint64_t uid,
						       const volatile uint32_t *fence);
struct TegraGM20BAbiFenceSemantic;
void switch_gpu_timeline_semantic_visibility(struct switch_gpu_timeline *,
					     volatile struct TegraGM20BAbiFenceSemantic *);
void switch_gpu_timeline_visibility(struct switch_gpu_timeline *timeline,
				    const volatile uint32_t *visible);
int switch_gpu_timeline_promote(const struct switch_gpu_backing_token *token);
/* Failure publication is safe under NVKM event spin locks; close requires those callbacks to be unregistered. A kill of
 * a compositor channel (critical, TegraGM20BGpu.h tgm_chan_new) fails its unfinished tokens WRITER_FAILED; any other
 * kill and a close fail them WRITER_GONE (switch_gpu_backing_token_failure). */
void switch_gpu_timeline_failed(struct switch_gpu_timeline *timeline, uint32_t completed,
				int compositor);
void switch_gpu_timeline_close(struct switch_gpu_timeline *timeline);
void switch_gpu_timeline_token(struct switch_gpu_timeline *timeline, uint32_t value,
			       struct switch_gpu_backing_token *token);

/* A channel's own host1x syncpoint (SwitchGpuTimelineSyncpt.cpp), held by at most SWITCH_GPU_TIMELINE_SYNCPTS
 * timelines at once. The channel's submitter, for each job whose end it drained (WFI, FB_FLUSH, L2_FLUSH_DIRTY,
 * FB_FLUSH): the syncpoint to increment right after that drain and ahead of the job's fence release, for the job
 * [first, last], its threshold recorded; 0 when the timeline has none. */
#define SWITCH_GPU_TIMELINE_SYNCPTS 8u
int switch_gpu_timeline_has_syncpt(const struct switch_gpu_timeline *timeline);
uint32_t switch_gpu_timeline_syncpt_next(struct switch_gpu_timeline *timeline, uint32_t first,
					 uint32_t last);
/* 1: the token is in a drained job whose last seqno is `last`; once host1x syncpoint `id` reaches `threshold`, that
 * job's writes are in DRAM (its fence lands just after, or never if the channel dies between them). 0: the token's own job did not drain (or is no longer kept), a shared-event
 * wait or a failure is in the job, or the channel ended. -61: the timeline has no syncpoint
 * (switch_gpu_timeline_syncpt_adopt). -19: no such timeline. */
int switch_gpu_timeline_syncpoint(const struct switch_gpu_backing_token *token, uint32_t *id,
				  uint32_t *threshold, uint32_t *last);
/* The token's channel at its job's last seqno `last`, by the fence alone: 1 completed (drained: its writes in DRAM),
 * 0 not yet, -5 the channel ended short of it. *stopped: the channel ended (killed, or closed after its hardware went),
 * so no increment of its can land any more. */
int switch_gpu_timeline_covered(const struct switch_gpu_backing_token *token, uint32_t last,
				int *stopped);
/* Gives the token's timeline syncpoint `id`, whose value is `base`: 0, or -16 when it has one (the caller keeps id). */
int switch_gpu_timeline_syncpt_adopt(const struct switch_gpu_backing_token *token, uint32_t id,
				     uint32_t base);
/* Takes up to `max` syncpoints whose timelines have gone, each with the value its channel's pushed increments promised
 (base + increments), which a channel killed or closed mid-job left unreached; returns how many. */
uint32_t switch_gpu_timeline_syncpt_reclaim(uint32_t *ids, uint32_t *promised, uint32_t max);

/* A token's status read with no lock, valid while the token is retained. Open (thread context) succeeds only when the
 * status is the physical one for good: no semantic wait at or below the token, and not failed. Semantic nodes are
 * reserved before a job's token is committed to its writes, so none can come after. */
struct switch_gpu_timeline_view {
	const struct switch_gpu_timeline *timeline;
	uint32_t value;
};

int switch_gpu_timeline_view_open(const struct switch_gpu_backing_token *token,
				  struct switch_gpu_timeline_view *view);
/* Any context, interrupts off included: as switch_gpu_backing_token_external_status: 1 complete and visible, 0 not
 * yet, -5 failed. */
int switch_gpu_timeline_view_visible(const struct switch_gpu_timeline_view *view);
#ifdef __cplusplus
}
#endif
