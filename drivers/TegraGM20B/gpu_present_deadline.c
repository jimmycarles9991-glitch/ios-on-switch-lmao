// SPDX-License-Identifier: MIT
// The GPU present's deadline (TGM_PRESENT_NS after a present was queued): what it still waits for, as a hang to
// recover from, a producer still running, or a lost interrupt.
#include "gpu_internal.h"

/* tgm_live_lock held; these descriptions only run after the present deadline expires. */
static void present_channel_brief(struct tgm_chan *chan, int chid, char *buf, unsigned int len)
{
	if (!chan) {
		scnprintf(buf, len, "untracked ch %d", chid);
		return;
	}
	scnprintf(buf, len, "%.16s ch %u fence %u/%u err %u%s%.12s uid %llu", chan->owner,
		  chan->chid, tgm_chan_completed(chan), chan->submitted, chan->error,
		  chan->fault[0] ? " " : "", chan->fault, chan->id);
}

static const char *const watch_late_names[] = {
	"its interrupt was never asked for", "its interrupt is owed by the channel's owner",
	"its interrupt is queued", "its interrupt was due and never came"};

/* The watched producer finished (physical: its status) and the watch is still armed: where the interrupt the offer
 * asked for is (SwitchGpuQuiet.h). Queued behind work the GPU has not done: 0, and the deadline follows that work
 * (*waited at *seqno) as it does a running producer. Owed by the channel's owner, which pushes it under its own
 * submission order: the deadline is set again. Anything else should have delivered the watch already (its pass, a kill
 * or a teardown): the pass gets a wake's grace, then the lost interrupt panics. tgm_present.lock and tgm_live_lock
 * held. */
static int watch_late(int physical, u64 age, struct tgm_chan **waited, u32 *seqno)
{
	const struct switch_gpu_backing_token *token = &tgm_present.watch.token;
	struct tgm_chan *chan = live_by_uid(token->queue);
	const u64 now = nvxnu_nanotime();
	const char *why = "it failed and no kill delivered it";
	u32 end = (u32)token->value;
	int where = SWITCH_GPU_QUIET_UNASKED;
	int recorded = 0;

	if (!chan || chan->error || !switch_gpu_quiet_token_on(token, chan->id)) {
		chan = NULL;
		if (physical > 0)
			why = "its channel is gone and no teardown delivered it";
	} else if (physical > 0) {
		end = switch_gpu_jobs_announced_by(&chan->jobs, (u32)token->value, &recorded);
		where = switch_gpu_quiet_interrupt(&chan->quiet, (u32)token->value,
						   tgm_chan_completed(chan), &end, recorded);
		why = watch_late_names[where];
	}
	if (where == SWITCH_GPU_QUIET_QUEUED) {
		*waited = chan;
		*seqno = end;
		return 0;
	}
	if (where == SWITCH_GPU_QUIET_OWED) {
		tgm_present.recovered = now;
		tgm_present.recheck = TGM_PRESENT_NS;
		nvxnu_work_schedule_after(tgm_present.deadline_work, TGM_PRESENT_NS);
		return 1;
	}
	if (!tgm_present.late)
		tgm_present.late = now;
	if (now - tgm_present.late < TGM_PRESENT_WAKE_GRACE_NS) {
		nvxnu_work_schedule_after(tgm_present.deadline_work,
					  TGM_PRESENT_WAKE_GRACE_NS - (now - tgm_present.late));
		return 1;
	}
	if (__atomic_load_n(&tgm_present.watch.state, __ATOMIC_ACQUIRE) !=
	    SWITCH_GPU_PRESENT_WATCH_ARMED)
		return 1;
	nvxnu_panic(
		"tegra-gm20b: present watch lost %llu ms after the offer: producer uid %u fence %u "
		"(status %d): %s; interrupt after %u, %.16s fence %u, last pass at %u",
		age / 1000000, token->queue, (u32)token->value, physical, why, end,
		chan ? chan->owner : "-", chan ? tgm_chan_completed(chan) : 0,
		chan ? READ_ONCE(chan->progress_done) : 0);
}

void tgm_present_deadline(void *arg)
{
	struct tgm_chan *chan = NULL;
	struct tgm_chan *waited = NULL;
	struct switch_gpu_present_pending pending;
	char state[TGM_HANG_LINE];
	char what[96], current_owner[96], next_owner[96];
	struct tgm_chan *late = NULL;
	u64 age = 0;
	u32 seqno = 0;
	u32 eng;
	u32 watch = 0;
	int watched = 0;

	(void)arg;
	mutex_lock(&tgm_present.lock);
	watch = __atomic_load_n(&tgm_present.watch.state, __ATOMIC_ACQUIRE);
	if ((!__atomic_load_n(&tgm_present.pending, __ATOMIC_ACQUIRE) &&
	     watch != SWITCH_GPU_PRESENT_WATCH_ARMED) ||
	    __atomic_load_n(&tgm_present.redoing, __ATOMIC_ACQUIRE)) {
		mutex_unlock(&tgm_present.lock);
		return;
	}
	/* a deadline of an earlier present that was already running when that one completed: this one has its own time,
	 * which a recovery under way restarts */
	age = nvxnu_nanotime() - max(tgm_present.queued, tgm_present.recovered);
	if (age < tgm_present.recheck) {
		nvxnu_work_schedule_after(tgm_present.deadline_work, tgm_present.recheck - age);
		mutex_unlock(&tgm_present.lock);
		return;
	}
	age = nvxnu_nanotime() - tgm_present.queued;
	if (tgm_present.pending &&
	    (s32)(tgm_chan_completed(tgm_present.chan) - tgm_present.pending) >= 0) {
		nvxnu_panic("tegra-gm20b: present fence %u complete after %llu ms but its nonstall "
			    "interrupt never came",
			    tgm_present.pending, age / 1000000);
	}
	/* Timeline status may lock engine state; never query it while holding tgm_live_lock. */
	pending = switch_gpu_present_tokens_pending(&tgm_present.tokens);
	if (!tgm_present.pending)
		watched = switch_gpu_backing_token_physical_status(&tgm_present.watch.token, 0);
	mutex_lock(&tgm_live_lock);
	if (watched) {
		if (watch_late(watched, age, &waited, &seqno)) {
			mutex_unlock(&tgm_live_lock);
			mutex_unlock(&tgm_present.lock);
			return;
		}
	} else if (pending.token.engine == SWITCH_GPU_ENGINE_GM20B) {
		chan = live_by_uid(pending.token.queue);
		if (chan && switch_gpu_present_pending_matches(&pending, chan->id,
							       tgm_chan_completed(chan))) {
			waited = chan;
			seqno = (u32)pending.token.value;
		}
	}
	late = waited ? waited : tgm_present.chan;
	if (tgm_hang_recover(late, "present", waited ? seqno : tgm_present.pending, age) !=
	    TGM_HANG_STUCK) {
		tgm_present.recheck = TGM_PRESENT_NS;
		mutex_unlock(&tgm_live_lock);
		tgm_present.recovered = nvxnu_nanotime();
		nvxnu_work_schedule_after(tgm_present.deadline_work, tgm_present.recheck);
		mutex_unlock(&tgm_present.lock);
		return;
	}
	if (waited) {
		present_channel_brief(waited, waited->chid, what, sizeof(what));
	} else {
		scnprintf(what, sizeof(what), "no pending GPU producer");
	}
	eng = nvkm_rd32(tgm_gpu.device, PFIFO_ENG_STATUS(0));
	present_channel_brief(eng & PFIFO_ENG_ID_TSG ? NULL : live_by(0, PFIFO_ENG_ID(eng)),
			      PFIFO_ENG_ID(eng), current_owner, sizeof(current_owner));
	present_channel_brief(eng & PFIFO_ENG_NEXT_ID_TSG ? NULL
							  : live_by(0, PFIFO_ENG_NEXT_ID(eng)),
			      PFIFO_ENG_NEXT_ID(eng), next_owner, sizeof(next_owner));
	tgm_chan_describe(tgm_present.chan, state, sizeof(state));
	nvxnu_panic("tegra-gm20b: present timeout %llums; typed %u pending %u:%u:%llu status %d "
		    "physical %d; "
		    "GR %08x current{%.80s} next{%.80s}; waiting{%.80s} seq %u; present{%.448s}",
		    (nvxnu_nanotime() - tgm_present.queued) / 1000000, tgm_present.tokens.count,
		    pending.token.engine, pending.token.queue, pending.token.value, pending.status,
		    pending.physical, eng, current_owner, next_owner, what, seqno, state);
}
