// SPDX-License-Identifier: MIT
// The GPU present: the display's frames copied into its scanout on a kernel channel.
#include "gpu_internal.h"
#include "TegraGM20BCopyResetABI.h"

#define CHAN_PRESENT 1 /* the GPU present's channel */
struct present_state tgm_present = {.lock = {NULL}};

static void present_typed_complete(void *unused)
{
	void (*done)(void *, int);
	void *arg;
	int status;
	(void)unused;
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
	done(arg, status);
	__atomic_fetch_sub(&tgm_present.completing, 1, __ATOMIC_SEQ_CST);
}

int tgm_present_outstanding(const void *arg)
{
	if ((__atomic_load_n(&tgm_present.pending, __ATOMIC_SEQ_CST) ||
	     switch_gpu_present_watch_armed(&tgm_present.watch)) &&
	    tgm_present.arg == arg)
		return 1;
	return __atomic_load_n(&tgm_present.completing, __ATOMIC_SEQ_CST) != 0;
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
	tgm_deadline_presented(tgm_present.queued);
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
	done(arg, status);
	__atomic_fetch_sub(&tgm_present.completing, 1, __ATOMIC_SEQ_CST);
}

/* The producer ended with its writes possibly still in the GPU L2: the fence-only present job after all, on the same
 * producer, whose fence flushes them. Its own completion delivers the present. */
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
	ret = tgm_present_copy_dependencies(0, 0, -1, 1, 1, 0, 0, 0, done, arg, held.tokens,
					    held.count);
	switch_gpu_present_tokens_drop(&held);
	if (ret) {
		nvxnu_work_cancel(tgm_present.deadline_work);
		done(arg, ret);
	}
	__atomic_fetch_sub(&tgm_present.completing, 1, __ATOMIC_SEQ_CST);
}

/* Thread context, may sleep: a kill, a semantic wait, a hidden producer, the offer's own late look. */
static void present_watch_work(void *unused)
{
	int found;
	int status;

	(void)unused;
	found = switch_gpu_present_watch_claim(&tgm_present.watch, present_physical,
					       present_visible);
	if (found == SWITCH_GPU_PRESENT_WATCH_PENDING)
		return;
	tgm_present_promise_leave(tgm_present.watch.token.queue,
				  (u32)tgm_present.watch.token.value);
	tgm_present_filter_detach();
	__atomic_fetch_add(&tgm_present.completing, 1, __ATOMIC_SEQ_CST);
	if (found == SWITCH_GPU_PRESENT_WATCH_HIDDEN) {
		present_watch_flush();
		return;
	}
	status = switch_gpu_present_tokens_finish_visible(
		&tgm_present.tokens, found == SWITCH_GPU_PRESENT_WATCH_FAILED ? -5 : 0,
		switch_gpu_timeline_promote);
	present_watch_complete(status, 0);
}

/* The end of every GPU interrupt pass (shim/xnu_glue.cpp gpuInterrupt), nvkm's locks released: a watch the pass made due
 * is delivered here when its producer's writes are in DRAM; anything else goes to the watch work item. Nothing here may
 * wait for the GPU, as no GPU interrupt is taken until this returns: the tokens finish without waiting, the snapshot's
 * leases (SwitchGpuScanoutSnapshot::complete) release under their backings' locks, and done only signals. The
 * nonstall line's filter may already have run the display's callout for it (SwitchGpuPresentFilter.cpp); each claim
 * detaches the filter's view first. */
void tgm_present_pass(void)
{
	if (__atomic_load_n(&tgm_present.watch.state, __ATOMIC_ACQUIRE) !=
	    SWITCH_GPU_PRESENT_WATCH_DUE)
		return;
	if (!switch_gpu_present_watch_take_visible(&tgm_present.watch,
						   switch_gpu_backing_token_status,
						   switch_gpu_backing_token_external_status)) {
		if (__atomic_load_n(&tgm_present.watch.state, __ATOMIC_ACQUIRE) ==
		    SWITCH_GPU_PRESENT_WATCH_DUE)
			nvxnu_work_schedule(tgm_present.watch_work);
		return;
	}
	tgm_present_filter_detach();
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
		ret = tgm_chan_new_copy(tgm_present.vm, NULL, tgm_present.fence,
					tgm_present.fence_gpu, "present", 1, &tgm_present.chan);
	}
	if (!ret) {
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
	return tgm_present.vm ? tgm_vm_unmap(tgm_present.vm, va, bytes) : -ENODEV;
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
	if (!ret)
		ret = tgm_backing_dependencies(chan, tgm_present.tokens.tokens, dependency_count,
					       0);
	if (ret) {
		switch_gpu_present_tokens_drop(&tgm_present.tokens);
		mutex_unlock(&tgm_present.lock);
		return ret;
	}
#if TGM_STATS
	cut = dst_va && tgm_copy_reset_armed();
#endif
	push = &chan->chan.push;
	ret = chan_push_wait(
		chan, dependency_count * 5 + 57 + nreads * TGM_COPY_RECT_DWORDS +
			      cut * (TegraGM20BCopyResetCopies * TGM_PRESENT_COPY_DWORDS + 2) +
			      CHAN_FENCE_DWORDS); /* acquires, a start stamp, the copy, the
								  read-backs, the fence */
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
	ret = tgm_backing_dependencies(chan, tgm_present.tokens.tokens, dependency_count, 1);
	if (ret) {
		push->cur = push_start;
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
	__atomic_store_n(&tgm_present.typed_pending, 1, __ATOMIC_RELEASE);
	__atomic_store_n(&tgm_present.pending, chan->submitted + 1, __ATOMIC_RELEASE);
	tgm_present.recheck = TGM_PRESENT_NS;
	/* before the kick, so the completion cancels it */
	nvxnu_work_schedule_after(tgm_present.deadline_work, tgm_present.recheck);
	tgm_chan_park_promises(chan, tgm_present.tokens.tokens, dependency_count);
	/* A copy's flush above put the frame's source in DRAM, and the copy and its read-backs write through the present's
	 * volatile PTEs (tgm_present_map); a fence-only present is direct scanout of a surface the GPU drew through the L2. */
	if (dst_va)
		chan_fence_volatile(chan);
	else
		chan_fence(chan);
	if (!dst_va && stats_on())
		__atomic_fetch_add(&tgm_stats.present_flushes, 1, __ATOMIC_RELAXED);
	if (stats_on())
		__atomic_fetch_add(dependency_count ? &tgm_stats.writer : &tgm_stats.idle, 1,
				   __ATOMIC_RELAXED);
	mutex_unlock(&tgm_present.lock);
	return 0;
}

/* Refused while the gate is shut: the display copies that frame on the CPU. */
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
	gate_leave();
	return ret;
}
#endif

/* No present job: the producer's own interrupt pass completes it (SwitchGpuPresentWatch.h). 1: not exactly one GM20B
 * producer or writer promise still to wait for, so the caller queues the fence-only job; otherwise as
 * tgm_present_copy_dependencies. */
int tgm_present_watch_dependencies(void (*done)(void *, int), void *arg,
				   const struct switch_gpu_backing_token *dependencies,
				   uint32_t dependency_count)
{
	struct switch_gpu_backing_token kept = {0};
	int ret = 0;

	if (!switch_gpu_present_watchable(dependencies, dependency_count) ||
	    switch_gpu_backing_token_status(&dependencies[0]) < 0)
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
	if (!ret && tgm_present.tokens.count != 1) {
		switch_gpu_present_tokens_drop(&tgm_present.tokens);
		ret = 1;
	}
	if (!ret && tgm_present.tokens.tokens[0].engine == SWITCH_GPU_ENGINE_PROMISE)
		tgm_present_promise_watch(&kept);
	if (!ret) {
		tgm_present.done = done;
		tgm_present.arg = arg;
		tgm_present.queued = nvxnu_nanotime();
		tgm_present.late = 0;
		__atomic_store_n(&tgm_present.typed_pending, 1, __ATOMIC_RELEASE);
		tgm_present.recheck = TGM_PRESENT_NS;
		if (tgm_present.tokens.tokens[0].engine == SWITCH_GPU_ENGINE_PROMISE) {
			/* no deadline until the job that keeps it is submitted (tgm_present_promise_kept): the give-up is the
			 * promise's */
			switch_gpu_path_promise_direct(SWITCH_GPU_PATH_DIRECT_PROMISED);
			switch_gpu_present_watch_promise(&tgm_present.watch,
							 &tgm_present.tokens.tokens[0]);
			__atomic_store_n(&tgm_present.promise_parked,
					 tgm_promise_park(tgm_present.watch.token.queue,
							  (u32)tgm_present.watch.token.value,
							  "the display", "a direct present", NULL,
							  1) == 1,
					 __ATOMIC_RELEASE);
		} else {
			nvxnu_work_schedule_after(tgm_present.deadline_work, TGM_PRESENT_NS);
			/* before the arm: a pass claiming the watch detaches the view after this */
			tgm_present_filter_attach(&tgm_present.tokens.tokens[0]);
			switch_gpu_present_watch_arm(&tgm_present.watch,
						     &tgm_present.tokens.tokens[0]);
		}
	}
	mutex_unlock(&tgm_present.lock);
	gate_leave();
	/* After the arm and before the last look: a producer's job may end quiet, and then only an interrupt asked for it
	 * runs the pass that completes the watch (SwitchGpuQuiet.h). The caller's tokens, not the watch's, which the pass
	 * may already have dropped. */
	if (!ret)
		tgm_chan_need_tokens(kept.engine ? &kept : dependencies,
				     kept.engine ? 1 : dependency_count);
	if (!ret && present_physical(&tgm_present.watch.token))
		nvxnu_work_schedule(tgm_present.watch_work);
	return ret;
}
