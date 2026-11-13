// SPDX-License-Identifier: MIT
// Push room, job fences and submission (chan_exec).
#include "gpu_internal.h"
#include "SwitchGpuWordWait.h"

#define NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE (0x0eu << 27)
#define NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE (0x09u << 27)
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_PDB_ALL 0x1u /* GPC 1:1 0: the GPCs' TLBs too */

/* Whether nvif_chan_gpfifo_wait(gpfifo_nr, push_nr) would find its room now, counted as nvif counts it (the GET reports
 * on the semaphore page, its post size, a wrap of the push buffer that kicks one more entry). */
static int chan_room(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)
{
	struct nvif_chan *c = &chan->chan;
	struct nvif_push *push = &c->push;
	u32 post = c->func->gpfifo.post ? c->func->gpfifo.post_size : 0;
	s32 free = 0;

	if (gpfifo_nr)
		push_nr += post;
	if (push_nr)
		gpfifo_nr++;
	if (push->cur + push_nr > push->end) {
		u32 nr = push_nr + post;
		u32 cur = push->cur - (u32 *)push->mem.object.map.ptr;
		u32 get = c->func->push.read_get(c);

		if (get <= cur ? push->hw.max - cur < nr && (!get || get - 1 < nr)
			       : get - cur - 1 < nr)
			return 0;
		gpfifo_nr++;
	}
	if ((u32)c->gpfifo.free >= gpfifo_nr)
		return 1;
	free = (s32)c->func->gpfifo.read_get(c) - (s32)c->gpfifo.cur - 1;
	if (free < 0)
		free += c->gpfifo.max + 1;
	return (u32)free >= gpfifo_nr;
}

int tgm_chan_room(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)
{
	return chan_room(chan, gpfifo_nr, push_nr);
}

/* Room for gpfifo_nr entries and push_nr dwords, waited for by interrupt: every submission's GET report comes before its
 * fence's nonstall interrupt (chan_fence), whose pass ends in nvxnu_wake(), so nvif's own wait then finds the room at
 * once and never spins; a quiet newest job gets its wake entry first. The caller owns the channel's submissions and is
 * inside the gate. -ETIMEDOUT after 1 s. */
int tgm_chan_wait_ready(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)
{
	u64 end = nvxnu_nanotime() + NSEC_PER_SEC;
	int ret = 0;

	for (;;) {
		u32 seq = nvxnu_wake_seq();

		if (chan_room(chan, gpfifo_nr, push_nr))
			break;
		if (switch_gpu_quiet_need(&chan->quiet, chan->submitted))
			tgm_chan_wake_push(chan);
		if (!nvxnu_idle(seq, end) && !chan_room(chan, gpfifo_nr, push_nr)) {
			ret = -ETIMEDOUT;
			break;
		}
	}
	return ret;
}

/* Fence push: semaphore release of the next 32-bit seqno after wait-for-idle, then nvif's GET report, then a nonstall
 * interrupt (nouveau nvc0_fence_emit32) unless the job is quiet and nobody asked for it (SwitchGpuQuiet.h), then kick. The L2's dirty lines go to DRAM first, and the end of the submission
 * is stamped for the stats. The GET report comes before the interrupt so that a wait for room (chan_room) finds the room
 * this submission frees once its interrupt has woken it; nvif's own report at the kick lands after the interrupt. Space
 * was reserved by the caller (chan_wait_room / chan_push_wait): CHAN_FENCE_DWORDS. A job's internal fences
 * (chan_job_fence) released first .. next - 1 ahead of it, in the same kick, and raise no interrupt: this one announces
 * them all (the job's record says which). */
static u32 chan_fence_job(struct tgm_chan *chan, int external, u32 first, u32 next, int quiet,
			  const struct switch_gpu_release *releases, u32 release_count)
{
	struct nvif_push *push = &chan->chan.push;
	struct switch_gpu_kicked_job *job = NULL;
	const int announce = switch_gpu_quiet_announces(&chan->quiet, quiet);
	u32 s = 0;
	idle_wake(); /* backstop: tgm_chan_exec and tgm_present_copy_dependencies raise it before their first kick */
	if (external && (l2_on() || chan->kernel)) {
		/* engines idle, their writes committed to the L2 (FB flush: nvgpu gk20a_mm_fb_flush), the L2's dirty lines
		 * written to DRAM, and those writes committed before the release below. Always on the present channel: its
		 * completion promises the display every byte in DRAM (TegraScanoutCopy.h). */
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_NVSQ(push, NV906F, NVB06F_WFI, 0);
		PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
		if (external != TGM_FENCE_VOLATILE_WRITES) {
			PUSH_NVSQ(push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
				  NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY);
			PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
		}
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	}
	if (stats_on()) {
		push_stamp(push, chan_stamps_va(chan, next) + offsetof(struct tgm_stamps, end),
			   next);
	}
	/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
	/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
	if (external && chan->visible_cpu) {
		PUSH_MTHD(push, NV906F, SEMAPHOREA, upper_32_bits(chan->visible_gpu), SEMAPHOREB,
			  lower_32_bits(chan->visible_gpu), SEMAPHOREC, next, SEMAPHORED,
			  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
				  NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
				  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 4BYTE));
	}
	PUSH_MTHD(push, NV906F, SEMAPHOREA,
		  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(chan->fence_gpu)),
		  SEMAPHOREB, lower_32_bits(chan->fence_gpu), SEMAPHOREC, next, SEMAPHORED,
		  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 4BYTE));
	for (s = 0; s < release_count; s++) {
		PUSH_MTHD(push, NV906F, SEMAPHOREA, upper_32_bits(releases[s].va), SEMAPHOREB,
			  lower_32_bits(releases[s].va), SEMAPHOREC, releases[s].value, SEMAPHORED,
			  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
				  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 4BYTE));
	}
	/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	nvif_chan_gpfifo_post(&chan->chan);
	if (announce) {
		/* NOLINTNEXTLINE(bugprone-signed-bitwise): nouveau's PUSH_MTHD computes in int */
		PUSH_MTHD(push, NV906F, NON_STALL_INTERRUPT, 0);
	}
	__asm__ volatile(
		"dsb sy" ::
			: "memory"); /* client WC writes and our push reach DRAM before GP_PUT */
	chan->submit_ns = nvxnu_nanotime();
	if (stats_on())
		for (s = first; (s32)(next - s) >= 0; s++)
			WRITE_ONCE(chan->stat_sub_ns[s % CHAN_NSTAMPS], chan->submit_ns);
	if ((s32)(tgm_chan_completed(chan) - chan->submitted) >= 0) {
		WRITE_ONCE(chan->progress_held, tgm_hold_held_ns(chan->chid, chan->submit_ns));
		WRITE_ONCE(chan->busy_ns, chan->submit_ns);
	}
	job = switch_gpu_jobs_kick(&chan->jobs, first, next, chan->submit_ns, !announce);
	if (announce)
		switch_gpu_quiet_announce(&chan->quiet, next);
	if (stats_on()) {
		__atomic_fetch_add(&tgm_wstat.jobs, 1, __ATOMIC_RELAXED);
		if (!announce)
			__atomic_fetch_add(&tgm_wstat.quiet, 1, __ATOMIC_RELAXED);
	}
	if (switch_gpu_irq_arm(&chan->irq_state, next) && nvif_event_allow(&chan->nonstall))
		nvxnu_panic("SwitchGpu: cannot arm channel completion");
	WRITE_ONCE(chan->submitted, next);
	idle_kicked(first, next);
	switch_emc_gpu_submit();
	chan->chan.defer_kick = false;
	PUSH_KICK(push);
	if (chan->high) {
		struct tgm_sched_queued queued;
		enum tgm_sched_kick kick = tgm_sched_kicked(tgm_gpu.device, chan->chid, &queued);

		WRITE_ONCE(job->app, kick != TGM_KICK_CLEAR);
		if (stats_on())
			for (s = next; (s32)(s - first) >= 0; s--)
				WRITE_ONCE(chan->stat_kick[s % CHAN_NSTAMPS], kick);
		WRITE_ONCE(job->asked_pt, queued.asked_pt);
		WRITE_ONCE(job->behind, queued.behind);
		tgm_deadline_kicked(!chan->kernel, kick != TGM_KICK_CLEAR);
	}
	if (stats_on())
		idle_stats();
	return next;
}

u32 tgm_chan_fence_job(struct tgm_chan *chan, int external, u32 first, u32 next)
{
	return chan_fence_job(chan, external, first, next, 0, NULL, 0);
}

void switch_gpu_chan_semantic_visibility(struct tgm_chan *chan,
					 volatile struct TegraGM20BAbiFenceSemantic *state)
{
	switch_gpu_timeline_semantic_visibility(chan->timeline, state);
}

void switch_gpu_chan_visibility(struct tgm_chan *chan, volatile uint32_t *visible_cpu,
				uint64_t visible_gpu)
{
	*visible_cpu = chan->submitted;
	chan->visible_cpu = visible_cpu;
	chan->visible_gpu = visible_gpu;
	switch_gpu_timeline_visibility(chan->timeline, visible_cpu);
}

__attribute__((weak)) void switch_sched_render_submitted(void)
{
}

static void frame_submitted(struct tgm_chan *chan, u32 seqno, u64 start)
{
	u64 now = nvxnu_nanotime();
	u64 none = 0;

	nvxnu_kdebug(SWITCH_KDBG_SUBMIT, chan->chid, seqno, chan->critical);
	if (!chan->critical) {
		__atomic_fetch_add(&tgm_frame_gpu.others, 1, __ATOMIC_RELAXED);
		return;
	}
	switch_sched_render_submitted();
	__atomic_compare_exchange_n(&tgm_frame_gpu.first, &none, start, false, __ATOMIC_RELAXED,
				    __ATOMIC_RELAXED);
	__atomic_store_n(&tgm_frame_gpu.last, start, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_frame_gpu.submits, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_frame_gpu.exec_ns, now - start, __ATOMIC_RELAXED);
}

void tegra_gm20b_frame_take(struct SwitchFrameGpu *out)
{
#if SWITCH_FRAME_TIMELINE
	out->firstSubmit = __atomic_exchange_n(&tgm_frame_gpu.first, 0, __ATOMIC_RELAXED);
	out->lastSubmit = __atomic_exchange_n(&tgm_frame_gpu.last, 0, __ATOMIC_RELAXED);
	out->submits = __atomic_exchange_n(&tgm_frame_gpu.submits, 0, __ATOMIC_RELAXED);
	out->others = __atomic_exchange_n(&tgm_frame_gpu.others, 0, __ATOMIC_RELAXED);
	out->execNs = __atomic_exchange_n(&tgm_frame_gpu.exec_ns, 0, __ATOMIC_RELAXED);
	out->imports = __atomic_exchange_n(&tgm_frame_gpu.imports, 0, __ATOMIC_RELAXED);
	out->binds = __atomic_exchange_n(&tgm_frame_gpu.binds, 0, __ATOMIC_RELAXED);
	out->allocs = __atomic_exchange_n(&tgm_frame_gpu.allocs, 0, __ATOMIC_RELAXED);
	out->frees = __atomic_exchange_n(&tgm_frame_gpu.frees, 0, __ATOMIC_RELAXED);
	out->importNs = __atomic_exchange_n(&tgm_frame_gpu.import_ns, 0, __ATOMIC_RELAXED);
	out->bindNs = __atomic_exchange_n(&tgm_frame_gpu.bind_ns, 0, __ATOMIC_RELAXED);
	out->allocNs = __atomic_exchange_n(&tgm_frame_gpu.alloc_ns, 0, __ATOMIC_RELAXED);
	out->cleanNs = __atomic_exchange_n(&tgm_frame_gpu.clean_ns, 0, __ATOMIC_RELAXED);
	out->cleanBytes = __atomic_exchange_n(&tgm_frame_gpu.clean_bytes, 0, __ATOMIC_RELAXED);
#else
	memset(out, 0, sizeof(*out));
#endif
}

/* Present order: held by a user submission from its present wait to its kick, and by the present
 * from its snapshot of the user channels to its kick, so every user submission is either in a present's snapshot (the
 * copy waits for it) or kicked after that present (it waits for the copy). */

int tgm_backing_dependencies(struct tgm_chan *chan, const struct switch_gpu_backing_token *tokens,
			     u32 count, int emit)
{
	u32 i;
	for (i = 0; i < count; ++i) {
		const struct switch_gpu_backing_token *token = &tokens[i];
		/* A producer that failed after the ownership layer collected it leaves nothing to wait for; that layer logs it,
		 * and its readers go on. */
		if (switch_gpu_backing_token_status(token))
			continue;
		if (token->engine == SWITCH_GPU_ENGINE_GM20B) {
			struct tgm_chan *source;
			u64 va;
			mutex_lock(&tgm_live_lock);
			source = live_by_uid(token->queue);
			if (!source || token->reset_generation != (source->id >> 32) + 1 ||
			    !chan->vm->fences_va || token->value > UINT32_MAX) {
				mutex_unlock(&tgm_live_lock);
				return -ENODEV;
			}
			va = chan->vm->fences_va +
			     ((uintptr_t)source->fence_cpu - (uintptr_t)tgm_fence_pages.va);
			if (emit) {
				PUSH_MTHD(
					&chan->chan.push, NV906F, SEMAPHOREA, upper_32_bits(va),
					SEMAPHOREB, lower_32_bits(va), SEMAPHOREC,
					(u32)token->value, SEMAPHORED,
					NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
						NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
				if (chan->high)
					tgm_hold_need(tgm_gpu.device, source->chid);
			}
			mutex_unlock(&tgm_live_lock);
		} else if (token->engine == SWITCH_GPU_ENGINE_PROMISE) {
			const u64 va = chan->vm->fences_va + token->queue;

			if (!chan->vm->fences_va || token->queue >= TGM_FENCE_BYTES ||
			    token->value > UINT32_MAX)
				return -ENODEV;
			if (emit)
				PUSH_MTHD(
					&chan->chan.push, NV906F, SEMAPHOREA, upper_32_bits(va),
					SEMAPHOREB, lower_32_bits(va), SEMAPHOREC,
					(u32)token->value, SEMAPHORED,
					NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
						NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
		} else if (token->engine == SWITCH_GPU_ENGINE_VIC ||
			   token->engine == SWITCH_GPU_ENGINE_NVDEC ||
			   token->engine == SWITCH_GPU_ENGINE_DISPLAY ||
			   token->engine == SWITCH_GPU_ENGINE_NVENC) {
			u32 words[TGM_SYNCPT_WAIT_DWORDS];
			u32 n;
			if (token->queue > 255)
				return -EINVAL;
			if (emit) {
				tgm_syncpt_wait(words, token->queue, (u32)token->value);
				for (n = 0; n < TGM_SYNCPT_WAIT_DWORDS; ++n)
					PUSH_DATA(&chan->chan.push, words[n]);
			}
		} else {
			return -EOPNOTSUPP;
		}
	}
	return 0;
}

/* A job's user entries and internal fences (SwitchGpuJobFences.h), in chan_exec's reserved room. */
struct chan_job {
	struct tgm_chan *chan;
	const struct tgm_push *pushes;
	const struct tgm_prof *prof;
	u32 first;        /* the GPFIFO entry the current batch starts at, for the hang log */
	int unprefetched; /* the first push follows the L2 and TLB maintenance */
};

/* Inlined into both copies of chan_exec_body, as it was into chan_exec alone: a call per GPFIFO entry otherwise. */
static inline __attribute__((always_inline)) void chan_job_entry(void *context, u32 i)
{
	struct chan_job *job = context;
	struct tgm_chan *chan = job->chan;
	const struct tgm_push *pushes = job->pushes;
	const struct tgm_prof *prof = job->prof;

	nvif_chan_gpfifo_push(&chan->chan, pushes[i].va, pushes[i].dwords << 2,
			      pushes[i].no_prefetch || (job->unprefetched && !i));
	if (prof && chan->prof_cls[i] != TGM_PC_SKIP) {
		push_stamp(&chan->chan.push,
			   chan->ring_va + CHAN_PROF + i * sizeof(struct tgm_stamp),
			   chan->submitted + 1);
		PUSH_KICK(&chan->chan.push);
	}
}

/* A batch's end inside a job: the WFI release of its seqno, as an entry of the job's kick (deferred: no GP_PUT write; the
 * entry keeps it between the batches). No interrupt: nothing waits on a batch alone. The job's own fence announces it,
 * and the stuck rule reads it from the fence word (chan_progress_ns). */
static void chan_job_fence(void *context, u32 seqno)
{
	struct chan_job *job = context;
	struct tgm_chan *chan = job->chan;
	struct nvif_push *push = &chan->chan.push;

	tgm_hang_log_submit(&chan->hang, seqno, job->first, chan->chan.gpfifo.cur);
	if (stats_on())
		push_stamp(push, chan_stamps_va(chan, seqno) + offsetof(struct tgm_stamps, end),
			   seqno);
	/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
	/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
	PUSH_MTHD(push, NV906F, SEMAPHOREA,
		  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(chan->fence_gpu)),
		  SEMAPHOREB, lower_32_bits(chan->fence_gpu), SEMAPHOREC, seqno, SEMAPHORED,
		  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 4BYTE));
	/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	PUSH_KICK(push);
	job->first = chan->chan.gpfifo.cur;
}

/* The room chan_exec reserves, and the one a waiter for it (switch_gpu_chan_wait_submission_room) waits for: one
 * computation, so a waiter that returns leaves room the retried submission finds. nouveau_exec_job_run(): every user
 * entry plus our fence push and, each kicked as its own entry before them, a present wait, the task order's acquires
 * and (stats) a start stamp, one more nvif GET semaphore with each. A profile's stamps: an entry each, a stamp and a GET
 * semaphore. A profile is given up (*prof NULL) where chan_exec gives it up; one whose completion the stats' interrupt
 * pass no longer looks at (more than CHAN_NSTAMPS behind) does not hold the channel's. */
static void chan_submission_room(struct tgm_chan *chan, const struct tgm_push *pushes, u32 count,
				 u32 dependency_count, u32 job_fences, const struct tgm_prof **prof,
				 u32 *entries, u32 *words)
{
	u32 profiled = READ_ONCE(chan->prof_seqno);
	u32 stamps = 0;
	u32 i;

	if (profiled && (s32)(tgm_chan_completed(chan) - profiled) > CHAN_NSTAMPS)
		profiled = 0;
	if (*prof && (!stats_on() || chan->kernel || count > TGM_PROF_MAX || profiled))
		*prof = NULL;
	for (i = 0; *prof && i < count; i++)
		stamps += switch_gpu_prof_stamped(chan->critical, &pushes[i], i, count,
						  (*prof)->cls[i]);
	*entries = count + 6 + stamps + job_fences + CHAN_WAKE_ENTRIES + 1;
	*words = CHAN_FENCE_DWORDS + 64 + 5 * (TGM_ORDER_MAX + dependency_count + 1) + 16 * stamps +
		 SWITCH_GPU_FENCE_POINT_DWORDS * job_fences + CHAN_WAKE_DWORDS + 16 +
		 5 * SWITCH_GPU_MAX_RELEASES;
}

/* SubmitJob's acquires on the latest set job of the connection's other channels, in the same entry as the dependencies:
 * a sibling shares the waiter's address space, so its fence_gpu is that word's VA here. */
static inline __attribute__((always_inline)) void
chan_sibling_waits(struct tgm_chan *chan, const struct switch_gpu_sibling_wait *siblings, u32 count)
{
	u32 i;

	for (i = 0; i < count; ++i) {
		const struct tgm_chan *source = siblings[i].source;
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_MTHD(&chan->chan.push, NV906F, SEMAPHOREA, upper_32_bits(source->fence_gpu),
			  SEMAPHOREB, lower_32_bits(source->fence_gpu), SEMAPHOREC,
			  siblings[i].seqno, SEMAPHORED,
			  NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
				  NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
		if (chan->high)
			tgm_hold_need(tgm_gpu.device, source->chid);
	}
}

/* A job's acquires on event words, in the dependencies' entry: every address space maps the fence pool (tgm_fences_map). */
static inline __attribute__((always_inline)) void
chan_word_waits(struct tgm_chan *chan, const struct switch_gpu_word_wait *words, u32 count)
{
	u32 i;

	for (i = 0; i < count; ++i) {
		const u64 va = chan->vm->fences_va + words[i].pool;
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_MTHD(&chan->chan.push, NV906F, SEMAPHOREA, upper_32_bits(va), SEMAPHOREB,
			  lower_32_bits(va), SEMAPHOREC, words[i].payload, SEMAPHORED,
			  NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
				  NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
		if (chan->high) {
			struct tgm_chan *source;
			mutex_lock(&tgm_live_lock);
			source = live_by_uid(words[i].queue);
			if (source)
				tgm_hold_need(tgm_gpu.device, source->chid);
			mutex_unlock(&tgm_live_lock);
		}
	}
}

static int words_valid(const struct tgm_chan *chan, const struct switch_gpu_word_wait *words,
		       u32 count)
{
	u32 i;

	if (count && (!chan->vm->fences_va || count > TegraGM20BMaxJobEvents))
		return 0;
	for (i = 0; i < count; ++i)
		if ((words[i].pool & 3) || words[i].pool >= TGM_FENCE_BYTES)
			return 0;
	return 1;
}

/* chan_exec and switch_gpu_chan_exec_events: one body, inlined into each, so chan_exec keeps its code. */
static inline __attribute__((always_inline)) int
chan_exec_body(struct tgm_chan *chan, const struct tgm_push *pushes, uint32_t count,
	       uint32_t *seqno, const struct tgm_prof *prof,
	       const struct switch_gpu_backing_token *dependencies, u32 dependency_count,
	       int may_wait, u32 visibility, const struct switch_gpu_sibling_wait *siblings,
	       u32 sibling_count, const struct switch_gpu_word_wait *word_waits,
	       u32 word_wait_count, const struct switch_gpu_release *releases, u32 release_count)
{
	u64 start = SWITCH_FRAME_TIMELINE ? nvxnu_nanotime() : 0;
	u32 i = 0;
	u32 fences = 0;
	u32 last = 0;
	struct chan_job job = {0};
	struct switch_gpu_job_emitter emitter = {&job, chan_job_entry, chan_job_fence};
	int ret = 0;
	int tlb = 0;
	u32 waits = 0;
	u32 ring_cur;
	int ring_free;
	u32 *push_bgn, *push_cur, *push_end, *push_seg;
	u32 entries, words;
	u32 bind_target = 0;
	u64 bind_va = 0;
	int bind = 0;
	int host_written = !(visibility & SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT) ||
			   (visibility & SWITCH_GPU_SUBMIT_HOST_WRITTEN);
	int external = !(visibility & SWITCH_GPU_SUBMIT_VISIBILITY_EXPLICIT) ||
		       (visibility & SWITCH_GPU_SUBMIT_EXTERNAL_VISIBILITY) || !chan->visible_cpu;
	int quiet = (visibility & SWITCH_GPU_SUBMIT_QUIET) &&
		    __atomic_load_n(&chan->wake, __ATOMIC_ACQUIRE);

	if (chan->error)
		return -EIO;
	for (i = 0; i < count; ++i)
		if (pushes[i].reserved ||
		    (pushes[i].incomplete && (i + 1 == count || !pushes[i + 1].no_prefetch)))
			return -EINVAL;
	ret = switch_gpu_job_fences(pushes, count, &fences);
	if (ret)
		return ret;
	if (!count && !(visibility & SWITCH_GPU_SUBMIT_FORCE_FENCE)) {
		*seqno = chan->submitted;
		return 0;
	}
	if (count > TegraGM20BMaxJobPushes || dependency_count > TegraGM20BMaxJobDependencies ||
	    sibling_count > SWITCH_GPU_MAX_SIBLINGS || release_count > SWITCH_GPU_MAX_RELEASES)
		return -E2BIG;
	if (!words_valid(chan, word_waits, word_wait_count))
		return -ENODEV;
	ret = tgm_backing_dependencies(chan, dependencies, dependency_count, 0);
	if (ret)
		return ret;
	if (may_wait) {
		idle_wake();
		tgm_chan_ramfc_due(chan);
	}
	/* from its first submission: a channel made before its connection's GPU priority came (the compositor's, at boot)
	 * is never held for the compositor's own frame */
	if (chan->app && !chan->high && !chan->tracked) {
		WRITE_ONCE(chan->tracked, 1);
		tgm_hold_track(tgm_gpu.device, chan->chid, 1);
	}
	/* a push profile (see "push profile"): only with the stats' stamps, one in flight per channel; one whose completion
	 * the stats' interrupt pass no longer looks at (more than CHAN_NSTAMPS behind) is given up */
	{
		u32 ps = READ_ONCE(chan->prof_seqno);
		if (ps && (s32)(tgm_chan_completed(chan) - ps) > CHAN_NSTAMPS)
			WRITE_ONCE(chan->prof_seqno, 0);
	}
	chan_submission_room(chan, pushes, count,
			     dependency_count + sibling_count + word_wait_count, fences, &prof,
			     &entries, &words);
	if (!may_wait && !chan_room(chan, entries, words))
		return -EAGAIN;
	ret = may_wait ? tgm_chan_wait_ready(chan, entries, words) : 0;
	if (ret == -ETIMEDOUT && tgm_gpu.stage >= 3) {
		tgm_chan_stalled(chan, "exec: GPFIFO still full after 1 s", chan->submitted,
				 NSEC_PER_SEC);
	}
	if (ret)
		return READ_ONCE(chan->error) ? -EIO : ret == -ETIMEDOUT ? -EBUSY : ret;
	ring_cur = chan->chan.gpfifo.cur;
	ring_free = chan->chan.gpfifo.free;
	push_bgn = chan->chan.push.bgn;
	push_cur = chan->chan.push.cur;
	push_end = chan->chan.push.end;
	push_seg = chan->chan.push.seg;
	chan->chan.defer_kick = true;
	ret = nvif_chan_gpfifo_wait(&chan->chan, entries, words);
	if (!ret)
		ret = tgm_backing_dependencies(chan, dependencies, dependency_count, 1);
	if (!ret) {
		chan_sibling_waits(chan, siblings, sibling_count);
		chan_word_waits(chan, word_waits, word_wait_count);
	}
	/* a VA rebound over work still queued: nothing after the bind runs before retirement switched its PTEs; the TLB
	 * invalidate below runs after this acquire */
	bind = !ret && !chan->kernel && bind_pending(chan->vm, &bind_target, &bind_va);
	if (bind)
		PUSH_MTHD(&chan->chan.push, NV906F, SEMAPHOREA, upper_32_bits(bind_va), SEMAPHOREB,
			  lower_32_bits(bind_va), SEMAPHOREC, bind_target, SEMAPHORED,
			  NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
				  NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
	if (ret) {
		chan->chan.gpfifo.cur = ring_cur;
		chan->chan.gpfifo.free = ring_free;
		chan->chan.push.bgn = push_bgn;
		chan->chan.push.cur = push_cur;
		chan->chan.push.end = push_end;
		chan->chan.push.seg = push_seg;
		chan->chan.defer_kick = false;
		return ret;
	}
	PUSH_KICK(&chan->chan.push);
	tgm_chan_park_promises(chan, dependencies, dependency_count);
	if (READ_ONCE(chan->norder)) {
		/* the task's other channels' work it follows (tgm_chan_order), first, so the start stamp below times the work
		 * itself */
		waits = (u32)tgm_order_waits(chan);
	}
	/* a high channel's start stamp, in every kernel: the end of the preemption latency of an app it was queued behind */
	if (stats_on() || chan->high) {
		u32 next = chan->submitted + 1;
		if (stats_on()) {
			WRITE_ONCE(chan->stat_sub_ptimer[next % CHAN_NSTAMPS],
				   nvkm_timer_read(tgm_gpu.device->timer));
		}
		if (chan->high)
			WRITE_ONCE(switch_gpu_jobs_next(&chan->jobs)->waits, waits);
		push_stamp(&chan->chan.push,
			   chan_stamps_va(chan, next) + offsetof(struct tgm_stamps, start), next);
		PUSH_KICK(&chan->chan.push);
	}
	if (prof) {
		u32 stamps = 0;

		for (i = 0; i < count; ++i) {
			const int stamped = switch_gpu_prof_stamped(chan->critical, &pushes[i], i,
								    count, prof->cls[i]);

			chan->prof_cls[i] = stamped ? prof->cls[i] : TGM_PC_SKIP;
			stamps += stamped;
		}
		__atomic_fetch_add(&chan->prof_stamped, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_stamps, stamps, __ATOMIC_RELAXED);
		memcpy(chan->prof_draws, prof->draws, count * sizeof(prof->draws[0]));
		memcpy(chan->prof_sdraws, prof->sampleDraws, count * sizeof(prof->sampleDraws[0]));
		memcpy(chan->prof_kpx, prof->kpx, count * sizeof(prof->kpx[0]));
		memcpy(chan->prof_ilk, prof->interlocks, count);
		WRITE_ONCE(chan->prof_count, count);
		WRITE_ONCE(chan->prof_first, chan->submitted + 1);
		WRITE_ONCE(chan->prof_seqno,
			   chan->submitted + 1 + fences); /* before the fence that completes it */
	}
	/* The L2 first: page-table lines a GPU-ordered map wrote around (TegraGM20BTlb.h) go before the TLB refetches. */
	tlb = tgm_tlb_take(&chan->tlb, &chan->vm->tlb) || bind;
	if ((l2_on() && host_written) || tlb) {
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_NVSQ(&chan->chan.push, NV906F, NVB06F_WFI, 0);
		PUSH_NVSQ(&chan->chan.push, NV906F, NVB06F_FB_FLUSH, 0);
		PUSH_NVSQ(&chan->chan.push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
			  NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY);
		PUSH_NVSQ(&chan->chan.push, NV906F, NVB06F_FB_FLUSH, 0);
		PUSH_NVSQ(&chan->chan.push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
			  NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);
		if (tlb) {
			PUSH_NVSQ(&chan->chan.push, NV906F, NVB06F_MEM_OP_C,
				  NVB06F_MEM_OP_C_TLB_INVALIDATE_PDB_ALL, NVB06F_MEM_OP_D,
				  NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE);
		}
		PUSH_KICK(&chan->chan.push);
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	}
	job.chan = chan;
	job.pushes = pushes;
	job.prof = prof;
	job.first = chan->chan.gpfifo.cur;
	job.unprefetched = (l2_on() && host_written) || tlb;
	if (stats_on()) {
		WRITE_ONCE(chan->stat_job[(chan->submitted + 1) % CHAN_NSTAMPS].last,
			   chan->submitted + 1 + fences);
		WRITE_ONCE(chan->stat_job[(chan->submitted + 1) % CHAN_NSTAMPS].first,
			   chan->submitted + 1);
	}
	last = switch_gpu_job_emit(pushes, count, chan->submitted, &emitter);
	tgm_hang_log_submit(&chan->hang, last, job.first, chan->chan.gpfifo.cur);

	*seqno = chan_fence_job(chan, external, chan->submitted + 1, last, quiet, releases,
				release_count);
	if (SWITCH_FRAME_TIMELINE && !chan->kernel)
		frame_submitted(chan, *seqno, start);
	if (stats_on())
		tgm_stats_tick();
	return 0;
}

static int chan_exec(struct tgm_chan *chan, const struct tgm_push *pushes, uint32_t count,
		     uint32_t *seqno, const struct tgm_prof *prof,
		     const struct switch_gpu_backing_token *dependencies, u32 dependency_count,
		     int may_wait, u32 visibility)
{
	return chan_exec_body(chan, pushes, count, seqno, prof, dependencies, dependency_count,
			      may_wait, visibility, NULL, 0, NULL, 0, NULL, 0);
}

int tgm_chan_exec(struct tgm_chan *chan, const struct tgm_push *pushes, uint32_t count,
		  uint32_t *seqno, const struct tgm_prof *prof)
{
	int ret = 0;

	tgm_gate_enter();
	ret = chan_exec(chan, pushes, count, seqno, prof, NULL, 0, 1, 0);
	gate_leave();
	return ret;
}

void switch_gpu_chan_submit_enter(struct tgm_chan *chan)
{
	tgm_gate_enter();
	idle_wake();
	tgm_chan_ramfc_due(chan);
}

void switch_gpu_chan_submit_leave(void)
{
	gate_leave();
}

int switch_gpu_chan_wait_submission_room(struct tgm_chan *chan, const struct tgm_push *pushes,
					 u32 count, u32 dependency_count,
					 const struct tgm_prof *prof)
{
	u32 fences = 0;
	u32 entries = 0;
	u32 words = 0;
	int ret;
	if (count > TegraGM20BMaxJobPushes || dependency_count > TegraGM20BMaxJobDependencies)
		return -E2BIG;
	if (switch_gpu_job_fences(pushes, count, &fences))
		return -EINVAL;
	chan_submission_room(chan, pushes, count, dependency_count, fences, &prof, &entries,
			     &words);
	ret = tgm_chan_wait_ready(chan, entries, words);
	if (ret == -ETIMEDOUT && tgm_gpu.stage >= 3)
		tgm_chan_stalled(chan, "exec: GPFIFO still full after 1 s", chan->submitted,
				 NSEC_PER_SEC);
	return READ_ONCE(chan->error) ? -EIO : ret == -ETIMEDOUT ? -EBUSY : ret;
}

int switch_gpu_chan_exec(struct tgm_chan *chan, const struct tgm_push *pushes, uint32_t count,
			 const struct switch_gpu_backing_token *dependencies,
			 uint32_t dependency_count, uint32_t *seqno, const struct tgm_prof *prof)
{
	return chan_exec(chan, pushes, count, seqno, prof, dependencies, dependency_count, 0, 0);
}

int switch_gpu_chan_exec_visibility(struct tgm_chan *chan, const struct tgm_push *pushes, u32 count,
				    const struct switch_gpu_backing_token *dependencies,
				    u32 dependency_count, u32 *seqno, const struct tgm_prof *prof,
				    u32 visibility)
{
	return chan_exec(chan, pushes, count, seqno, prof, dependencies, dependency_count, 0,
			 visibility);
}

int switch_gpu_chan_exec_events(struct tgm_chan *chan, const struct tgm_push *pushes, u32 count,
				const struct switch_gpu_backing_token *dependencies,
				u32 dependency_count,
				const struct switch_gpu_sibling_wait *siblings, u32 sibling_count,
				const struct switch_gpu_word_wait *words, u32 word_count,
				u32 *seqno, const struct tgm_prof *prof, u32 visibility,
				const struct switch_gpu_release *releases, u32 release_count)
{
	return chan_exec_body(chan, pushes, count, seqno, prof, dependencies, dependency_count, 0,
			      visibility, siblings, sibling_count, words, word_count, releases,
			      release_count);
}

int switch_gpu_chan_exec_job(struct tgm_chan *chan, const struct tgm_push *pushes, u32 count,
			     const struct switch_gpu_backing_token *dependencies,
			     u32 dependency_count, const struct switch_gpu_sibling_wait *siblings,
			     u32 sibling_count, u32 *seqno, const struct tgm_prof *prof,
			     u32 visibility, const struct switch_gpu_release *releases,
			     u32 release_count)
{
	return switch_gpu_chan_exec_events(chan, pushes, count, dependencies, dependency_count,
					   siblings, sibling_count, NULL, 0, seqno, prof,
					   visibility, releases, release_count);
}

void switch_gpu_chan_event_releases(struct tgm_chan *chan, u32 seqno)
{
	WRITE_ONCE(chan->event_through, seqno);
}
