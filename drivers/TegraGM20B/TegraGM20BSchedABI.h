/* SPDX-License-Identifier: MIT */
/* Scheduler and submission extensions kept out of TegraGM20BABI.h and TegraGM20BJobABI.h
 * so //gpu:stack_key does not move. */
#ifndef TEGRA_GM20B_SCHED_ABI_H
#define TEGRA_GM20B_SCHED_ABI_H
#include <stdint.h>

enum {
	/* GetInfo.flags: SubmitJob accepts TegraGM20BJobSameUses */
	TegraGM20BInfoSameUses = 1u << 25,
	/* GetInfo.flags: MethodPriority accepts an optional TegraGM20BAbiPriorityReply */
	TegraGM20BInfoPriorityReply = 1u << 30,
	/* GetInfo.flags: MethodChannelProgress returns a channel's GPU progress word VA */
	TegraGM20BInfoChannelProgress = 1u << 31,
	/* TegraGM20BAbiPriorityReply.flags: connection is the UI-critical compositor */
	TegraGM20BPriorityCritical = 1u << 0,
	/* TegraGM20BAbiJob.flags: reuse the channel's last resolved shared uses (shared_count == 0) */
	TegraGM20BJobSameUses = 1u << 10,

	TegraGM20BMethodChannelProgress = 42,
	/* Byte offset inside each 64-byte channel slot of Channels (16-byte tgm_stamp after AbiFence + Semantic + Identity) */
	TegraGM20BFencePageChannelProgressOffset = 48,
};

struct TegraGM20BAbiPriorityReply {
	uint32_t flags;
	uint32_t reserved;
};

struct TegraGM20BAbiChannelProgressReply {
	uint64_t gpu_va;
};

#ifdef __cplusplus
static_assert(sizeof(TegraGM20BAbiPriorityReply) == 8, "TegraGM20BAbiPriorityReply layout");
static_assert(sizeof(TegraGM20BAbiChannelProgressReply) == 8,
	      "TegraGM20BAbiChannelProgressReply layout");
#else
_Static_assert(sizeof(struct TegraGM20BAbiPriorityReply) == 8, "TegraGM20BAbiPriorityReply layout");
_Static_assert(sizeof(struct TegraGM20BAbiChannelProgressReply) == 8,
	       "TegraGM20BAbiChannelProgressReply layout");
#endif

#endif
