/* SPDX-License-Identifier: MIT */
/* A connection's persistent resource set (TegraGM20BJobABI.h methods 31-33): private backings registered once, whose GPU
 * use a job records on the set instead of on each backing. The set keeps the latest job of each channel slot that named
 * it; those jobs stand for every member's GPU users wherever a backing's own records are read: a CPU lease on a member
 * and the moment a member stops being private take them into the member's records first. */
#ifndef SWITCH_GPU_RESOURCE_SET_H
#define SWITCH_GPU_RESOURCE_SET_H
#include <stdint.h>
#include "SwitchGpuBacking.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
	SWITCH_GPU_SET_SLOTS = 8,
	SWITCH_GPU_SET_CAUSE_FREE = 0,
	SWITCH_GPU_SET_CAUSE_LAYOUT = 1,
	SWITCH_GPU_SET_CAUSE_SHARED = 2,
	SWITCH_GPU_SET_CAUSE_KILL = 3,
	SWITCH_GPU_SET_CAUSE_REPLACE = 4,
	SWITCH_GPU_SET_CAUSE_CPU = 5,
	SWITCH_GPU_SET_CAUSES = 6,
};

struct switch_gpu_resource_set;

/* access: SWITCH_GPU_BACKING_WRITE when the set's jobs may write the member, else they only read it */
struct switch_gpu_set_member {
	struct switch_gpu_backing *backing;
	uint32_t access, reserved;
};

/* mirror: where the generation is published for the client (the fence page), or NULL */
int switch_gpu_set_create(volatile uint32_t *mirror, struct switch_gpu_resource_set **set);
/* Setup only, with every submission naming the set excluded. Members must be private (no surface, never reached from
 * outside or by the CPU, in no other set); on success the set has exactly these members and a new generation, and a
 * member it no longer lists keeps the set's pending jobs as its users. -EBUSY: a member is in another set; -EINVAL: one
 * is not private; either way `refused` (if given) names it and the set is as it was. */
int switch_gpu_set_attach(struct switch_gpu_resource_set *set,
			  const struct switch_gpu_set_member *members, uint32_t count,
			  uint32_t *generation, struct switch_gpu_backing **refused);
void switch_gpu_set_destroy(struct switch_gpu_resource_set *set);

uint32_t switch_gpu_set_generation(const struct switch_gpu_resource_set *set);
void switch_gpu_set_invalidate(struct switch_gpu_resource_set *set, uint32_t cause);

/* A submission naming the set, from its generation check to its job's record: 0 and nothing held when `generation` is
 * not the set's. A member leaving the set waits for the submissions inside to record their jobs. */
int switch_gpu_set_enter(struct switch_gpu_resource_set *set, uint32_t generation);
void switch_gpu_set_leave(struct switch_gpu_resource_set *set);
/* The slot's channel ran a job naming the set; only that slot's submitter calls it. */
void switch_gpu_set_note_job(struct switch_gpu_resource_set *set, uint32_t slot,
			     const struct switch_gpu_backing_token *job);
uint32_t switch_gpu_set_used(const struct switch_gpu_resource_set *set);
/* Each used slot's latest job, whole: returns how many */
uint32_t switch_gpu_set_jobs(const struct switch_gpu_resource_set *set,
			     struct switch_gpu_backing_token *jobs);
uint32_t switch_gpu_set_seqno(const struct switch_gpu_resource_set *set, uint32_t slot);

struct switch_gpu_set_wait {
	uint32_t slot, seqno;
};

/* What a job of `slot` follows: each other slot's latest set job, for slots in `live` (a channel, not killed) whose
 * fence (completed[slot]) has not reached it. */
uint32_t switch_gpu_set_waits(const struct switch_gpu_resource_set *set, uint32_t slot,
			      uint32_t live, const uint32_t *completed,
			      struct switch_gpu_set_wait *waits);
/* A channel that ran the set's jobs was killed: what it wrote is undefined, so the generation ends. */
void switch_gpu_set_killed(struct switch_gpu_resource_set *set, uint32_t slot);

/* CPU writes to members since the last job: the next job invalidates the GPU L2. */
void switch_gpu_set_host_written(struct switch_gpu_resource_set *set);
int switch_gpu_set_take_host_written(struct switch_gpu_resource_set *set);

struct switch_gpu_resource_set *switch_gpu_backing_set(struct switch_gpu_backing *backing);
/* Before a CPU lease or CpuComplete names the backing: it is never a member again, and a member leaves its set with the
 * set's pending jobs as its users (writers over the whole backing where the set may write it). */
void switch_gpu_backing_member_cpu(struct switch_gpu_backing *backing);
/* The member's BO is gone (setup only, with the set's submissions excluded): its pending set jobs stay its users, it
 * leaves the set and its list, and the generation ends. */
void switch_gpu_backing_member_freed(struct switch_gpu_backing *backing);

#ifdef __cplusplus
}
#endif
#endif
