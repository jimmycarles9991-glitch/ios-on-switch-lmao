/* SPDX-License-Identifier: MIT */
/* A copy engine reset under the present, debug kernels only (a release kernel answers kIOReturnUnsupported): a call on a
 * TegraGM20B connection (TegraGM20BABI.h's wire form, direct or tunnelled) through which hw_test_app's
 * gpu.present.copy_reset has the kernel do to the display's present what a context-switch timeout on the present's
 * channel does (tgm_ctxsw_decide, TGM_CTXSW_REDO). Kept out of TegraGM20BABI.h, which NVK, the GPU seed and the raw stack
 * key build from: the method number is far from theirs.
 *
 * Arm: the display's next present that copies runs its copy TegraGM20BCopyResetCopies times, with a nonstall interrupt
 * after the first, so the copy engine is busy with the present's context for a few milliseconds; on that interrupt the
 * kernel marks the present for its redo and resets the copy engine if it still holds the present's channel. Query: where
 * that is, the status the display's completion of that present carried (0: it flips the frame), and how many times the
 * present was copied again. One arm system-wide.
 *
 * ArmAgain: as Arm, but the kernel takes each cut as a context-switch timeout tick does (tgm_kernel_stall_locked) and
 * cuts twice: the redo of the first is as long as the armed copy, and its interrupt finds the present stuck again
 * before its redo landed, which kills the client contexts the GPU holds. The reply names those kills and the second
 * verdict; a verdict that would panic (the hardware-failure row) is returned instead. */
#ifndef TEGRA_GM20B_COPY_RESET_ABI_H
#define TEGRA_GM20B_COPY_RESET_ABI_H
#include <stdint.h>

enum {
	TegraGM20BMethodCopyReset = 0x4352, /* 'CR' */

	TegraGM20BCopyResetDisarm = 0,
	TegraGM20BCopyResetArm = 1,
	TegraGM20BCopyResetQuery = 2,
	TegraGM20BCopyResetArmAgain = 3,

	/* TegraGM20BAbiCopyResetReply.state */
	TegraGM20BCopyResetIdle = 0,
	TegraGM20BCopyResetArmed = 1,    /* waiting for the display's next present that copies */
	TegraGM20BCopyResetInFlight = 2, /* its present is queued, the interrupt not taken yet */
	TegraGM20BCopyResetReset = 3,    /* the copy engine was reset under it */
	TegraGM20BCopyResetDone = 4,     /* reset, and the display got that present's completion */
	TegraGM20BCopyResetMissed =
		5, /* the copy engine no longer held the present's channel: nothing reset */

	TegraGM20BCopyResetCopies = 8,
};

struct TegraGM20BAbiCopyReset {
	uint32_t op;
	uint32_t reserved;
};

struct TegraGM20BAbiCopyResetReply {
	int32_t state;
	int32_t status;         /* Done: the completion status the display got */
	uint32_t engine_status; /* the copy engine's PFIFO_ENG_STATUS at the interrupt */
	uint32_t redone;        /* the present's copies redone since the arm */
	uint32_t killed;        /* ArmAgain: client channels killed at the second cut */
	int32_t verdict;        /* ArmAgain: the second cut's enum tgm_ctxsw, -1 before it */
};

#ifdef __cplusplus
static_assert(sizeof(TegraGM20BAbiCopyReset) == 8, "TegraGM20BAbiCopyReset layout");
static_assert(sizeof(TegraGM20BAbiCopyResetReply) == 24, "TegraGM20BAbiCopyResetReply layout");
#endif
#endif
