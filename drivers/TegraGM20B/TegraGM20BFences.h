/* SPDX-License-Identifier: MIT */
/* Fence decisions apart from the locks they run under, so a host test runs them. */
#ifndef TEGRA_GM20B_FENCES_H
#define TEGRA_GM20B_FENCES_H
#include <stdint.h>
#include "TegraGM20BABI.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How a kernel copy on channel `self` (the copy engine's, a tiling move's) follows a surface writer `uid` still running:
 * its own channel runs it in order; another channel's fence in its space's fence view (every channel's is, the kernel's own
 * client channels' included: gpu_chan.c tgm_fence_kernel) is a semaphore acquire in its push. One it cannot acquire on is a
 * refusal, never a CPU wait: the copy goes ahead unordered and says so. */
enum tgm_writer_order {
	TGM_WRITER_IN_CHANNEL,
	TGM_WRITER_GPU_ACQUIRE,
	TGM_WRITER_REFUSED,
};

static inline enum tgm_writer_order tgm_writer_order(uint32_t self, uint32_t uid, int acquirable)
{
	if (uid == self)
		return TGM_WRITER_IN_CHANNEL;
	return acquirable != 0 ? TGM_WRITER_GPU_ACQUIRE : TGM_WRITER_REFUSED;
}

/* Whether a wait on `seqno` of a channel whose fence word is `fence` passed only because the channel was killed with that
 * submission unfinished (its kill record, gpu_hang.c tgm_chan_killed): what the waiter ordered after never ran to its end. */
static inline int tgm_fence_lost(const volatile struct TegraGM20BAbiFence *fence, uint32_t seqno)
{
	return (fence->error != 0 && (int32_t)(seqno - fence->killed_first) >= 0 &&
		(int32_t)(fence->killed_last - seqno) >= 0)
		       ? 1
		       : 0;
}

#ifdef __cplusplus
}
#endif
#endif
