// SPDX-License-Identifier: MIT
// The writer promise give-up (SwitchGpuPromiseGiveUp.h) in the kernel: its table and one call under tgm_live_lock,
// the interrupt pass's look, and the raise.
#include "gpu_internal.h"
#include "SwitchGpuPromiseGiveUp.h"
#include "TegraGM20BJobABI.h"

_Static_assert(TGM_FENCE_PROMISES == TegraGM20BMaxPromises &&
		       TGM_FENCE_PROMISE_WORD == TegraGM20BPromiseWord,
	       "the guard's rows are a page's promise slots");
_Static_assert(SWITCH_GPU_GIVEUP_PAGES == TGM_FENCE_USER_PAGES &&
		       SWITCH_GPU_GIVEUP_PER_PAGE == TegraGM20BMaxPromises,
	       "a give-up record for every promise slot that can exist");
_Static_assert(SWITCH_GPU_GIVEUP_LOOK_NS == TGM_HANG_NS,
	       "a kept promise's look is the hang rule's");

/* The promise slot whose word is at `offset` (page by page), -1 for any other word */
static int promise_slot(uint32_t offset)
{
	const uint32_t page = offset / TGM_FENCE_SLOT;
	const uint32_t word = offset % TGM_FENCE_SLOT / 4;

	if (page >= TGM_FENCE_USER_PAGES || word < TGM_FENCE_PROMISE_WORD ||
	    word >= TGM_FENCE_PROMISE_WORD + TGM_FENCE_PROMISES)
		return -1;
	return (int)(page * TGM_FENCE_PROMISES + word - TGM_FENCE_PROMISE_WORD);
}

/* The guard's value of the promise slot whose word is at `offset`, NULL for any other word */
static uint32_t *promise_made(uint32_t offset)
{
	const int slot = promise_slot(offset);

	if (slot < 0)
		return NULL;
	return &tgm_fence_pages.made[slot / TGM_FENCE_PROMISES][slot % TGM_FENCE_PROMISES];
}

/* tgm_live_lock held (one raiser): a pending promise word raised from the CPU, a plain store-release after one more
 * look. The fence pool is mapped write-combined (Normal Non-cacheable), where T210's bus answers an exclusive access (a
 * compare-and-swap, any atomic read-modify-write) with an SError. Interrupts are off so the look and the store are a
 * few instructions apart, and the guard (switch_gpu_promise_claim) keeps the slot from being made again meanwhile: the
 * store can never lower a later promise's word. The parked reader's PBDMA reads the word from DRAM (volatile, no L2),
 * which a store-release need not have reached until a DSB completes it. */
static int promise_raise(uint32_t offset, volatile uint32_t *word, uint32_t value)
{
	uint32_t *made = promise_made(offset);
	unsigned long flags;
	int raised = 0;

	if (!made)
		return 0;
	local_irq_save(flags);
	switch_gpu_promise_raise_mark(&tgm_fence_pages.raising, offset);
	if (switch_gpu_promise_raise_made_still(made, value) &&
	    switch_gpu_promise_raise_word_below(word, value)) {
		__atomic_store_n(word, value, __ATOMIC_RELEASE);
		__asm__ volatile("dsb sy" ::: "memory");
		raised = 1;
	}
	switch_gpu_promise_raise_unmark(&tgm_fence_pages.raising);
	local_irq_restore(flags);
	return raised;
}

const struct switch_gpu_promise_fence tgm_promise_fence = {tgm_fence_word, promise_raise,
							   tgm_fence_owner};

static struct switch_gpu_giveups giveups;
static void *giveup_call;
/* tgm_live_lock: the GPU is powered down (system sleep, rail-gating) from tgm_promise_suspend to tgm_promise_resume, its
 * registers out of reach. The call stays armed for holders' bounds, which are memory work only. */
static int giveup_down;
/* the call's arming: taken by the interrupt pass's cancel, never around anything that sleeps */
static DEFINE_SPINLOCK(giveup_lock);

/* tgm_live_lock held */
static int giveup_reader_parked(const struct switch_gpu_giveup_job *reader)
{
	struct tgm_chan *chan = live_by_uid(reader->uid);

	return chan && !READ_ONCE(chan->error) &&
	       (s32)(tgm_chan_completed(chan) - reader->first) < 0;
}

/* tgm_live_lock held: the promised job's channel, waited on as tgm_chan_check_progress's waiter of the job ahead of the
 * promised one, then of the promised job itself (its last seqno `last`) */
static int giveup_job_ahead(const struct switch_gpu_giveup_job *job, uint32_t last,
			    const volatile uint32_t **fence, uint32_t *seqno, uint64_t *check_ns)
{
	struct tgm_chan *chan = live_by_uid(job->uid);
	const u32 ahead = job->first - 1;
	u32 wait = ahead;
	u64 check = 0;

	if (!chan || READ_ONCE(chan->error))
		return SWITCH_GPU_GIVEUP_GONE;
	/* nothing runs before the resume, and the hang rule's look reads registers */
	if (giveup_down)
		return SWITCH_GPU_GIVEUP_DOWN;
	if ((s32)(tgm_chan_completed(chan) - ahead) >= 0)
		wait = last;
	tgm_chan_need(chan, wait);
	check = tgm_chan_check_progress_locked(chan, "a writer promise's reader", wait);
	if (READ_ONCE(chan->error))
		return SWITCH_GPU_GIVEUP_GONE;
	*fence = (s32)(tgm_chan_completed(chan) - wait) >= 0 ? NULL : chan->fence_cpu;
	*seqno = wait;
	*check_ns = check ? check : SWITCH_GPU_GIVEUP_LOOK_NS;
	return wait == ahead ? SWITCH_GPU_GIVEUP_BEHIND : SWITCH_GPU_GIVEUP_HEAD;
}

/* tgm_live_lock held */
static void giveup_need(uint32_t uid, uint32_t seqno)
{
	struct tgm_chan *chan = live_by_uid(uid);

	if (chan && !READ_ONCE(chan->error))
		tgm_chan_need(chan, seqno);
}

/* tgm_live_lock held: the one line of a promise given up, the time from its first reader's park */
static void giveup_report(uint32_t offset, uint32_t value, const struct switch_gpu_giveup *e,
			  u64 waited_ns)
{
	switch_gpu_path_promise_abandoned(waited_ns);
	if (e->display)
		switch_gpu_path_promise_direct(SWITCH_GPU_PATH_DIRECT_GIVEN_UP);
	nvxnu_log(
		"tegra-gm20b: %s's writer promise at fence offset %u (value %u) was not kept: "
		"%s waited %llu ms for it in %s; "
		"it goes on without the promised writes, and a present of the surface is dropped\n",
		tgm_promise_maker(offset), offset, value, e->waiter, waited_ns / 1000000, e->who);
}

#if TGM_STATS
/* tgm_live_lock held: the ACQUIRE probe's look at the GPU readers still parked at a give-up */
static void giveup_probe(const struct switch_gpu_giveup *e)
{
	const struct switch_gpu_giveup_readers *r = switch_gpu_giveup_readers_of(&giveups, e);
	const u64 now = nvxnu_nanotime();
	u32 i;

	if (giveup_down || !r)
		return;
	for (i = 0; i < r->count; i++) {
		struct tgm_chan *chan = live_by_uid(r->job[i].uid);

		if (chan)
			tgm_acquire_probe_giveup(chan, e->parked_ns, now);
	}
}

#endif
/* tgm_live_lock held: the mark before the word moves, taken back when the maker's own release came first */
static void giveup_stepped(uint32_t offset, uint32_t value, int step, void *entry)
{
	const struct switch_gpu_giveup *e = (const struct switch_gpu_giveup *)entry;

	if (step == SWITCH_GPU_PROMISE_RAISING) {
#if TGM_STATS
		giveup_probe(e);
#endif
		tgm_promise_abandon(offset, value);
		return;
	}
	if (step == SWITCH_GPU_PROMISE_KEPT_FIRST) {
		tgm_promise_kept_first(offset, value);
		return;
	}
#if TGM_STATS
	tgm_promise_cpu_raised(offset, value);
#endif
	giveup_report(offset, value, e, nvxnu_nanotime() - e->parked_ns);
}

/* tgm_live_lock held */
static void giveup_recall(struct switch_gpu_giveup *e)
{
	unsigned long flags;
	int recalled = 0;

	spin_lock_irqsave(&giveup_lock, flags);
	recalled = switch_gpu_giveup_recall(&giveups, e);
	spin_unlock_irqrestore(&giveup_lock, flags);
	if (recalled)
		giveup_need(e->job.uid, e->job_last);
}

/* tgm_live_lock held */
static int giveup_killed(const struct switch_gpu_giveup_job *job)
{
	struct tgm_chan *chan = live_by_uid(job->uid);

	return !chan || READ_ONCE(chan->error);
}

/* tgm_live_lock held: the call's simple lock decides between the holder's bound and a keep (tgm_promise_unhold) */
static int giveup_expire(struct switch_gpu_giveup *e, uint64_t now)
{
	unsigned long flags;
	int expired = 0;

	spin_lock_irqsave(&giveup_lock, flags);
	expired = switch_gpu_giveup_held_expire(&giveups, e, now);
	spin_unlock_irqrestore(&giveup_lock, flags);
	return expired;
}

static const struct switch_gpu_giveup_ops giveup_ops = {
	&tgm_promise_fence, promise_slot,   giveup_reader_parked,         giveup_job_ahead,
	giveup_need,        giveup_stepped, tgm_present_promise_released, giveup_recall,
	giveup_killed,      giveup_expire};

static void giveup_after(uint64_t ns)
{
	nvxnu_work_schedule_after(giveup_call, ns);
}

static void giveup_cancel(void)
{
	nvxnu_work_cancel(giveup_call);
}

static const struct switch_gpu_giveup_call giveup_arming = {giveup_after, giveup_cancel};

/* tgm_live_lock held: the call armed for the earliest due (a holder's bound, the hang rule's next look), the pass left
 * looking while the direct present waits on an unkept promise with no bound, and cancelled once nothing is waited on */
static void giveup_arm(void)
{
	u64 next = switch_gpu_giveup_next(&giveups, &giveup_ops);
	unsigned long flags;

	if (!next && switch_gpu_giveup_watched(&giveups))
		next = SWITCH_GPU_GIVEUP_WATCHING;

	spin_lock_irqsave(&giveup_lock, flags);
	switch_gpu_giveup_arm_locked(&giveups, &giveup_arming, next, nvxnu_nanotime());
	spin_unlock_irqrestore(&giveup_lock, flags);
}

/* The call: thread context, under tgm_live_lock alone */
static void giveup_run(void *unused)
{
	unsigned long flags;

	(void)unused;
	spin_lock_irqsave(&giveup_lock, flags);
	__atomic_store_n(&giveups.armed_ns, 0, __ATOMIC_RELEASE);
	spin_unlock_irqrestore(&giveup_lock, flags);
	mutex_lock(&tgm_live_lock);
	switch_gpu_giveup_run(&giveups, &giveup_ops, nvxnu_nanotime());
	giveup_arm();
	mutex_unlock(&tgm_live_lock);
}

void tgm_promise_claim(const volatile uint32_t *first, struct switch_gpu_promise_claim *claim)
{
	const uint32_t offset = tgm_fence_offset(first);

	claim->made = promise_made(offset);
	claim->raising = &tgm_fence_pages.raising;
	claim->first = offset;
}

int tgm_promise_giveup_open(void)
{
	int ret = 0;

	if (__atomic_load_n(&giveup_call, __ATOMIC_ACQUIRE))
		return 0;
	mutex_lock(&tgm_live_lock);
	if (!giveup_call)
		__atomic_store_n(&giveup_call, nvxnu_work_alloc(giveup_run, NULL),
				 __ATOMIC_RELEASE);
	ret = giveup_call ? 0 : -ENOMEM;
	mutex_unlock(&tgm_live_lock);
	return ret;
}

int switch_gpu_promise_park_locked(uint32_t offset, uint32_t value, const char *waiter,
				   const char *who, const struct switch_gpu_giveup_job *reader,
				   int display)
{
	struct switch_gpu_giveup *e = NULL;
	unsigned long flags;
	int recalled = 0;
	int at = 0;

	at = switch_gpu_giveup_park(&giveups, &giveup_ops, offset, value, waiter, who, reader,
				    display, nvxnu_nanotime());
	e = at >= 0 ? &giveups.entry[at] : NULL;
	if (e && !e->kept) {
		spin_lock_irqsave(&giveup_lock, flags);
		recalled = switch_gpu_giveup_recall(&giveups, e);
		if (!recalled && !e->due_ns)
			e->due_ns = switch_gpu_giveup_bound(
				&giveups, e, e->parked_ns + TegraGM20BPromiseBoundNs);
		spin_unlock_irqrestore(&giveup_lock, flags);
		if (recalled)
			giveup_need(e->job.uid, e->job_last);
	}
	if (e) {
		switch_gpu_giveup_due(e, nvxnu_nanotime());
		giveup_arm();
	}
	return at >= 0;
}

int tgm_promise_park(uint32_t offset, uint32_t value, const char *waiter, const char *who,
		     const struct switch_gpu_giveup_job *reader, int display)
{
	mutex_lock(&tgm_live_lock);
	const int result =
		switch_gpu_promise_park_locked(offset, value, waiter, who, reader, display);
	mutex_unlock(&tgm_live_lock);
	return result;
}

/* Method 34 Arm: the holder's bound on the promise of `value` at `offset`, `ns` from now (`fails`: its command buffer
 * fails there). A reader parked on it now or later goes on at the bound. */
void tgm_promise_hold(uint32_t offset, uint32_t value, uint64_t ns, int fails)
{
	const int slot = promise_slot(offset);
	struct switch_gpu_giveup *e = slot < 0 ? NULL : &giveups.entry[slot];
	const uint64_t due = nvxnu_nanotime() + ns;
	const uint32_t owner = tgm_fence_owner(offset);
	unsigned long flags;

	if (slot < 0)
		return;
	mutex_lock(&tgm_live_lock);
	spin_lock_irqsave(&giveup_lock, flags);
	switch_gpu_giveup_hold(&giveups, slot, value, owner, due, fails);
	if (e->offset == offset && e->value == value && e->owner == owner && !e->kept)
		e->due_ns = due;
	spin_unlock_irqrestore(&giveup_lock, flags);
	giveup_arm();
	mutex_unlock(&tgm_live_lock);
}

/* A job that was to keep the promise of `value` at `offset` was refused after its keep (AGXDeviceUserClient::runJob):
 * the bound runs again, its holder's or the default from its first reader's park, already past or not */
void tgm_promise_rehold(uint32_t offset, uint32_t value)
{
	const int slot = promise_slot(offset);
	struct switch_gpu_giveup *e = slot < 0 ? NULL : &giveups.entry[slot];
	const uint32_t owner = tgm_fence_owner(offset);
	unsigned long flags;
	int reopened = 0;

	if (slot < 0)
		return;
	mutex_lock(&tgm_live_lock);
	spin_lock_irqsave(&giveup_lock, flags);
	reopened = switch_gpu_giveup_rehold(&giveups, slot, value, owner);
	if (reopened && e->offset == offset && e->value == value && e->owner == owner && !e->kept &&
	    !e->due_ns)
		e->due_ns = switch_gpu_giveup_bound(&giveups, e,
						    e->parked_ns + TegraGM20BPromiseBoundNs);
	spin_unlock_irqrestore(&giveup_lock, flags);
	if (reopened)
		giveup_arm();
	mutex_unlock(&tgm_live_lock);
}

/* keepPromises before its job, or a release of the promise: its bound ends. 1 when a bound its command buffer
 * fails at passed first, where a keep is refused. The call's simple lock alone: never tgm_live_lock on a submission. */
int tgm_promise_unhold(uint32_t offset, uint32_t value)
{
	const int slot = promise_slot(offset);
	unsigned long flags;
	int past = 0;

	if (slot < 0)
		return 0;
	spin_lock_irqsave(&giveup_lock, flags);
	past = switch_gpu_giveup_unhold(&giveups, slot, value, tgm_fence_owner(offset),
					nvxnu_nanotime());
	spin_unlock_irqrestore(&giveup_lock, flags);
	return past;
}

void tgm_promise_leave(uint32_t offset, uint32_t value, int display)
{
	mutex_lock(&tgm_live_lock);
	switch_gpu_giveup_leave(&giveups, &giveup_ops, offset, value, display);
	giveup_arm();
	mutex_unlock(&tgm_live_lock);
}

/* The power-down (tgm_recover_suspend), before the registers go: a run in progress holds tgm_live_lock and is waited out */
void tgm_promise_suspend(void)
{
	mutex_lock(&tgm_live_lock);
	giveup_down = 1;
	mutex_unlock(&tgm_live_lock);
}

/* The power-up (tgm_wake, tgm_rail_up_locked) */
void tgm_promise_resume(void)
{
	mutex_lock(&tgm_live_lock);
	switch_gpu_giveup_resume(&giveups, nvxnu_nanotime());
	giveup_down = 0;
	giveup_arm();
	mutex_unlock(&tgm_live_lock);
}

/* keptPromises: `job` (its channel's uid and its fence's seqno) keeps the promise; chan is that channel, which the
 * caller holds. tgm_live_lock is taken only while a reader is parked on the slot: one that parks after the look recalls
 * the job from the slot. */
void tgm_promise_kept(uint32_t offset, uint32_t value, struct tgm_chan *chan,
		      const struct switch_gpu_backing_token *job)
{
	struct switch_gpu_kicked_job record;
	struct switch_gpu_giveup_job kept = {job->queue, (u32)job->value};
	const int slot = promise_slot(offset);
	unsigned long flags;

	if (slot < 0)
		return;
	if (chan && switch_gpu_jobs_find(&chan->jobs, (u32)job->value, &record))
		kept.first = record.first;
	spin_lock_irqsave(&giveup_lock, flags);
	switch_gpu_giveup_remember(&giveups, slot, value, tgm_fence_owner(offset), &kept,
				   (u32)job->value);
	spin_unlock_irqrestore(&giveup_lock, flags);
	if (!switch_gpu_giveup_in_use(&giveups, slot))
		return;
	mutex_lock(&tgm_live_lock);
	switch_gpu_giveup_kept(&giveups, &giveup_ops, offset, value, &kept, (u32)job->value,
			       nvxnu_nanotime());
	giveup_arm();
	mutex_unlock(&tgm_live_lock);
}

/* The interrupt pass with the call armed: plain loads, and the call scheduled or cancelled under its simple lock. Out of
 * line, so the pass's look at an unarmed call takes no stack frame. */
static noinline void giveup_pass_armed(void)
{
	unsigned long flags;
	const int look = switch_gpu_giveup_pass(&giveups, &tgm_promise_fence);

	if (look == SWITCH_GPU_GIVEUP_PASS_RUN) {
		nvxnu_work_schedule(giveup_call);
		return;
	}
	if (look != SWITCH_GPU_GIVEUP_PASS_CANCEL)
		return;
	spin_lock_irqsave(&giveup_lock, flags);
	if (switch_gpu_giveup_pass(&giveups, &tgm_promise_fence) == SWITCH_GPU_GIVEUP_PASS_CANCEL)
		switch_gpu_giveup_arm_locked(&giveups, &giveup_arming, 0, 0);
	spin_unlock_irqrestore(&giveup_lock, flags);
}

/* The end of every GPU interrupt pass (shim/xnu_glue.cpp gpuInterrupt), nvkm's locks released; nothing here may sleep.
 * While nothing is waited on, one acquire load and a branch (gm20b_promise_pass_object_test holds the built object to
 * that). */
void tgm_promise_pass(void)
{
	if (switch_gpu_giveup_armed(&giveups))
		giveup_pass_armed();
}
