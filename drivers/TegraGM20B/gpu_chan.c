// SPDX-License-Identifier: MIT
// Channel creation and teardown, timeslices and fence pages.
#include "gpu_internal.h"

#define CHAN_SEMA_BYTES \
	0x2000 /* nvif's GET semaphore at +0; stats stamps at CHAN_STAMPS, push profile at CHAN_PROF */
#define CHAN_BYTES (CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES + CHAN_SEMA_BYTES)

/* The interrupt pass's own nvxnu_wake() at its end wakes the kernel's waits (channel teardown, sync_for_cpu, system
 * sleep's drain); a channel's fence waiters sleep on its word and wake only once its fence reaches the lowest seqno one
 * of them waits for. */
static void chan_wake_waiters(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan);

	if (done == chan->woken_done)
		return;
	chan->woken_done = done;
	if (SWITCH_FRAME_TIMELINE)
		nvxnu_kdebug(SWITCH_KDBG_GPU_DONE, chan->chid, done, chan->critical);
	if (switch_gpu_waiter_reached(&chan->wanted, done))
		chan_wake_all_waiters(chan);
}

void tgm_chan_want(struct tgm_chan *chan, uint32_t seqno)
{
	switch_gpu_waiter_want(&chan->wanted, seqno);
	tgm_chan_need(chan, seqno);
}

int tgm_chan_announced(struct tgm_chan *chan, uint32_t seqno)
{
	int announced = 0;
	const u32 end = switch_gpu_jobs_announced_by(&chan->jobs, seqno, &announced);

	return announced && (s32)(tgm_chan_completed(chan) - end) >= 0;
}

#include "SwitchGpuChannelCompletion.h"

void tgm_chan_on_fence(struct tgm_chan *chan, void (*fenced)(void *arg), void *arg)
{
	chan->fenced_arg = arg;
	__atomic_store_n(&chan->fenced, fenced, __ATOMIC_RELEASE);
}

static void chan_free(struct tgm_chan *chan)
{
	int i = 0;
	tgm_fault_unpublish(chan);
	if (stats_on())
		tgm_stats_gr_unbind(chan);
	if (chan->high || chan->tracked)
		tgm_sched_forget(tgm_gpu.device, chan->chid);
	nvif_event_dtor(&chan->nonstall);
	nvif_event_dtor(&chan->kill);
	switch_gpu_timeline_close(chan->timeline);
	chan->timeline = NULL;
	for (i = chan->nobj - 1; i >= 0; i--)
		nvif_object_dtor(&chan->obj[i]);
	nvif_object_unmap(&chan->user);
	nvif_object_dtor(&chan->user);
	/* the channel is gone: unmap its ring (TLB invalidated) before the ring's pages go back to the pool */
	nvif_vmm_put(&chan->vm->vmm, &chan->ring_vma);
	if (chan->ring_mem) {
		struct tgm_mem *m = chan->ring_mem;
		mem_big_views_drop(m);
		nvif_mem_dtor(&m->mem);
		kfree(m);
	}
	if (chan->ring)
		dma_free_attrs(&tgm_gpu_pdev.dev, CHAN_BYTES, chan->ring, chan->ring_pa, 0);
	kfree(chan->ring_pas);
	kfree(chan);
}

/* Compositor priority: nvkm's runlist has no priorities ("TODO: prio, interleaving",
 * fifo/nv50.c nv50_runl_update) and gives every channel RAMFC's default timeslice, 128 << 3 (fifo/gk104.c
 * gk104_chan_ramfc_write, RAMFC 0xf8 = PFIFO 0x2310: enable 28, scale 15:12, timeout 7:0), as nvgpu's default
 * (gk20a/fifo_gk20a.c gk20a_fifo_default_timeslice_us). nvgpu gives high-priority work 4x its low timeslice
 * (os/linux/driver_common.c nvgpu_init_timeslice: 5200 vs 1300 us); critical channels get scale 5, 4x, written before
 * their first submission (the host reads RAMFC when it first loads the channel). */
#define RAMFC_TIMESLICE 0xf8
#define RAMFC_TIMESLICE_DEFAULT 0x10003080
#define RAMFC_TIMESLICE_HIGH 0x10005080

static int compositor_timeslice_on(void)
{
	return 1;
}

static void chan_timeslice(u32 chid, u32 value)
{
	tgm_ramfc_write(chid, RAMFC_TIMESLICE, value);
}

void tgm_chan_set_level(struct tgm_chan *chan, unsigned int level)
{
	int high = level == TGM_LEVEL_HIGH;
	u64 now = nvxnu_nanotime();
	u32 i = 0;

	if (chan->kernel)
		return;
	if (high == chan->high) {
		tgm_sched_level(tgm_gpu.device, chan->chid, level);
		return;
	}
	if (high) {
		/* work kicked before the change found no preempt: none of it is timed as the compositor's */
		for (i = 0; i < SWITCH_GPU_JOBS; i++) {
			WRITE_ONCE(chan->jobs.job[i].kick_ns, now);
			WRITE_ONCE(chan->jobs.job[i].behind, -1);
			WRITE_ONCE(chan->jobs.job[i].app, 1);
			WRITE_ONCE(chan->jobs.job[i].waits, 1);
		}
		WRITE_ONCE(chan->tracked, 0);
		chan->critical = 1;
		/* chan_nonstall reads the job records once it sees high */
		__atomic_store_n(&chan->high, 1, __ATOMIC_RELEASE);
		tgm_sched_level(tgm_gpu.device, chan->chid, TGM_LEVEL_HIGH);
	} else {
		WRITE_ONCE(chan->high, 0);
		chan->critical = 0;
		tgm_sched_level(tgm_gpu.device, chan->chid, level);
	}
	if (compositor_timeslice_on())
		chan->timeslice_due = high ? RAMFC_TIMESLICE_HIGH : RAMFC_TIMESLICE_DEFAULT;
}

/* chan_exec, before a submission is kicked: the host saves a channel's RAMFC when it leaves it, so a level change's
 * timeslice, and the acquire timeout an ACQUIRE interrupt turned off, are written while the channel is idle and on
 * neither GR nor the PBDMA. */
void tgm_chan_ramfc_due(struct tgm_chan *chan)
{
	u32 value = chan->timeslice_due;
	int acquire = READ_ONCE(chan->acquire_due);

	if ((!value && !acquire) || (s32)(tgm_chan_completed(chan) - chan->submitted) < 0 ||
	    tgm_sched_loaded(tgm_gpu.device, chan->chid))
		return;
	if (value)
		chan_timeslice(chan->chid, value);
	chan->timeslice_due = 0;
	if (acquire) {
		tgm_acquire_arm(chan->chid);
		WRITE_ONCE(chan->acquire_due, 0);
	}
}

static int chan_new(struct tgm_vm *vm, const void *vm_owner, volatile uint32_t *fence_cpu,
		    uint64_t fence_gpu_va, const char *owner, int critical, int copy_only,
		    struct tgm_chan **pchan)
{
	static const u32 classes[5] = {MAXWELL_B, MAXWELL_COMPUTE_B, KEPLER_INLINE_TO_MEMORY_B,
				       FERMI_TWOD_A, MAXWELL_DMA_COPY_A};

	union {
		struct nvif_chan_v0 chan;
		u8 raw[sizeof(struct nvif_chan_v0) + 32];
	} args = {};

	union {
		struct nvif_event_v0 ev;
		u8 raw[sizeof(struct nvif_event_v0) + sizeof(struct nvif_chan_event_v0)];
	} kargs = {};
	struct nvif_chan_event_v0 *host = (struct nvif_chan_event_v0 *)kargs.ev.data;
	struct gf100_vmm_map_v0 margs = {.version = 0, .vol = 1};
	struct nvif_push *push = NULL;
	struct tgm_chan *chan = NULL;
	u64 runm = 0;
	u64 locked = 0;
	u32 i = 0;
	u32 npages = (u32)CHAN_BYTES >> PAGE_SHIFT;
	u32 bind_words[10];
	u32 bind_count = switch_gpu_channel_bind_words(bind_words, classes, copy_only);
	int ret = 0;

	if (tgm_gpu.stage < 2)
		return -ENODEV;
	/* BoFree's zombies wait for the claimant's channels only: a space is addressed by its claimant's alone */
	if (vm->owner != vm_owner)
		return -EINVAL;
	chan = kzalloc(sizeof(*chan), GFP_KERNEL);
	if (!chan)
		return -ENOMEM;
	chan->vm = vm;
	tgm_tlb_chan_init(&chan->tlb);
	chan->fence_cpu = fence_cpu;
	chan->fence_gpu = fence_gpu_va;
	chan->critical = critical;
	strscpy(chan->owner, owner, sizeof(chan->owner));
	/* a fence word only moves forward: a channel made in a slot continues its predecessor's sequence, so a semaphore
	 * acquire on the old seqno (the GPU present) is never left waiting for a word that went back to 0 */
	chan->submitted = READ_ONCE(*fence_cpu);
	chan->event_through = chan->submitted;
	switch_gpu_jobs_init(&chan->jobs, chan->submitted);
	switch_gpu_quiet_init(&chan->quiet, chan->submitted);
	chan->stat_done = chan->submitted;
	chan->woken_done = chan->submitted;
	chan->gov_done = chan->submitted;
	chan->progress_done = chan->submitted;
	chan->seen_done = chan->submitted;
	chan->ring = dma_alloc_attrs(&tgm_gpu_pdev.dev, CHAN_BYTES, &chan->ring_pa, GFP_KERNEL,
				     DMA_ATTR_WRITE_COMBINE);
	chan->ring_pas = kcalloc(npages, sizeof(u64), GFP_KERNEL);
	chan->ring_mem = kzalloc(sizeof(*chan->ring_mem), GFP_KERNEL);
	if (!chan->ring || !chan->ring_pas || !chan->ring_mem) {
		chan_free(chan);
		return -ENOMEM;
	}
	for (i = 0; i < npages; i++)
		chan->ring_pas[i] = chan->ring_pa + ((u64)i << PAGE_SHIFT);

	tgm_gpu_lock();
	locked = stats_on() ? nvxnu_nanotime() : 0;
	{
		struct nvif_mem_ram_v0 rargs = {.version = 0, .dma = (dma_addr_t *)chan->ring_pas};
		chan->ring_mem->bytes = CHAN_BYTES;
		ret = nvif_mem_ctor_type(&tgm_gpu.mmu, "chanRing", NVIF_CLASS_MEM_GF100,
					 tgm_gpu.mem_type, PAGE_SHIFT, CHAN_BYTES, &rargs,
					 sizeof(rargs), &chan->ring_mem->mem);
		if (ret) {
			kfree(chan->ring_mem);
			chan->ring_mem = NULL;
		}
	}
	if (!ret) {
		ret = nvif_vmm_get(&vm->vmm, ADDR, false, PAGE_SHIFT, 0, CHAN_BYTES,
				   &chan->ring_vma);
	}
	if (!ret) {
		ret = nvif_vmm_map(&vm->vmm, chan->ring_vma.addr, CHAN_BYTES, &margs, sizeof(margs),
				   &chan->ring_mem->mem, 0);
		chan->ring_va = chan->ring_vma.addr;
	}
	if (ret)
		goto fail;

	/* nouveau_channel_ctor(): GPFIFO channel on the GR runlist, in this VM, GPFIFO after the pushbuf */
	runm = nvif_fifo_runlist(&tgm_gpu.dev, NV_DEVICE_HOST_RUNLIST_ENGINES_GR);
	args.chan.version = 0;
	args.chan.namelen = 32;
	args.chan.runlist = runm ? __ffs64(runm) : 0;
	args.chan.runq = 0;
	args.chan.priv = 0;
	/* NOLINTNEXTLINE(bugprone-signed-bitwise): nouveau's BIT is int */
	args.chan.devm = BIT(0);
	args.chan.vmm = nvif_handle(&vm->vmm.object);
	args.chan.ctxdma = 0;
	args.chan.offset = chan->ring_va + CHAN_PUSH_BYTES;
	args.chan.length = CHAN_GPFIFO_BYTES;
	args.chan.huserd = 0;
	args.chan.ouserd = 0;
	snprintf((char *)args.chan.name, 32, "%s", chan->owner); /* nvkm names it in its reports */
	ret = nvif_object_ctor(&tgm_gpu.dev.object, "chan", 0, MAXWELL_CHANNEL_GPFIFO_A, &args,
			       sizeof(args), &chan->user);
	chan->chid = args.chan.chid;
	if (!ret)
		tgm_acquire_arm(chan->chid);
	if (!ret && critical && tgm_all_clients() && compositor_timeslice_on())
		chan_timeslice(chan->chid, RAMFC_TIMESLICE_HIGH);
	if (!ret && stats_on())
		tgm_stats_gr_bind(chan);
	if (!ret && critical && vm_owner)
		chan_prioritize(chan);
	if (!ret)
		ret = nvif_object_map(&chan->user, NULL, 0); /* USERD (BAR1) for GP_PUT */
	if (ret)
		goto fail;

	/* CE-only channels share the verified runlist but never instantiate a GR context. */
	for (i = switch_gpu_channel_first_object(copy_only); i < 5; i++) {
		ret = nvif_object_ctor(&chan->user, "obj", 0x00010000 + i, classes[i], NULL, 0,
				       &chan->obj[chan->nobj]);
		if (ret) {
			pr_err("class %04x on channel: %d\n", classes[i], ret);
			goto fail;
		}
		chan->nobj++;
	}
	host->version = 0;
	host->type = NVIF_CHAN_EVENT_V0_KILLED;
	ret = nvif_event_ctor(&chan->user, "chanKilled", args.chan.chid, tgm_chan_killed, false,
			      &kargs.ev, sizeof(kargs), &chan->kill);
	if (!ret)
		ret = nvif_event_allow(&chan->kill);
	if (ret)
		goto fail;
	/* nouveau_fence_context_new(): the fence's nonstall interrupt (uchan.c NVIF_CHAN_EVENT_V0_NON_STALL_INTR); its
	 * allow sets PFIFO_INTR_EN_0 bit 31 (gf100_fifo_nonstall_allow). Without it, fences are still seen by polling. */
	host->type = NVIF_CHAN_EVENT_V0_NON_STALL_INTR;
	ret = nvif_event_ctor(&chan->user, "fenceNonStallIntr", args.chan.chid, chan_nonstall, true,
			      &kargs.ev, sizeof(kargs), &chan->nonstall);
	if (ret)
		goto fail;
	ret = nvif_chan906f_ctor(&chan->chan, chan->user.map.ptr,
				 (u8 *)chan->ring + CHAN_PUSH_BYTES, CHAN_GPFIFO_BYTES, chan->ring,
				 chan->ring_va, CHAN_PUSH_BYTES,
				 (u8 *)chan->ring + CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES,
				 chan->ring_va + CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES);
	if (stats_on()) {
		__atomic_fetch_add(&tgm_stats.lock_new, 1, __ATOMIC_RELAXED);
		stats_add(&tgm_stats.lock_new_sum, &tgm_stats.lock_new_max,
			  nvxnu_nanotime() - locked);
	}
	mutex_unlock(&tgm_gpu.lock);
	if (ret) {
		tgm_gpu_lock();
		goto fail;
	}

	push = &chan->chan.push;
	ret = chan_push_wait(chan, bind_count);
	if (ret) {
		tgm_gpu_lock();
		goto fail;
	}
	for (i = 0; i < bind_count; i++)
		PUSH_DATA(push, bind_words[i]);
	PUSH_KICK(push);
	WRITE_ONCE(chan->progress_held, tgm_hold_held_ns(chan->chid, nvxnu_nanotime()));
	chan->app = vm_owner != NULL;
	ret = switch_gpu_timeline_init();
	if (ret) {
		tgm_gpu_lock();
		goto fail;
	}
	mutex_lock(&tgm_live_lock);
	chan->id = ++tgm_live_ids;
	chan->timeline = switch_gpu_timeline_create(chan->id, chan->fence_cpu);
	if (!chan->timeline) {
		mutex_unlock(&tgm_live_lock);
		ret = -ENOMEM;
		tgm_gpu_lock();
		goto fail;
	}
	list_add_tail(&chan->live, &tgm_live_chans);
	tgm_fault_publish(chan);
	mutex_unlock(&tgm_live_lock);
	*pchan = chan;
	return 0;

fail:
	mutex_unlock(&tgm_gpu.lock);
	chan_free(chan);
	return ret;
}

/* The gate: its subchannel binds are kicked after tgm_gpu.lock is dropped. */
int tgm_chan_new(struct tgm_vm *vm, const void *vm_owner, volatile uint32_t *fence_cpu,
		 uint64_t fence_gpu_va, const char *owner, int critical, struct tgm_chan **pchan)
{
	int ret = 0;

	tgm_gate_enter();
	ret = chan_new(vm, vm_owner, fence_cpu, fence_gpu_va, owner, critical, 0, pchan);
	gate_leave();
	return ret;
}

int tgm_chan_new_copy(struct tgm_vm *vm, const void *vm_owner, volatile uint32_t *fence_cpu,
		      uint64_t fence_gpu_va, const char *owner, int critical,
		      struct tgm_chan **pchan)
{
	tgm_gate_enter();
	const int ret = chan_new(vm, vm_owner, fence_cpu, fence_gpu_va, owner, critical, 1, pchan);
	gate_leave();
	return ret;
}

static void chan_abandon(struct tgm_chan *chan)
{
	u64 now = nvxnu_nanotime();
	u64 end = now + TGM_ISOLATE_NVKM_NS;

	if (chan->kernel)
		tgm_chan_hung(chan, "channel destroy", chan->submitted, NSEC_PER_SEC);
	mutex_lock(&tgm_live_lock);
	tgm_chan_isolate(chan, HANG_REPORT "destroyed with its work still running 1 s later", now);
	mutex_unlock(&tgm_live_lock);
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if (READ_ONCE(chan->error))
			return;
		if (!nvxnu_idle(seq, end) && !READ_ONCE(chan->error)) {
			tgm_chan_hung(chan, "channel destroy: kill", chan->submitted,
				      nvxnu_nanotime() - now);
		}
	}
}

void tgm_chan_del(struct tgm_chan **pchan)
{
	struct tgm_chan *chan = *pchan;
	u32 waiter = 0;
	u64 end = 0;
	u64 held = 0;
	if (!chan)
		return;
	waiter = tgm_chan_waiter(chan);
	if (chan->tracked)
		tgm_hold_track(tgm_gpu.device, chan->chid, 0);
	/* bounded wait for idle (CONTRACT.md: <= 1 s), then nvkm tears the channel down (preempt + runlist). The caller owns
	 * the channel's submissions: a quiet last job gets its wake entry here. */
	end = nvxnu_nanotime() + NSEC_PER_SEC;
	if ((s32)(tgm_chan_completed(chan) - chan->submitted) < 0 &&
	    switch_gpu_quiet_need(&chan->quiet, chan->submitted))
		tgm_chan_quiet_wake(chan);
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if (chan->error || (s32)(tgm_chan_completed(chan) - chan->submitted) >= 0 ||
		    nvxnu_nanotime() >= end)
			break;
		if (!nvxnu_idle(seq, end) && tgm_chan_announced(chan, chan->submitted))
			nvxnu_wake_due(seq, "fence", chan->submitted);
	}
	/* tearing down a channel that still runs would free pages the GPU is using: its owner is gone, so it is killed
	 * as a hung client's is, and the teardown waits for that */
	if (tgm_gpu.stage >= 3 && !chan->error &&
	    (s32)(tgm_chan_completed(chan) - chan->submitted) < 0)
		chan_abandon(chan);
	/* A watched producer whose fence passed with its pass still to come (an asked wake entry not taken yet) gets no
	 * pass of this channel after the teardown: the teardown hands the watch to its work item, as a kill does. */
	if (!chan->kernel)
		present_watch_fenced(chan, 1);
	/* the idle clock's count: what it finished, and (a dead channel) what it never will */
	idle_retire(chan, chan->submitted);
	mutex_lock(&tgm_live_lock);
	list_del(&chan->live);
	order_forget(chan);
	mutex_unlock(&tgm_live_lock);
	tgm_gpu_lock();
	held = stats_on() ? nvxnu_nanotime() : 0;
	chan_free(chan);
	if (stats_on()) {
		__atomic_fetch_add(&tgm_stats.lock_free, 1, __ATOMIC_RELAXED);
		stats_add(&tgm_stats.lock_free_sum, &tgm_stats.lock_free_max,
			  nvxnu_nanotime() - held);
	}
	mutex_unlock(&tgm_gpu.lock);
	*pchan = NULL;
	nvxnu_word_wake(&tgm_waiter_words[waiter]);
}
struct fences_state tgm_fence_pages;
u64 tgm_fences_pas
	[TGM_FENCE_GPU_PAGES]; /* the range's GPU pages, for the address spaces that map all of it */

/* tgm_live_lock held: the fence range, made once and kept */
static int fences_alloc(void)
{
	u32 i = 0;

	if (tgm_fence_pages.va)
		return 0;
	tgm_fence_pages.va = tgm_pool_alloc(TGM_FENCE_BYTES, 0, &tgm_fence_pages.pa);
	if (!tgm_fence_pages.va)
		return -ENOMEM;
	for (i = 0; i < TGM_FENCE_GPU_PAGES; i++)
		tgm_fences_pas[i] = tgm_fence_pages.pa + i * PAGE_SIZE;
	return 0;
}

int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)
{
	u32 i = 0;
	u32 n = 0;
	int ret = -ENOMEM;

	mutex_lock(&tgm_live_lock);
	for (n = 0; !fences_alloc() && n < TGM_FENCE_USER_PAGES; n++) {
		i = (tgm_fence_pages.next + n) % TGM_FENCE_USER_PAGES;
		if (__atomic_load_n(&tgm_fence_pages.used, __ATOMIC_ACQUIRE) & BIT_ULL(i))
			continue;
		__atomic_fetch_or(&tgm_fence_pages.used, BIT_ULL(i), __ATOMIC_ACQ_REL);
		__atomic_store_n(&tgm_fence_pages.refs[i], 1, __ATOMIC_RELEASE);
		__atomic_store_n(&tgm_fence_pages.owner[i], tgm_fence_pages.owner[i] + 1,
				 __ATOMIC_RELEASE);
		tgm_fence_pages.next = i + 1;
		memset((u8 *)tgm_fence_pages.va + i * TGM_FENCE_SLOT, 0, PAGE_SIZE);
		memset(tgm_fence_pages.made[i], 0, sizeof(tgm_fence_pages.made[i]));
		tgm_clock_anchors_fill(i);
		*va = (volatile uint32_t *)((u8 *)tgm_fence_pages.va + i * TGM_FENCE_SLOT);
		*pa = tgm_fence_pages.pa + i * TGM_FENCE_SLOT;
		ret = 0;
		break;
	}
	mutex_unlock(&tgm_live_lock);
	return ret;
}

void tgm_fence_page_put(const volatile uint32_t *va)
{
	switch_gpu_fence_put(va);
}

void switch_gpu_fence_hold(const volatile uint32_t *va)
{
	uintptr_t off = (uintptr_t)va - (uintptr_t)tgm_fence_pages.va;
	if (va && off / TGM_FENCE_SLOT < TGM_FENCE_USER_PAGES)
		__atomic_add_fetch(&tgm_fence_pages.refs[off / TGM_FENCE_SLOT], 1,
				   __ATOMIC_ACQ_REL);
}

volatile uint32_t *tgm_fence_word(uint32_t offset)
{
	return tgm_fence_pages.va && offset < TGM_FENCE_USER_PAGES * TGM_FENCE_SLOT && !(offset & 3)
		       ? (volatile uint32_t *)((u8 *)tgm_fence_pages.va + offset)
		       : NULL;
}

uint32_t tgm_fence_owner(uint32_t offset)
{
	return offset < TGM_FENCE_USER_PAGES * TGM_FENCE_SLOT
		       ? __atomic_load_n(&tgm_fence_pages.owner[offset / TGM_FENCE_SLOT],
					 __ATOMIC_ACQUIRE)
		       : 0;
}

uint32_t tgm_fence_offset(const volatile uint32_t *word)
{
	return (uint32_t)((uintptr_t)word - (uintptr_t)tgm_fence_pages.va);
}

void switch_gpu_fence_put(const volatile uint32_t *va)
{
	uintptr_t off = (uintptr_t)va - (uintptr_t)tgm_fence_pages.va;
	if (va && off / TGM_FENCE_SLOT < TGM_FENCE_USER_PAGES &&
	    !__atomic_sub_fetch(&tgm_fence_pages.refs[off / TGM_FENCE_SLOT], 1, __ATOMIC_ACQ_REL))
		__atomic_fetch_and(&tgm_fence_pages.used, ~BIT_ULL(off / TGM_FENCE_SLOT),
				   __ATOMIC_ACQ_REL);
}

uint64_t switch_gpu_chan_identity(struct tgm_chan *chan)
{
	return chan->id;
}

void switch_gpu_chan_token(struct tgm_chan *chan, uint32_t seqno,
			   struct switch_gpu_backing_token *token)
{
	switch_gpu_timeline_token(chan->timeline, seqno, token);
}

void switch_gpu_chan_next_token(struct tgm_chan *chan, struct switch_gpu_backing_token *token)
{
	switch_gpu_timeline_token(chan->timeline, chan->submitted + 1, token);
}

void switch_gpu_chan_job_token(struct tgm_chan *chan, uint32_t fences,
			       struct switch_gpu_backing_token *token)
{
	switch_gpu_timeline_token(chan->timeline, chan->submitted + fences + 1, token);
}

int tgm_fence_kernel(unsigned int which, volatile uint32_t **va, uint64_t *page_pa,
		     uint32_t *offset)
{
	const size_t off =
		TGM_FENCE_USER_PAGES * TGM_FENCE_SLOT + which * sizeof(struct TegraGM20BAbiFence);
	int ret = 0;

	if (which >= TGM_FENCE_KERNEL_CHANNELS)
		return -EINVAL;
	mutex_lock(&tgm_live_lock);
	ret = fences_alloc();
	mutex_unlock(&tgm_live_lock);
	if (ret)
		return ret;
	*va = (volatile uint32_t *)((u8 *)tgm_fence_pages.va + off);
	*page_pa = tgm_fence_pages.pa + (off & ~(PAGE_SIZE - 1));
	*offset = (u32)(off & (PAGE_SIZE - 1));
	return 0;
}

/* order between the channels of a task:
 * Two drivers in one process (the render server's raw GM20B path and NVK) each have their own connection, address space and
 * channels, and Metal's order between their command buffers is the order the process submits them in. tgm_chan_order
 * (TegraGM20BABI.h method 14) keeps it on the GPU, with no CPU wait: it queues semaphore ACQUIRE_GEQs (acquire switch on, as
 * the present's) on the fence words of the task's other channels, which the waiting channel's next tgm_chan_exec kicks as an
 * entry of their own ahead of its pushes. Every address space maps the whole fence pool read-only for them (tgm_fences_map, from
 * tgm_vm_new), as the present's does. Each acquire names a seqno already submitted when it was queued (AfterTask: the other
 * channels' last; BeforeTask: one up to the channel's last), and a channel's entries run in order, so no two submissions can
 * wait for each other. A source channel's teardown waits for it to go idle and drops what was queued on it (order_forget),
 * so no acquire is left on a fence page handed out again; one already kicked has passed by then. A waiter stuck on one is a
 * hang like any other (tgm_chan_check_progress), and its line names the acquire (tgm_chan_hung). */
static DEFINE_MUTEX(fences_mem_lock);
struct tgm_mem *tgm_fences_mem; /* the whole pool as GPU memory, made once and kept */

int tgm_fences_map(struct tgm_vm *vm)
{
	int ret = 0;

	mutex_lock(&fences_mem_lock);
	if (!tgm_fences_mem) {
		/* the pool exists from the first connection on; make sure it does now */
		mutex_lock(&tgm_live_lock);
		ret = fences_alloc();
		mutex_unlock(&tgm_live_lock);
		if (!ret)
			ret = tgm_mem_new(vm, tgm_fences_pas, TGM_FENCE_GPU_PAGES, &tgm_fences_mem);
	}
	mutex_unlock(&fences_mem_lock);
	if (!ret)
		ret = tgm_vm_kmap_raw(vm, tgm_fences_mem, TGM_FENCE_BYTES, 1, &vm->fences_va);
	return ret;
}

void tgm_chan_set_group(struct tgm_chan *chan, const void *group)
{
	mutex_lock(&tgm_live_lock);
	chan->group = group;
	mutex_unlock(&tgm_live_lock);
}
