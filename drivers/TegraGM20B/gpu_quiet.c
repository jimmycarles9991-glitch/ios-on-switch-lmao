// SPDX-License-Identifier: MIT
// Quiet job ends (SwitchGpuQuiet.h): waiters' asks, the owner's wake entries, and the idle clock's settle.
#include "gpu_internal.h"

void tgm_chan_on_wake(struct tgm_chan *chan, void (*wake)(void *arg, uint32_t tag), void *arg,
		      uint32_t tag)
{
	chan->wake_arg = arg;
	chan->wake_tag = tag;
	__atomic_store_n(&chan->wake, wake, __ATOMIC_RELEASE);
}

void tgm_chan_need(struct tgm_chan *chan, uint32_t seqno)
{
	void (*wake)(void *, u32) = __atomic_load_n(&chan->wake, __ATOMIC_ACQUIRE);

	if (!wake || (s32)(tgm_chan_completed(chan) - seqno) >= 0 ||
	    !switch_gpu_quiet_need(&chan->quiet, seqno))
		return;
	if (stats_on())
		__atomic_fetch_add(&tgm_wstat.asked, 1, __ATOMIC_RELAXED);
	switch_gpu_path_taken(SWITCH_GPU_PATH_WAKE_ASK, 1);
	wake(chan->wake_arg, chan->wake_tag);
}

void tgm_chan_need_uid(uint32_t uid, uint32_t seqno)
{
	struct tgm_chan *chan = NULL;

	mutex_lock(&tgm_live_lock);
	chan = live_by_uid(uid);
	if (chan && !chan->error)
		tgm_chan_need(chan, seqno);
	mutex_unlock(&tgm_live_lock);
}

void tgm_chan_need_tokens(const struct switch_gpu_backing_token *tokens, uint32_t count)
{
	struct tgm_chan *chan = NULL;
	u32 i = 0;

	mutex_lock(&tgm_live_lock);
	for (i = 0; i < count; i++) {
		chan = live_by_uid(tokens[i].queue);
		if (chan && !chan->error && switch_gpu_quiet_token_on(&tokens[i], chan->id))
			tgm_chan_need(chan, (u32)tokens[i].value);
	}
	mutex_unlock(&tgm_live_lock);
}

/* A job the interrupt pass already saw end needs no entry: that pass woke whatever waited for it. A killed channel's
 * waiters are woken by its kill. */
void tgm_chan_wake_push(struct tgm_chan *chan)
{
	struct nvif_push *push = &chan->chan.push;

	if (READ_ONCE(chan->error) || !switch_gpu_quiet_wake_due(&chan->quiet, chan->submitted))
		return;
	if ((s32)(READ_ONCE(chan->progress_done) - chan->submitted) >= 0) {
		switch_gpu_quiet_announce(&chan->quiet, chan->submitted);
		return;
	}
	if (!tgm_chan_room(chan, CHAN_WAKE_ENTRIES, CHAN_WAKE_DWORDS) ||
	    nvif_chan_gpfifo_wait(&chan->chan, CHAN_WAKE_ENTRIES, CHAN_WAKE_DWORDS))
		nvxnu_panic("tegra-gm20b: %s channel %u: no room for the wake entry its "
			    "submissions left room for",
			    chan->owner, chan->chid);
	/* NOLINTNEXTLINE(bugprone-signed-bitwise): nouveau's PUSH_MTHD computes in int */
	PUSH_MTHD(push, NV906F, NON_STALL_INTERRUPT, 0);
	switch_gpu_quiet_announce(&chan->quiet, chan->submitted);
	switch_gpu_jobs_announce_newest(&chan->jobs);
	if (switch_gpu_irq_arm(&chan->irq_state, chan->submitted) &&
	    nvif_event_allow(&chan->nonstall))
		nvxnu_panic("SwitchGpu: cannot arm channel completion");
	if (stats_on())
		__atomic_fetch_add(&tgm_wstat.pushed, 1, __ATOMIC_RELAXED);
	PUSH_KICK(push);
}

void tgm_chan_quiet_wake(struct tgm_chan *chan)
{
	if (!switch_gpu_quiet_wake_due(&chan->quiet, READ_ONCE(chan->submitted)))
		return;
	tgm_gate_enter();
	tgm_chan_wake_push(chan);
	gate_leave();
}

/* The idle clock's tick found seqnos in flight and no kick for its drop time: what completed is retired from the fence
 * words, and each channel whose newest job is still running asks for its interrupt, as a waiter would. */
void tgm_idle_settle(void *arg)
{
	struct tgm_chan *chan = NULL;

	(void)arg;
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		idle_retire(chan, tgm_chan_completed(chan));
		tgm_chan_need(chan, READ_ONCE(chan->submitted));
	}
	mutex_unlock(&tgm_live_lock);
	if (stats_on())
		__atomic_fetch_add(&tgm_wstat.settled, 1, __ATOMIC_RELAXED);
	__atomic_store_n(&tgm_idle.settling, 0, __ATOMIC_SEQ_CST);
}

/* sync_done's channel by id, and tgm_writer_done's by uid: a channel that is gone or failed counts as announced, as
 * those count it done. */
void tgm_chan_need_id(u64 id, u32 seqno)
{
	struct tgm_chan *chan = NULL;

	mutex_lock(&tgm_live_lock);
	chan = live_by(id, 0);
	if (chan && !chan->error)
		tgm_chan_need(chan, seqno);
	mutex_unlock(&tgm_live_lock);
}

int tgm_chan_announced_id(u64 id, u32 seqno)
{
	struct tgm_chan *chan = NULL;
	int announced = 1;

	mutex_lock(&tgm_live_lock);
	chan = live_by(id, 0);
	if (chan && !chan->error)
		announced = tgm_chan_announced(chan, seqno);
	mutex_unlock(&tgm_live_lock);
	return announced;
}

int tgm_chan_announced_uid(u32 uid, u32 seqno)
{
	struct tgm_chan *chan = NULL;
	int announced = 1;

	mutex_lock(&tgm_live_lock);
	chan = live_by_uid(uid);
	if (chan && !chan->error)
		announced = tgm_chan_announced(chan, seqno);
	mutex_unlock(&tgm_live_lock);
	return announced;
}
