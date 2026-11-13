// SPDX-License-Identifier: MIT
// Writer promises (SwitchGpuWriterPromise.h): the GPU jobs that park on them (SwitchGpuPromiseGiveUp.h), what was
// given up, and the one a direct present watches (SwitchGpuPresentWatch.h).
#include "gpu_internal.h"

/* The reader's job (its acquires pushed, nothing to roll them back) parks on a pending promise its acquires include */
int tgm_chan_note_promise(struct tgm_chan *chan, uint32_t offset, uint32_t value)
{
	const struct switch_gpu_giveup_job reader = {(u32)chan->id, chan->submitted + 1};

	return tgm_promise_park(offset, value, chan->owner,
				chan == tgm_present.chan ? "a present job" : "a GPU job", &reader,
				0);
}

void tgm_chan_park_promises(struct tgm_chan *chan, const struct switch_gpu_backing_token *tokens,
			    u32 count)
{
	u32 i;

	for (i = 0; i < count; i++)
		if (tokens[i].engine == SWITCH_GPU_ENGINE_PROMISE &&
		    !switch_gpu_backing_token_status(&tokens[i]))
			tgm_chan_note_promise(chan, tokens[i].queue, (u32)tokens[i].value);
}

#if TGM_STATS
/* Promise words the CPU raised (a cancel, a kill, a close, a give-up): a reader parked on ACQ_GEQ for one goes on
 * only if the GPU reads the word again after that write, which the present's completion reports once
 * (tgm_promise_was_cpu_raised). */
#define TGM_PROMISE_RAISED 8
static uint64_t promise_raised[TGM_PROMISE_RAISED];
static uint32_t promise_raised_next;

void tgm_promise_cpu_raised(uint32_t offset, uint32_t value)
{
	const uint32_t at =
		__atomic_fetch_add(&promise_raised_next, 1, __ATOMIC_RELAXED) % TGM_PROMISE_RAISED;

	__atomic_store_n(&promise_raised[at], (uint64_t)offset << 32 | value, __ATOMIC_RELEASE);
	switch_gpu_path_taken(SWITCH_GPU_PATH_PROMISE_CPU_RELEASE, 1);
}

int tgm_promise_was_cpu_raised(uint32_t offset, uint32_t value)
{
	const uint64_t raised = (uint64_t)offset << 32 | value;
	u32 i;

	for (i = 0; i < TGM_PROMISE_RAISED; i++)
		if (__atomic_load_n(&promise_raised[i], __ATOMIC_ACQUIRE) == raised)
			return 1;
	return 0;
}

/* The present's job completed after acquiring writer promises on the GPU: logged once for one its promised job's fence
 * released, and once for one the CPU raised (its channel, parked on that ACQ_GEQ, read the word again after the CPU
 * wrote it). A CPU raise older than the last TGM_PROMISE_RAISED reads as the job's. */
void tgm_present_promises_done(const struct switch_gpu_present_tokens *tokens, u64 queued, int copy)
{
	static int logged[2];
	u32 i;

	for (i = 0; i < tokens->count; i++) {
		const struct switch_gpu_backing_token *token = &tokens->tokens[i];
		int cpu = 0;

		if (token->engine != SWITCH_GPU_ENGINE_PROMISE)
			continue;
		cpu = tgm_promise_was_cpu_raised(token->queue, (u32)token->value);
		if (logged[cpu])
			continue;
		logged[cpu] = 1;
		nvxnu_log("tegra-gm20b: first %s present behind a writer promise %s completed %llu "
			  "ms after its offer "
			  "(fence offset %u value %u)\n",
			  copy ? "copy" : "fence-only",
			  cpu ? "the CPU released (a channel parked on ACQ_GEQ read the word again "
				"after a CPU write)"
			      : "its promised job's fence released",
			  (nvxnu_nanotime() - queued) / 1000000, token->queue, (u32)token->value);
	}
}
#endif

static struct switch_gpu_promise_abandoned promise_abandoned;
static void *promise_wake_work;

/* CPU readers asleep behind a promise given up on go on, as after a cancel: thread context, for the backings' locks */
static void promise_wake(void *unused)
{
	(void)unused;
	switch_gpu_backing_promise_sleepers_wake();
}

/* Before the first promise is made (switch_gpu_promise_engine) and at the present's open */
int tgm_promise_open(void)
{
	int ret = 0;

	if (!__atomic_load_n(&promise_wake_work, __ATOMIC_ACQUIRE)) {
		mutex_lock(&tgm_live_lock);
		if (!promise_wake_work)
			__atomic_store_n(&promise_wake_work, nvxnu_work_alloc(promise_wake, NULL),
					 __ATOMIC_RELEASE);
		ret = promise_wake_work ? 0 : -ENOMEM;
		mutex_unlock(&tgm_live_lock);
	}
	return ret ? ret : tgm_promise_giveup_open();
}

int tgm_promise_abandoned(uint32_t offset, uint32_t value)
{
	return switch_gpu_promise_abandoned_has(&promise_abandoned, offset, value);
}

void tgm_promise_abandon(uint32_t offset, uint32_t value)
{
	switch_gpu_promise_abandoned_add(&promise_abandoned, offset, value);
	nvxnu_work_schedule(promise_wake_work);
	tgm_present_promise_released(offset, value);
}

/* The maker's own GPU release came first after all: no longer abandoned */
void tgm_promise_kept_first(uint32_t offset, uint32_t value)
{
	switch_gpu_promise_abandoned_drop(&promise_abandoned, offset, value);
}

/* tgm_live_lock held: the process whose connection's fence page holds the word at `offset` */
const char *tgm_promise_maker(uint32_t offset)
{
	struct tgm_chan *chan = NULL;

	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (!chan->kernel &&
		    ((uintptr_t)chan->fence_cpu - (uintptr_t)tgm_fence_pages.va) / TGM_FENCE_SLOT ==
			    offset / TGM_FENCE_SLOT)
			return chan->owner;
	}
	return "a connection with no channel";
}

/* Promises kept lately, under tgm_present.lock: an offer whose snapshot took one just before its job was submitted
 * watches that job. */
#define PRESENT_KEPT 8

struct present_kept {
	u32 offset, value;
	struct switch_gpu_backing_token producer;
};
static struct present_kept present_kept[PRESENT_KEPT];
static u32 present_kept_next;
#if TGM_STATS
static int present_kept_logged;
#endif

/* tgm_present.lock held, the job retained: it takes the promise's place among the present's tokens, as the GM20B
 * producer a watch of it finishes. Not the promise too: the job's fence push releases the word after the fence a pass
 * may already have seen. */
static void present_takes(const struct switch_gpu_backing_token *producer)
{
	switch_gpu_backing_token_release(&tgm_present.tokens.tokens[0]);
	tgm_present.tokens.tokens[0] = *producer;
}

/* tgm_present.lock held, the offer's one token a writer promise: its job as *kept if it is submitted already, else
 * nothing (the watch waits PROMISED). */
void tgm_present_promise_watch(struct switch_gpu_backing_token *kept)
{
	const struct switch_gpu_backing_token promise = tgm_present.tokens.tokens[0];
	u32 i;

	for (i = 0; i < PRESENT_KEPT; i++) {
		const struct present_kept *k = &present_kept[i];

		if (k->producer.engine && k->offset == promise.queue &&
		    k->value == (u32)promise.value &&
		    !switch_gpu_backing_token_retain(&k->producer)) {
			present_takes(&k->producer);
			switch_gpu_path_promise_direct(SWITCH_GPU_PATH_DIRECT_PROMISED);
			switch_gpu_path_promise_direct(SWITCH_GPU_PATH_DIRECT_KEPT);
			*kept = k->producer;
			return;
		}
	}
}

/* SubmitJob, the job that keeps the promise submitted: a watch waiting for it watches the job, whose fence push
 * releases the word after its fence, as the nonstall filter's view (SwitchGpuPresentFilter.cpp) and as the producer
 * whose asked interrupt's pass completes the watch. */
void tgm_present_promise_kept(uint32_t offset, uint32_t value,
			      const struct switch_gpu_backing_token *producer)
{
	int watched = 0;

	mutex_lock(&tgm_present.lock);
	present_kept[present_kept_next++ % PRESENT_KEPT] =
		(struct present_kept){offset, value, *producer};
	if (switch_gpu_present_watch_promised(&tgm_present.watch, offset, value) &&
	    !switch_gpu_backing_token_retain(producer)) {
		if (switch_gpu_present_watch_kept(&tgm_present.watch, offset, value, producer)) {
			present_takes(producer);
			switch_gpu_path_promise_direct(SWITCH_GPU_PATH_DIRECT_KEPT);
			tgm_present_promise_leave(offset, value);
			tgm_present.recovered = nvxnu_nanotime();
			nvxnu_work_schedule_after(tgm_present.deadline_work, TGM_PRESENT_NS);
			/* before the re-arm, as the offer attaches before its arm: the job's end then runs the display's flip
			 * from the nonstall filter, and a pass claiming the watch detaches the view after this */
			tgm_present_filter_attach(producer);
			switch_gpu_present_watch_rearm(&tgm_present.watch);
			watched = 1;
		} else {
			switch_gpu_backing_token_release(producer);
		}
	}
	mutex_unlock(&tgm_present.lock);
	if (!watched)
		return;
#if TGM_STATS
	if (!__atomic_exchange_n(&present_kept_logged, 1, __ATOMIC_RELAXED))
		nvxnu_log("tegra-gm20b: first direct present of a frame behind a writer promise "
			  "watches the job "
			  "that keeps it, with no present job\n");
#endif
	tgm_chan_need_tokens(producer, 1);
	if (switch_gpu_backing_token_physical_status(producer, 0))
		nvxnu_work_schedule(tgm_present.watch_work);
}

/* The watch is done waiting on its promise: kept, given up on or released */
void tgm_present_promise_leave(uint32_t offset, uint32_t value)
{
	if (__atomic_exchange_n(&tgm_present.promise_parked, 0, __ATOMIC_ACQ_REL))
		tgm_promise_leave(offset, value, 1);
}

/* A promise released or given up on from the CPU: a watch still waiting for its job goes to the work item. */
void tgm_present_promise_released(uint32_t offset, uint32_t value)
{
	if (switch_gpu_present_watch_promised(&tgm_present.watch, offset, value) &&
	    tgm_present.watch_work)
		nvxnu_work_schedule(tgm_present.watch_work);
}
