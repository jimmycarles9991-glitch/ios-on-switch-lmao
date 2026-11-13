// SPDX-License-Identifier: MIT
// The present's copy, and its redo after a copy engine reset cut it (tgm_ctxsw_decide).
#include "gpu_internal.h"
#include "TegraGM20BCopyResetABI.h"

void tgm_present_push_copy(struct nvif_push *push)
{
	const u32 src_pitch = tgm_present.copy.src_block_log2 >= 0 ? tgm_present.copy.width * 4
								   : (u32)tgm_present.copy.stride;
	const struct tgm_copy_side src = {tgm_present.copy.src_va, src_pitch,
					  tgm_present.copy.src_block_log2};
	const struct tgm_copy_side dst = {tgm_present.copy.dst_va, tgm_present.copy.dst_pitch,
					  (int32_t)tgm_present.copy.block_height_log2};

	PUSH_NVSQ(push, NV906F, NVB06F_WFI, 0);
	PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
	PUSH_NVSQ(push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
		  NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY);
	PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
	if (stats_on())
		__atomic_fetch_add(&tgm_stats.present_flushes, 1, __ATOMIC_RELAXED);
	push->cur += tgm_copy_texels(push->cur, &src, &dst, tgm_present.copy.width,
				     tgm_present.copy.height);
}

/* The present whose copy a copy engine reset cut (tgm_present_redo): its fence completed, so it is copied again, with no
 * acquire (what it waited for completed before), the L2 flush and the fence as present_copy pushes them. The display
 * keeps scanning the frame before it until this copy's fence completes and present_fenced delivers its completion. While
 * the GPU sleeps the redo waits: gate_open kicks it again (tgm_present_redo_kick). A redo that finds no room in the
 * present's ring for 1 s (the copy engine stuck again) goes to the hang rule, as present_copy's does, and is tried
 * again. */
void tgm_present_redo_work(void *unused)
{
	struct tgm_chan *chan = tgm_present.chan;
	struct nvif_push *push = NULL;
	int ret = 0;
	u32 again = 0;

	(void)unused;
	if (!__atomic_load_n(&tgm_present.redoing, __ATOMIC_ACQUIRE) || !gate_try())
		return;
	mutex_lock(&tgm_present.lock);
#if TGM_STATS
	again = (u32)tgm_copy_reset_again_take();
#endif
	push = &chan->chan.push;
	ret = chan->error ? -ENODEV
			  : chan_push_wait(chan, TGM_PRESENT_COPY_DWORDS + CHAN_FENCE_DWORDS +
							 again * (TegraGM20BCopyResetCopies *
									  TGM_PRESENT_COPY_DWORDS +
								  2));
	if (ret == -ETIMEDOUT) {
		tgm_chan_stalled(chan, "present redo: push buffer still full after 1 s",
				 chan->submitted, NSEC_PER_SEC);
	}
	if (ret) {
		mutex_unlock(&tgm_present.lock);
		gate_leave();
		if (ret == -ETIMEDOUT)
			nvxnu_work_schedule(tgm_present.redo_work);
		return;
	}
	tgm_present_push_copy(push);
#if TGM_STATS
	if (again) {
		PUSH_MTHD(push, NV906F, NON_STALL_INTERRUPT, 0);
		for (u32 i = 1; i < TegraGM20BCopyResetCopies; i++)
			tgm_present_push_copy(push);
	}
#endif
	tgm_present.queued = nvxnu_nanotime();
	__atomic_store_n(&tgm_present.pending, chan->submitted + 1, __ATOMIC_RELEASE);
	__atomic_store_n(&tgm_present.redoing, 0, __ATOMIC_RELEASE);
	tgm_present.recheck = TGM_PRESENT_NS;
	nvxnu_work_schedule_after(tgm_present.deadline_work, TGM_PRESENT_NS);
	chan_fence_volatile(chan);
#if TGM_STATS
	tgm_copy_reset_redone();
#endif
	mutex_unlock(&tgm_present.lock);
	gate_leave();
}

void tgm_present_redo_kick(void)
{
	if (__atomic_load_n(&tgm_present.redoing, __ATOMIC_ACQUIRE))
		nvxnu_work_schedule(tgm_present.redo_work);
}

/* present_fenced takes pending to 0 before it reads redo: a present that completed before redo was set is seen gone here
 * and the mark withdrawn, so no later present inherits it. */
int tgm_present_redo(void)
{
	u32 pending = __atomic_load_n(&tgm_present.pending, __ATOMIC_SEQ_CST);

	if (!pending || !tgm_present.copy.valid ||
	    __atomic_load_n(&tgm_present.redoing, __ATOMIC_ACQUIRE))
		return 0;
	__atomic_store_n(&tgm_present.redo, pending, __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&tgm_present.pending, __ATOMIC_SEQ_CST) == pending)
		return 1;
	__atomic_compare_exchange_n(&tgm_present.redo, &pending, 0, false, __ATOMIC_SEQ_CST,
				    __ATOMIC_RELAXED);
	return 0;
}
