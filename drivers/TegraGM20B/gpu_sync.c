// SPDX-License-Identifier: MIT
// Channel progress and order, CPU syncs and GPU writers.
#include "gpu_internal.h"

uint32_t tgm_chan_submitted(struct tgm_chan *chan)
{
	return chan->submitted;
}

uint32_t tgm_chan_completed(struct tgm_chan *chan)
{
	return READ_ONCE(*chan->fence_cpu);
}

uint32_t tgm_chan_error(struct tgm_chan *chan)
{
	return chan->error;
}

/* The target is released after the caller's snapshot of each channel's submitted seqno and acquired before
 * chan_exec moves its channel's: a submission that waits for this epoch is never in that snapshot. */
void tgm_vm_bind_wait(struct tgm_vm *vm, uint64_t va, const volatile uint32_t *cpu, uint32_t target)
{
	WRITE_ONCE(vm->bind_va, va);
	__atomic_store_n(&vm->bind_target, target, __ATOMIC_RELEASE);
	__atomic_store_n(&vm->bind_cpu, cpu, __ATOMIC_RELEASE);
}

uint32_t tgm_chan_settled(struct tgm_chan *chan, int *failed)
{
	const u32 done = __atomic_load_n(chan->fence_cpu, __ATOMIC_ACQUIRE);

	*failed = READ_ONCE(chan->error) != 0;
	return done;
}

uint64_t tgm_chan_fault_va(struct tgm_chan *chan)
{
	return READ_ONCE(chan->fault_va);
}
#if TGM_STATS
uint64_t tgm_chan_killed_ns(struct tgm_chan *chan)
{
	return READ_ONCE(chan->killed_ns);
}

#endif
uint32_t tgm_chan_waiter(struct tgm_chan *chan)
{
	return (uint32_t)(chan->id % TGM_WAITERS);
}

/* tgm_live_lock held: `waiter`'s next submission waits for `source` to complete `seqno` (one entry per source, the later seqno) */
static int order_add(struct tgm_chan *waiter, struct tgm_chan *source, u32 seqno)
{
	uintptr_t off = (uintptr_t)source->fence_cpu - (uintptr_t)tgm_fence_pages.va;
	u32 i = 0;

	if (!waiter->vm->fences_va || !tgm_fence_pages.va || off >= TGM_FENCE_BYTES)
		return -ENODEV;
	for (i = 0; i < waiter->norder; i++) {
		if (waiter->order[i].id != source->id)
			continue;
		if ((s32)(seqno - waiter->order[i].seqno) > 0)
			waiter->order[i].seqno = seqno;
		return 0;
	}
	if (waiter->norder == TGM_ORDER_MAX) {
		for (i = 0; i < waiter->norder;) {
			if ((s32)(READ_ONCE(*waiter->order[i].cpu) - waiter->order[i].seqno) >= 0)
				waiter->order[i] = waiter->order[--waiter->norder];
			else
				i++;
		}
		if (waiter->norder == TGM_ORDER_MAX)
			return -ENOSPC;
	}
	waiter->order[waiter->norder].va = waiter->vm->fences_va + off;
	waiter->order[waiter->norder].cpu = source->fence_cpu;
	waiter->order[waiter->norder].id = source->id;
	waiter->order[waiter->norder].seqno = seqno;
	waiter->order[waiter->norder].chid = source->chid;
	waiter->norder++;
	return 0;
}

int tgm_chan_order(struct tgm_chan *chan, uint32_t flags, uint32_t seqno, uint32_t *queued)
{
	struct tgm_chan *other = NULL;
	u32 n = 0;
	u32 last = 0;
	int ret = 0;

	*queued = 0;
	if (chan->kernel || !flags || (flags & ~(TGM_ORDER_AFTER | TGM_ORDER_BEFORE)))
		return -EINVAL;
	if (READ_ONCE(chan->error))
		return -EIO;
	if (flags & TGM_ORDER_BEFORE) {
		if (!seqno)
			seqno = chan->submitted;
		if ((s32)(seqno - chan->submitted) > 0)
			return -EINVAL; /* only submitted work: an acquire never waits for a later submission */
		if ((s32)(tgm_chan_completed(chan) - seqno) >= 0)
			flags &= ~TGM_ORDER_BEFORE; /* done already: nothing to wait for */
	}
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(other, &tgm_live_chans, live)
	{
		if (other == chan || other->kernel || !chan->group || other->group != chan->group ||
		    READ_ONCE(other->error))
			continue;
		last = READ_ONCE(other->submitted);
		if ((flags & TGM_ORDER_AFTER) && (s32)(tgm_chan_completed(other) - last) < 0) {
			ret = order_add(chan, other, last);
			if (ret)
				break;
			n++;
		}
		if (flags & TGM_ORDER_BEFORE) {
			ret = order_add(other, chan, seqno);
			if (ret)
				break;
			n++;
		}
	}
	mutex_unlock(&tgm_live_lock);
	*queued = n;
	return ret;
}

/* From tgm_chan_exec, before a submission's own entries: the acquires tgm_chan_order queued for it, as one entry. One whose
 * source passed its seqno already is left out. 1 if any was pushed. */
int tgm_order_waits(struct tgm_chan *chan)
{
	struct nvif_push *push = &chan->chan.push;
	struct tgm_order wait[TGM_ORDER_MAX];
	u32 i = 0;
	u32 n = 0;
	u32 kicked = 0;

	mutex_lock(&tgm_live_lock);
	n = chan->norder;
	memcpy(wait, chan->order, n * sizeof(wait[0]));
	chan->norder = 0;
	mutex_unlock(&tgm_live_lock);
	for (i = 0; i < n; i++) {
		if ((s32)(READ_ONCE(*wait[i].cpu) - wait[i].seqno) >= 0)
			continue;
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_MTHD(push, NV906F, SEMAPHOREA,
			  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(wait[i].va)),
			  SEMAPHOREB, lower_32_bits(wait[i].va), SEMAPHOREC, wait[i].seqno,
			  SEMAPHORED,
			  NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
				  NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
		chan->waited = wait[i];
		kicked++;
		/* the compositor never waits behind an app channel the deadline holds */
		if (chan->high)
			tgm_hold_need(tgm_gpu.device, wait[i].chid);
	}
	if (kicked)
		PUSH_KICK(push);
	return kicked != 0;
}

static int sync_wait(u64 id, u32 seqno, u64 end)
{
	tgm_chan_need_id(id, seqno);
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if (sync_done(id, seqno))
			return 0;
		if (nvxnu_nanotime() >= end)
			return -ETIMEDOUT;
		if (!nvxnu_idle(seq, end) && tgm_chan_announced_id(id, seqno))
			nvxnu_wake_due(seq, "fence", seqno);
	}
}

int tegra_gm20b_sync_for_cpu(const void *va, uint64_t bytes, uint64_t timeout_ns)
{
	static struct {
		u64 id;
		u32 seqno;
	} want[TGM_SYNC_MAX]; /* 8 KiB: not on the kernel stack; sync_lock */

	static DEFINE_MUTEX(sync_lock);
	struct tgm_chan *chan = NULL;
	enum tgm_hang verdict = TGM_HANG_STUCK;
	unsigned int n = 0;
	unsigned int i = 0;
	int ret = 0;
	u64 end = 0;

	mutex_lock(&sync_lock);
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (n < TGM_SYNC_MAX) {
			want[n].id = chan->id;
			want[n].seqno = READ_ONCE(chan->submitted);
			n++;
		}
	}
	mutex_unlock(&tgm_live_lock);
	if (!n) {
		mutex_unlock(&sync_lock);
		return 0;
	}
	end = nvxnu_nanotime() + timeout_ns;
	for (i = 0; i < n && !ret; i++) {
		ret = sync_wait(want[i].id, want[i].seqno, end);
		if (!ret || tgm_gpu.stage < 3)
			continue;
		/* the caller would copy a surface the GPU may still be writing: what the GPU is stuck on is killed and
		 * waited for; slow work only times the copy out */
		verdict = TGM_HANG_PROGRESSING;
		mutex_lock(&tgm_live_lock);
		chan = live_by(want[i].id, 0);
		if (chan)
			verdict = tgm_hang_recover(chan, "sync_for_cpu", want[i].seqno, timeout_ns);
		if (verdict == TGM_HANG_STUCK)
			tgm_chan_hung(chan, "sync_for_cpu", want[i].seqno, timeout_ns);
		mutex_unlock(&tgm_live_lock);
		if (verdict == TGM_HANG_RECOVERING) {
			ret = sync_wait(want[i].id, want[i].seqno,
					nvxnu_nanotime() + 2 * TGM_ISOLATE_NS);
		}
	}
	mutex_unlock(&sync_lock);
	nvxnu_dcache_clean_inval(va, bytes);
	return ret;
}

/* The IOSurface lock's wait for the submissions that may write a surface (TegraGM20BCoherency.cpp) */
#define TGM_WRITER_HANG_NS (1000 * NSEC_PER_MSEC)

uint32_t tgm_chan_uid(struct tgm_chan *chan)
{
	return (u32)chan->id;
}

int tgm_writer_done(uint32_t uid, uint32_t seqno)
{
	struct tgm_chan *chan = NULL;
	int done = 0;

	mutex_lock(&tgm_live_lock);
	chan = live_by_uid(uid);
	done = !chan || chan->error || (s32)(tgm_chan_completed(chan) - seqno) >= 0;
	mutex_unlock(&tgm_live_lock);
	return done;
}

/* tgm_live_lock held: what a wait for `uid` reaching `seqno` is late on. A kernel copy that sits on an acquire completes
 * nothing of its own, so its lateness is the first writer it acquires on that has not completed; only once they all
 * have is it the copy's. */
static struct tgm_chan *late_writer(u32 uid, u32 seqno, const struct tgm_relayout_writer *acquired,
				    u32 nacquired, u32 *late_seqno)
{
	struct tgm_chan *chan = NULL;
	u32 i = 0;

	for (i = 0; i < nacquired; i++) {
		chan = live_by_uid(acquired[i].uid);
		if (chan && !chan->error &&
		    (s32)(tgm_chan_completed(chan) - acquired[i].seqno) < 0) {
			*late_seqno = acquired[i].seqno;
			return chan;
		}
	}
	*late_seqno = seqno;
	return live_by_uid(uid);
}

/* No bound of its own: a writer that stops progressing is recovered (killed, its fence released) as
 * tegra_gm20b_sync_for_cpu's is, which ends the wait; a lost interrupt is nvxnu_wake_due's panic. */
static void writer_wait(u32 uid, u32 seqno, const struct tgm_relayout_writer *acquired,
			u32 nacquired)
{
	const u64 start = nvxnu_nanotime();
	struct tgm_chan *chan = NULL;
	enum tgm_hang verdict = TGM_HANG_STUCK;
	u32 late = 0;

	tgm_chan_need_uid(uid, seqno);
	for (;;) {
		const u64 end = nvxnu_nanotime() + TGM_WRITER_HANG_NS;

		for (;;) {
			const u32 seq = nvxnu_wake_seq();

			if (tgm_writer_done(uid, seqno))
				return;
			if (nvxnu_nanotime() >= end)
				break;
			if (!nvxnu_idle(seq, end) && tgm_chan_announced_uid(uid, seqno))
				nvxnu_wake_due(seq, "fence", seqno);
		}
		if (tgm_gpu.stage < 3)
			continue;
		verdict = TGM_HANG_PROGRESSING;
		mutex_lock(&tgm_live_lock);
		chan = late_writer(uid, seqno, acquired, nacquired, &late);
		if (chan)
			verdict = tgm_hang_recover(chan, "IOSurface lock", late,
						   nvxnu_nanotime() - start);
		if (verdict == TGM_HANG_STUCK)
			tgm_chan_hung(chan, "IOSurface lock", late, nvxnu_nanotime() - start);
		mutex_unlock(&tgm_live_lock);
	}
}

void tgm_writer_wait(uint32_t uid, uint32_t seqno)
{
	writer_wait(uid, seqno, NULL, 0);
}

void tgm_copy_wait(uint32_t uid, uint32_t seqno, const struct tgm_relayout_writer *acquired,
		   uint32_t nacquired)
{
	writer_wait(uid, seqno, acquired, nacquired);
}

int tgm_writer_fence_va(struct tgm_vm *vm, uint32_t uid, uint64_t *va)
{
	struct tgm_chan *chan = NULL;
	uintptr_t off = 0;
	int ret = -ENOENT;

	mutex_lock(&tgm_live_lock);
	chan = live_by_uid(uid);
	off = chan ? (uintptr_t)chan->fence_cpu - (uintptr_t)tgm_fence_pages.va : TGM_FENCE_BYTES;
	if (chan && !chan->error && vm->fences_va && tgm_fence_pages.va && off < TGM_FENCE_BYTES) {
		*va = vm->fences_va + off;
		ret = 0;
	}
	mutex_unlock(&tgm_live_lock);
	return ret;
}

int tgm_writers_all_done(void)
{
	struct tgm_chan *chan = NULL;
	int done = 1;

	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (!chan->error &&
		    (s32)(tgm_chan_completed(chan) - READ_ONCE(chan->submitted)) < 0) {
			done = 0;
			break;
		}
	}
	mutex_unlock(&tgm_live_lock);
	return done;
}

void tgm_writers_wait_all(void)
{
	while (tegra_gm20b_sync_for_cpu(NULL, 0, TGM_WRITER_HANG_NS) == -ETIMEDOUT) {
	}
}

/* Implicit order between processes (TegraGM20BABI.h method 17, TegraGM20BFences.h): a submission's acquires on the writers of
 * the surfaces it touches, queued as tgm_chan_order's are. Each writer names a submission kicked before the call (Exec records
 * it once tgm_chan_exec returned), so no two submissions can wait for each other. */
int tgm_chan_follow(struct tgm_chan *chan, const uint64_t *writers, uint32_t n)
{
	struct tgm_fence_source state;
	struct tgm_chan *src = NULL;
	uint32_t i = 0;
	int ret = 0;

	mutex_lock(&tgm_live_lock);
	for (i = 0; i < n && !ret; i++) {
		src = live_by_uid(tgm_fence_uid(writers[i]));
		if (!src || src->kernel)
			continue;
		state.self = src == chan;
		state.killed = READ_ONCE(src->error) != 0;
		state.completed = tgm_chan_completed(src);
		if (tgm_fence_wanted(writers[i], &state))
			ret = order_add(chan, src, tgm_fence_seqno(writers[i]));
	}
	mutex_unlock(&tgm_live_lock);
	return ret;
}

int tgm_chan_follow_importers(struct tgm_chan *chan, uint64_t pa, uint32_t *pending)
{
	struct tgm_fence_source state;
	struct tgm_chan *other = NULL;
	u32 last = 0;
	int ret = 0;

	*pending = 0;
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(other, &tgm_live_chans, live)
	{
		if (other->kernel || !vm_imports(other->vm, pa))
			continue;
		last = READ_ONCE(other->submitted);
		state.self = other == chan;
		state.killed = READ_ONCE(other->error) != 0;
		state.completed = tgm_chan_completed(other);
		if (!tgm_fence_wanted((u64)(u32)other->id << 32 | last, &state))
			continue;
		(*pending)++;
		ret = order_add(chan, other, last);
		if (ret)
			break;
	}
	mutex_unlock(&tgm_live_lock);
	return ret;
}

void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages)
{
	nvxnu_dcache_clean_inval_pages(pas, npages);
}
