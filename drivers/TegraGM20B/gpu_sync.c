// SPDX-License-Identifier: MIT
// Channel progress, CPU syncs and GPU writers.
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
#if TGM_DEBUG_PROBES
uint64_t tgm_chan_killed_ns(struct tgm_chan *chan)
{
	return READ_ONCE(chan->killed_ns);
}

#endif
uint32_t tgm_chan_waiter(struct tgm_chan *chan)
{
	return (uint32_t)(chan->id % TGM_WAITERS);
}

static int sync_wait(u64 id, u32 seqno, u64 end)
{
	const u32 waiter = (u32)(id % TGM_WAITERS);

	tgm_chan_need_id(id, seqno);
	for (;;) {
		u32 seq = tgm_waiter_seq(waiter);
		tgm_chan_wanted((u32)id, seqno);
		if (sync_done(id, seqno))
			return 0;
		if (nvxnu_nanotime() >= end)
			return -ETIMEDOUT;
		if (!tgm_waiter_idle(waiter, seq, end) && tgm_chan_announced_id(id, seqno))
			tgm_waiter_unwoken(waiter, seq, seqno);
	}
}

static unsigned int live_count(void)
{
	struct tgm_chan *chan = NULL;
	unsigned int n = 0;

	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (n < TGM_SYNC_MAX)
			n++;
	}
	mutex_unlock(&tgm_live_lock);
	return n;
}

/* Each caller's own snapshot: no lock is held while it sleeps, and a sync never waits for work submitted after it
 * started. Channels are appended to tgm_live_chans, so every channel counted and still live is among the first `n`. */
int tegra_gm20b_sync_for_cpu(const void *va, uint64_t bytes, uint64_t timeout_ns)
{
	struct sync_want {
		u64 id;
		u32 seqno;
	} *want = NULL;
	struct tgm_chan *chan = NULL;
	enum tgm_hang verdict = TGM_HANG_STUCK;
	unsigned int capacity = live_count();
	unsigned int n = 0;
	unsigned int i = 0;
	int ret = 0;
	u64 end = 0;

	if (!capacity)
		return 0;
	want = kmalloc_array(capacity, sizeof(*want), GFP_KERNEL);
	if (!want) {
		nvxnu_log("tegra-gm20b: CPU sync refused: no memory for %u channels' seqnos\n",
			  capacity);
		return -ENOMEM;
	}
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (n < capacity) {
			want[n].id = chan->id;
			want[n].seqno = READ_ONCE(chan->submitted);
			n++;
		}
	}
	mutex_unlock(&tgm_live_lock);
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
	kfree(want);
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
	done = !chan || __atomic_load_n(&chan->error, __ATOMIC_ACQUIRE) ||
	       (s32)(tgm_chan_completed(chan) - seqno) >= 0;
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
	const u32 waiter = uid % TGM_WAITERS;
	struct tgm_chan *chan = NULL;
	enum tgm_hang verdict = TGM_HANG_STUCK;
	u32 late = 0;

	tgm_chan_need_uid(uid, seqno);
	for (;;) {
		const u64 end = nvxnu_nanotime() + TGM_WRITER_HANG_NS;

		for (;;) {
			const u32 seq = tgm_waiter_seq(waiter);

			tgm_chan_wanted(uid, seqno);
			if (tgm_writer_done(uid, seqno))
				return;
			if (nvxnu_nanotime() >= end)
				break;
			if (!tgm_waiter_idle(waiter, seq, end) &&
			    tgm_chan_announced_uid(uid, seqno))
				tgm_waiter_unwoken(waiter, seq, seqno);
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

void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages)
{
	nvxnu_dcache_clean_inval_pages(pas, npages);
}
