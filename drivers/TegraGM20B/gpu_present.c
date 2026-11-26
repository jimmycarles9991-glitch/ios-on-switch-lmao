// SPDX-License-Identifier: MIT
// The GPU present: the display's frames copied into its scanout on a kernel channel.
#include "gpu_internal.h"
#include "TegraGM20BCopyResetABI.h"

#define CHAN_PRESENT 1 /* the GPU present's channel */
struct present_state tgm_present = {.lock = {NULL}};

static int present_copy(uint64_t src_va, uint64_t stride, int32_t src_block_log2, uint32_t width,
			uint32_t height, uint64_t dst_va, uint32_t dst_pitch,
			uint32_t block_height_log2, void (*done)(void *, int), void *arg,
			const struct tgm_present_read *reads, u32 nreads,
			const struct switch_gpu_backing_token *dependencies, u32 dependency_count);

/* An accepted present holds the GPU's sleep gate from its acceptance to here: system sleep and the rail shut the gate
 * and wait for it to drain (gpu_power.c sleep_gate), so every accepted present is delivered before the GPU goes down,
 * and nothing in its life (a hidden producer's flush, a reset copy's redo) enters the gate again. */
static void present_deliver(void (*done)(void *, int), void *arg, int status)
{
	switch_gpu_present_deliver(&tgm_present.completing, done, arg, status);
	switch_gpu_admission_changed();
	gate_leave();
}

void tgm_present_set_owner(const void *display)
{
	__atomic_store_n(&tgm_present.owner, display, __ATOMIC_RELEASE);
}

void tgm_present_set_filter_display(const void *display)
{
	__atomic_store_n(&tgm_present.filter_display, display, __ATOMIC_RELEASE);
}

static void present_typed_complete(void *unused)
{
	void (*done)(void *, int);
	void *arg;
	int status;
	(void)unused;
	tgm_present_filter_detach_for(
		__atomic_load_n(&tgm_present.filter_display, __ATOMIC_ACQUIRE));
	mutex_lock(&tgm_present.lock);
	done = tgm_present.done;
	arg = tgm_present.arg;
	mutex_unlock(&tgm_present.lock);
#if TGM_STATS
	tgm_present_promises_done(&tgm_present.tokens, tgm_present.queued, tgm_present.copy.valid);
#endif
	status = switch_gpu_present_tokens_finish_visible(&tgm_present.tokens, 0,
							  switch_gpu_timeline_promote);
	mutex_lock(&tgm_present.lock);
	__atomic_store_n(&tgm_present.typed_pending, 0, __ATOMIC_RELEASE);
	mutex_unlock(&tgm_present.lock);
#if TGM_STATS
	tgm_copy_reset_presented(status);
#endif
	present_deliver(done, arg, status);
}

int tgm_present_outstanding(const void *arg)
{
	const int active = __atomic_load_n(&tgm_present.pending, __ATOMIC_SEQ_CST) ||
			   switch_gpu_present_watch_armed(&tgm_present.watch) ||
			   __atomic_load_n(&tgm_present.completing, __ATOMIC_SEQ_CST) != 0;
	const void *owner = __atomic_load_n(&tgm_present.owner, __ATOMIC_ACQUIRE);

	if (!active)
		return 0;
	return !arg || tgm_present.arg == arg || owner == arg;
}

static int present_physical(const struct switch_gpu_backing_token *token)
{
	return switch_gpu_backing_token_physical_status(token, 0);
}

static int present_visible(const struct switch_gpu_backing_token *token)
{
	return switch_gpu_backing_token_physical_status(token, 1);
}

/* The watch claimed and its producer's verdict in status: delivered as a present job's completion is. */
static void present_watch_complete(int status, int from_pass)
{
	void (*done)(void *, int) = tgm_present.done;
	void *arg = tgm_present.arg;
#if TGM_STATS
	static int logged;
#endif

	nvxnu_work_cancel(tgm_present.deadline_work);
	if (tgm_present.primary)
		tgm_deadline_presented(tgm_present.queued, from_pass);
	switch_gpu_present_watch_reset(&tgm_present.watch);
	__atomic_store_n(&tgm_present.typed_pending, 0, __ATOMIC_RELEASE);
#if TGM_STATS
	if (from_pass && !status && !logged) {
		logged = 1;
		nvxnu_log("tegra-gm20b: first present completed from its producer's interrupt "
			  "pass, with "
			  "no present job\n");
	}
#else
	(void)from_pass;
#endif
	present_deliver(done, arg, status);
}

/* The producer ended with its writes possibly still in the GPU L2: the fence-only present job after all, on the same
 * producer, whose fence flushes them, inside the gate the watch holds. Its own completion delivers the present. The
 * job's every refusal is the kernel's own state gone wrong (the present channel's ring full for 1 s, its fence page
 * unmapped), never the frame's. */
static void present_watch_flush(void)
{
	struct switch_gpu_present_tokens held;
	void (*done)(void *, int) = tgm_present.done;
	void *arg = tgm_present.arg;
	int ret;

	mutex_lock(&tgm_present.lock);
	held = tgm_present.tokens;
	tgm_present.tokens.count = 0;
	__atomic_store_n(&tgm_present.typed_pending, 0, __ATOMIC_RELEASE);
	switch_gpu_present_watch_reset(&tgm_present.watch);
	mutex_unlock(&tgm_present.lock);
	ret = present_copy(0, 0, -1, 1, 1, 0, 0, 0, done, arg, NULL, 0, held.tokens, held.count);
	if (ret)
		nvxnu_panic("tegra-gm20b: present: the fence-only job that puts a direct "
			    "present's producer (uid %u fence %llu) in DRAM was refused (%d)",
			    held.tokens[0].queue, held.tokens[0].value, ret);
	switch_gpu_present_tokens_drop(&held);
	__atomic_fetch_sub(&tgm_present.completing, 1, __ATOMIC_SEQ_CST);
}

static int present_watch_failure(const struct switch_gpu_backing_token *tokens, uint32_t count)
{
	uint32_t i;

	for (i = 0; i < count; ++i)
		if (present_physical(&tokens[i]) < 0 || present_visible(&tokens[i]) < 0)
			return switch_gpu_backing_token_failure(&tokens[i]);
	return count ? switch_gpu_backing_token_failure(&tokens[0])
		     : SWITCH_GPU_BACKING_WRITER_FAILED;
}

/* Thread context, may sleep: a kill, a semantic wait, a hidden producer, the offer's own late look. */
static void present_watch_work(void *unused)
{
	struct switch_gpu_backing_token tokens[SWITCH_GPU_PRESENT_WATCH_MAX];
	uint32_t count = 0;
	int found;
	int status;

	(void)unused;
	found = switch_gpu_present_watch_claim(&tgm_present.watch, present_physical,
					       present_visible);
	if (found == SWITCH_GPU_PRESENT_WATCH_PENDING)
		return;
	switch_gpu_present_watch_snapshot(&tgm_present.watch, tokens, &count, NULL);
	if (count)
		tgm_present_promise_leave(tokens[0].queue, (u32)tokens[0].value);
	tgm_present_filter_detach_for(
		__atomic_load_n(&tgm_present.filter_display, __ATOMIC_ACQUIRE));
	__atomic_fetch_add(&tgm_present.completing, 1, __ATOMIC_SEQ_CST);
	if (found == SWITCH_GPU_PRESENT_WATCH_HIDDEN) {
		present_watch_flush();
		return;
	}
	status = switch_gpu_present_tokens_finish_visible(
		&tgm_present.tokens,
		found == SWITCH_GPU_PRESENT_WATCH_FAILED ? present_watch_failure(tokens, count) : 0,
		switch_gpu_timeline_promote);
	present_watch_complete(status, 0);
}

/* The end of every GPU interrupt pass (shim/xnu_glue.cpp gpuInterrupt), nvkm's locks released and every channel's GPU time
 * of the pass added: a present job the scan completed is learned and handed to its completion work item, and a watch the
 * pass made due is delivered here when its producer's writes are in DRAM; anything else goes to the watch work item. Nothing here may
 * wait for the GPU, as no GPU interrupt is taken until this returns: the tokens finish without waiting, the snapshot's
 * leases (SwitchGpuScanoutSnapshot::complete) release under their backings' locks, and done only signals. The
 * nonstall line's filter may already have run the display's callout for it (SwitchGpuPresentFilter.cpp); each claim
 * detaches the filter's view first. */
void tgm_present_pass(void)
{
	if (tgm_present.fenced_due) {
		tgm_present.fenced_due = 0;
		if (tgm_present.fenced_primary)
			tgm_deadline_presented(tgm_present.fenced_queued, 1);
		nvxnu_work_schedule(tgm_present.completion_work);
	}
	if (switch_gpu_present_watch_state(&tgm_present.watch) != SWITCH_GPU_PRESENT_WATCH_DUE)
		return;
	if (!switch_gpu_present_watch_take_visible(&tgm_present.watch,
						   switch_gpu_backing_token_status,
						   switch_gpu_backing_token_external_status)) {
		if (switch_gpu_present_watch_state(&tgm_present.watch) ==
		    SWITCH_GPU_PRESENT_WATCH_DUE)
			nvxnu_work_schedule(tgm_present.watch_work);
		return;
	}
	tgm_present_filter_detach_for(
		__atomic_load_n(&tgm_present.filter_display, __ATOMIC_ACQUIRE));
	__atomic_fetch_add(&tgm_present.completing, 1, __ATOMIC_SEQ_CST);
	present_watch_complete(switch_gpu_present_tokens_finish_ready(&tgm_present.tokens,
								      switch_gpu_timeline_promote),
			       1);
}

#if TGM_STATS
void tgm_present_stats(u32 *presents, u64 *sum, u64 *max)
{
	*presents = __atomic_exchange_n(&tgm_present.presents, 0, __ATOMIC_RELAXED);
	*sum = __atomic_exchange_n(&tgm_present.lat_sum, 0, __ATOMIC_RELAXED);
	*max = __atomic_exchange_n(&tgm_present.lat_max, 0, __ATOMIC_RELAXED);
}

#endif
int tgm_present_open(struct tgm_vm **pvm)
{
	volatile u32 *probe = NULL;
	u64 probe_pa = 0;
	int ret = 0;

	if (tgm_gpu.stage < 3)
		return -ENODEV;
	mutex_lock(&tgm_present.lock);
	if (tgm_present.chan)
		goto out;
	/* the fence pages exist from the first connection on; make sure they do now */
	ret = tgm_fence_page_get(&probe, &probe_pa);
	if (ret)
		goto out;
	tgm_fence_page_put(probe);
	tgm_present.deadline_work = nvxnu_work_alloc(tgm_present_deadline, NULL);
	tgm_present.redo_work = nvxnu_work_alloc(tgm_present_redo_work, NULL);
	tgm_present.completion_work = nvxnu_work_alloc(present_typed_complete, NULL);
	tgm_present.watch_work = nvxnu_work_alloc(present_watch_work, NULL);
	ret = tgm_present.deadline_work && tgm_present.completion_work && tgm_present.watch_work &&
			      tgm_present.redo_work && !tgm_promise_open()
		      ? tgm_vm_new(&tgm_present.vm)
		      : -ENOMEM;
	if (!ret) {
		tgm_vm_claim(tgm_present.vm, &tgm_present);
		ret = tgm_pfence_get(
			tgm_present
				.vm); /* the channel's own fence: the page every address space may wait on */
	}
	if (!ret) {
		tgm_present.fence = tgm_pfence.va;
		tgm_present.fence_pa = tgm_pfence.pa;
		tgm_present.fence_mem = tgm_pfence.mem;
		ret = tgm_vm_kmap(tgm_present.vm, tgm_present.fence_mem, PAGE_SIZE,
				  &tgm_present.fence_gpu);
	}
	if (!ret)
		ret = tgm_mem_new(tgm_present.vm, tgm_fences_pas, TGM_FENCE_GPU_PAGES,
				  &tgm_present.fences_mem);
	if (!ret) {
		ret = tgm_vm_kmap(tgm_present.vm, tgm_present.fences_mem, TGM_FENCE_BYTES,
				  &tgm_present.fences_gpu);
	}
	if (!ret) {
		ret = tgm_chan_new_copy(tgm_present.vm, &tgm_present, tgm_present.fence,
					tgm_present.fence_gpu, "present", 1, &tgm_present.chan);
	}
	if (!ret) {
		tgm_present.chan->app = 0;
		tgm_present.chan->kernel = CHAN_PRESENT;
		tgm_gpu_lock();
		chan_prioritize(tgm_present.chan);
		mutex_unlock(&tgm_gpu.lock);
	} else {
		/* stays unusable: every offer is refused and the display copies with the CPU */
		nvxnu_log("tegra-gm20b: GPU present unavailable: %d\n", ret);
	}
out:
	*pvm = tgm_present.chan ? tgm_present.vm : NULL;
	mutex_unlock(&tgm_present.lock);
	if (tgm_present.chan)
		return 0;
	return ret ? ret : -ENODEV;
}

/* Present mappings: volatile, at a fixed address of the present's user range. */
int tgm_present_map(uint64_t va, struct tgm_mem *mem, uint64_t bytes, uint8_t kind)
{
	return tgm_present.vm
		       ? tgm_vm_map_raw(tgm_present.vm, va, bytes, mem, 0, kind, 0, 1, 0, NULL)
		       : -ENODEV;
}

int tgm_present_unmap(uint64_t va, uint64_t bytes)
{
	return tgm_present.vm ? tgm_vm_unmap_volatile(tgm_present.vm, va, bytes) : -ENODEV;
}

#if TGM_STATS
/* Each read-back waits for the copies before it (NON_PIPELINED: the scanout buffer is read after the frame's copy wrote
 * it); the fence after them flushes their writes past the L2. */
static void present_push_reads(struct nvif_push *push, const struct tgm_present_read *reads, u32 n)
{
	u32 i = 0;

	for (i = 0; i < n; i++) {
		const struct tgm_present_read *r = &reads[i];
		const struct tgm_copy_side src = {r->src_va, r->src_pitch, r->src_block_log2};
		const struct tgm_copy_side dst = {r->dst_va, r->dst_pitch, -1};

		push->cur += tgm_copy_texels(push->cur, &src, &dst, r->width, r->height);
	}
}

#endif
static int present_copy(uint64_t src_va, uint64_t stride, int32_t src_block_log2, uint32_t width,
			uint32_t height, uint64_t dst_va, uint32_t dst_pitch,
			uint32_t block_height_log2, void (*done)(void *, int), void *arg,
			const struct tgm_present_read *reads, u32 nreads,
			const struct switch_gpu_backing_token *dependencies, u32 dependency_count)
{
	struct tgm_chan *chan = tgm_present.chan;
	struct nvif_push *push = NULL;
	u32 *push_start = NULL;
	u32 words = 0;
	int ret = 0;
	int cut = 0;

	if (!chan || chan->error)
		return -ENODEV;
	idle_wake(); /* the run rate before the copy is kicked */
	/* dst_va 0: no copy, only the acquires and the fence (direct scanout: the display scans the surface itself once the
	 * compositor's rendering of it is done) */
	if (dst_va && (block_height_log2 > 5 || dst_pitch < width * 4))
		return -EINVAL;
	mutex_lock(&tgm_present.lock);
	if (__atomic_load_n(&tgm_present.pending, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&tgm_present.typed_pending, __ATOMIC_ACQUIRE)) {
		mutex_unlock(&tgm_present.lock);
		return -EBUSY;
	}
	ret = switch_gpu_present_tokens_prepare(&tgm_present.tokens, dependencies,
						dependency_count);
	if (ret) {
		switch_gpu_present_tokens_drop(&tgm_present.tokens);
		mutex_unlock(&tgm_present.lock);
		return ret;
	}
#if TGM_STATS
	cut = dst_va && tgm_copy_reset_armed();
#endif
	push = &chan->chan.push;
	/* acquires, a start stamp, optional L2+TLB invalidate, the copy, the read-backs, the fence */
	words = dependency_count * 5 + 57 + 18 + nreads * TGM_COPY_RECT_DWORDS +
		cut * (TegraGM20BCopyResetCopies * TGM_PRESENT_COPY_DWORDS + 2) + CHAN_FENCE_DWORDS;
#if TGM_STATS
	if (push->cur + words > push->end && !tgm_chan_room(chan, 1, words))
		tgm_stats_unlocked(TGM_UNLOCKED_PRESENT_ROOM, 0);
#endif
	ret = chan_push_wait(chan, words);
	if (ret == -ETIMEDOUT) {
		tgm_chan_stalled(chan, "present: push buffer still full after 1 s", chan->submitted,
				 NSEC_PER_SEC);
	}
	if (ret) {
		switch_gpu_present_tokens_drop(&tgm_present.tokens);
		mutex_unlock(&tgm_present.lock);
		return ret;
	}
	push_start = push->cur;
	chan->chan.defer_kick = true;
	ret = tgm_backing_dependencies(chan, tgm_present.tokens.tokens, dependency_count);
	if (ret) {
		push->cur = push_start;
		chan->chan.defer_kick = false;
		switch_gpu_present_tokens_drop(&tgm_present.tokens);
		mutex_unlock(&tgm_present.lock);
		return ret;
	}
	if (stats_on()) { /* the copy's start, after the acquires: stats_fences times the copy and the gap before it */
		tgm_present.stamped = chan->submitted + 1;
		push_stamp(push,
			   chan_stamps_va(chan, tgm_present.stamped) +
				   offsetof(struct tgm_stamps, start),
			   tgm_present.stamped);
	}
	tgm_present.copy.valid = dst_va != 0;
	tgm_present.copy.src_va = src_va;
	tgm_present.copy.stride = stride;
	tgm_present.copy.src_block_log2 = src_block_log2;
	tgm_present.copy.width = width;
	tgm_present.copy.height = height;
	tgm_present.copy.dst_va = dst_va;
	tgm_present.copy.dst_pitch = dst_pitch;
	tgm_present.copy.block_height_log2 = block_height_log2;
	if (tgm_tlb_take(&chan->tlb, &chan->vm->tlb)) {
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_NVSQ(push, NV906F, NVB06F_WFI, 0);
		PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
		PUSH_NVSQ(push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
			  NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY);
		PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
		PUSH_NVSQ(push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
			  NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);
		PUSH_NVSQ(push, NV906F, NVB06F_MEM_OP_C, NVB06F_MEM_OP_C_TLB_INVALIDATE_PDB_ALL,
			  NVB06F_MEM_OP_D, NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE);
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	}
	if (dst_va)
		tgm_present_push_copy(push);
#if TGM_STATS
	if (cut && tgm_copy_reset_take()) {
		PUSH_MTHD(push, NV906F, NON_STALL_INTERRUPT, 0);
		for (u32 i = 1; i < TegraGM20BCopyResetCopies; i++)
			tgm_present_push_copy(push);
	}
#endif
#if TGM_STATS
	if (nreads)
		present_push_reads(push, reads, nreads);
#endif
	/* set before the kick: the nonstall interrupt can come before chan_fence returns */
	tgm_present.done = done;
	tgm_present.arg = arg;
	tgm_present.queued = nvxnu_nanotime();
	tgm_present.primary = tgm_present_filter_is_owner(
		__atomic_load_n(&tgm_present.filter_display, __ATOMIC_ACQUIRE));
	__atomic_store_n(&tgm_present.typed_pending, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&tgm_present.pending, chan->submitted + 1, __ATOMIC_RELEASE);
	tgm_present.recheck = TGM_PRESENT_NS;
	/* before the kick, so the completion cancels it */
	nvxnu_work_schedule_after(tgm_present.deadline_work, tgm_present.recheck);
	/* The present job's own fence, on this kernel channel whose completion is its visibility: the nonstall filter runs
	 * the display's callout as it passes, ahead of the completion work item. A direct present of a producer that ended
	 * without external visibility (a compositor job's) comes here through present_watch_flush, so this is the filter's
	 * only chance at that frame. Before the kick: its nonstall interrupt can come before chan_fence returns. */
	{
		struct switch_gpu_backing_token own = {0};
		switch_gpu_chan_next_token(chan, &own);
		tgm_present_filter_attach_for(
			__atomic_load_n(&tgm_present.filter_display, __ATOMIC_ACQUIRE), &own);
	}
	/* A copy writes through volatile PTEs (tgm_present_map); a fence-only present skips
	 * L2_FLUSH_DIRTY only when no promise/CE token is present and every GM20B dependency is
	 * already externally visible. */
	{
		int need_flush = 0;

		if (!dst_va)
			need_flush = switch_gpu_present_tokens_need_l2_flush(
				&tgm_present.tokens, switch_gpu_backing_token_external_status);
		switch_gpu_chan_fence_sealed(chan, need_flush ? 1 : TGM_FENCE_VOLATILE_WRITES,
					     NULL);
		if (need_flush && stats_on())
			__atomic_fetch_add(&tgm_stats.present_flushes, 1, __ATOMIC_RELAXED);
	}
	if (stats_on())
		__atomic_fetch_add(dependency_count ? &tgm_stats.writer : &tgm_stats.idle, 1,
				   __ATOMIC_RELAXED);
	mutex_unlock(&tgm_present.lock);
	return 0;
}

/* Refused while the gate is shut; an accepted present keeps its entry until it is delivered (present_deliver). */
int tgm_present_copy_dependencies(uint64_t src_va, uint64_t stride, int32_t src_block_log2,
				  uint32_t width, uint32_t height, uint64_t dst_va,
				  uint32_t dst_pitch, uint32_t block_height_log2,
				  void (*done)(void *, int), void *arg,
				  const struct switch_gpu_backing_token *dependencies,
				  uint32_t dependency_count)
{
	int ret;
	if (!gate_try())
		return -EBUSY;
	ret = present_copy(src_va, stride, src_block_log2, width, height, dst_va, dst_pitch,
			   block_height_log2, done, arg, NULL, 0, dependencies, dependency_count);
	if (ret)
		gate_leave();
	return ret;
}
#if TGM_STATS
int tgm_present_copy_read_dependencies(uint64_t src_va, uint64_t stride, int32_t src_block_log2,
				       uint32_t width, uint32_t height, uint64_t dst_va,
				       uint32_t dst_pitch, uint32_t block_height_log2,
				       void (*done)(void *, int), void *arg,
				       const struct tgm_present_read *reads, uint32_t nreads,
				       const struct switch_gpu_backing_token *dependencies,
				       uint32_t dependency_count)
{
	int ret;
	if (nreads > TGM_PRESENT_READS)
		return -EINVAL;
	if (!gate_try())
		return -EBUSY;
	ret = present_copy(src_va, stride, src_block_log2, width, height, dst_va, dst_pitch,
			   block_height_log2, done, arg, reads, nreads, dependencies,
			   dependency_count);
	if (ret)
		gate_leave();
	return ret;
}
#endif

static uint32_t present_watch_collect(const struct switch_gpu_backing_token *tokens, uint32_t count,
				      struct switch_gpu_backing_token *out)
{
	uint32_t watched = 0;
	uint32_t first_gm20b = count;
	uint32_t i;

	if (!tokens || !count)
		return 0;
	if (count == 1 && tokens[0].engine == SWITCH_GPU_ENGINE_PROMISE) {
		out[0] = tokens[0];
		return 1;
	}
	for (i = 0; i < count; ++i) {
		if (tokens[i].engine == SWITCH_GPU_ENGINE_GM20B) {
			if (first_gm20b == count)
				first_gm20b = i;
			if (switch_gpu_backing_token_status(&tokens[i]) == 1 &&
			    switch_gpu_backing_token_external_status(&tokens[i]) == 1)
				continue;
			if (watched == SWITCH_GPU_PRESENT_WATCH_MAX)
				return 0;
			out[watched++] = tokens[i];
		} else if (tokens[i].engine == SWITCH_GPU_ENGINE_PROMISE ||
			   tokens[i].engine == SWITCH_GPU_ENGINE_CE ||
			   switch_gpu_backing_token_status(&tokens[i]) != 1 ||
			   switch_gpu_backing_token_external_status(&tokens[i]) != 1) {
			return 0;
		}
	}
	if (!watched && first_gm20b < count)
		out[watched++] = tokens[first_gm20b];
	return watched;
}

/* No present job: the producers' own interrupt passes complete it (SwitchGpuPresentWatch.h). 1:
 * cannot be watched (empty, unsettled non-GM20B producer, or > SWITCH_GPU_PRESENT_WATCH_MAX
 * active GM20B producers), so the caller queues the fence-only job. */
int tgm_present_watch_dependencies(void (*done)(void *, int), void *arg,
				   const struct switch_gpu_backing_token *dependencies,
				   uint32_t dependency_count)
{
	struct switch_gpu_backing_token watch_tokens[SWITCH_GPU_PRESENT_WATCH_MAX];
	struct switch_gpu_backing_token kept = {0};
	uint32_t watch_count = 0;
	uint64_t watch_gen = 0;
	int ret = 0;

	if (!dependencies || !dependency_count)
		return 1;
	if (!tgm_present.chan || tgm_present.chan->error || !tgm_present.watch_work)
		return -ENODEV;
	if (!gate_try())
		return -EBUSY;
	mutex_lock(&tgm_present.lock);
	if (__atomic_load_n(&tgm_present.pending, __ATOMIC_ACQUIRE) ||
	    __atomic_load_n(&tgm_present.typed_pending, __ATOMIC_ACQUIRE)) {
		ret = -EBUSY;
	} else {
		ret = switch_gpu_present_tokens_prepare(&tgm_present.tokens, dependencies,
							dependency_count);
	}
	if (!ret) {
		watch_count = present_watch_collect(tgm_present.tokens.tokens,
						    tgm_present.tokens.count, watch_tokens);
		if (!switch_gpu_present_watchable(watch_tokens, watch_count)) {
			switch_gpu_present_tokens_drop(&tgm_present.tokens);
			ret = 1;
		}
	}
	if (!ret && watch_count == 1 && watch_tokens[0].engine == SWITCH_GPU_ENGINE_PROMISE) {
		tgm_present_promise_watch(&kept);
		if (kept.engine)
			watch_tokens[0] = kept;
	}
	if (!ret) {
		tgm_present.done = done;
		tgm_present.arg = arg;
		tgm_present.queued = nvxnu_nanotime();
		tgm_present.primary = tgm_present_filter_is_owner(
			__atomic_load_n(&tgm_present.filter_display, __ATOMIC_ACQUIRE));
		tgm_present.late = 0;
		__atomic_store_n(&tgm_present.typed_pending, 1, __ATOMIC_RELEASE);
		tgm_present.recheck = TGM_PRESENT_NS;
		if (watch_count == 1 && watch_tokens[0].engine == SWITCH_GPU_ENGINE_PROMISE) {
			switch_gpu_path_promise_direct(SWITCH_GPU_PATH_DIRECT_PROMISED);
			watch_gen = switch_gpu_present_watch_promise(&tgm_present.watch,
								     &watch_tokens[0]);
			__atomic_store_n(&tgm_present.promise_parked,
					 tgm_promise_park(watch_tokens[0].queue,
							  (u32)watch_tokens[0].value, "the display",
							  "a direct present", NULL, 1) == 1,
					 __ATOMIC_RELEASE);
		} else {
			nvxnu_work_schedule_after(tgm_present.deadline_work, TGM_PRESENT_NS);
			/* Primary filter tracks one timeline view; multi-producer watches deliver
			 * at the last producer's interrupt pass instead. */
			if (watch_count == 1)
				tgm_present_filter_attach_for(
					__atomic_load_n(&tgm_present.filter_display,
							__ATOMIC_ACQUIRE),
					&watch_tokens[0]);
			watch_gen = switch_gpu_present_watch_arm_many(&tgm_present.watch,
								      watch_tokens, watch_count);
		}
	}
	mutex_unlock(&tgm_present.lock);
	if (ret)
		gate_leave();
	if (!ret)
		tgm_chan_need_tokens(watch_tokens, watch_count);
	if (!ret && switch_gpu_present_watch_check(&tgm_present.watch, watch_gen, watch_tokens,
						   watch_count, present_physical))
		nvxnu_work_schedule(tgm_present.watch_work);
	return ret;
}
