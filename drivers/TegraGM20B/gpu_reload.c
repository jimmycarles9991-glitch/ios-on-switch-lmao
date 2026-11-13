// SPDX-License-Identifier: MIT
// Engine resets for stalls no client is to blame for: a live channel's stuck context load (TegraGM20BRecover.h
// tgm_load_verdict), and a kernel channel's stall at a context-switch timeout tick (tgm_kernel_stall_verdict) or at a
// hang check (TegraGM20BKernelHang.h).
#include "gpu_internal.h"
#include "../../kits/SwitchLinux/SwitchLinux.h"

/* tgm_live_lock: the line names the channel and every engine, past nvxnu_log's 512 characters and within os_log's 1,024 */
static char load_line[1024];

/* tgm_live_lock held: the live client channels whose contexts the GPU holds */
static int resident_clients(struct tgm_chan **out, int max)
{
	int chids[8];
	const int held = tgm_recover_resident(tgm_gpu.device, chids, ARRAY_SIZE(chids));
	struct tgm_chan *chan = NULL;
	int n = 0;
	int i = 0;

	for (i = 0; i < held && n < max; i++) {
		chan = live_by(0, chids[i]);
		if (chan && !chan->kernel && !READ_ONCE(chan->error))
			out[n++] = chan;
	}
	return n;
}

static u32 kill_clients(struct tgm_chan *kernel, const char *engine, struct tgm_chan **clients,
			int n, u64 now)
{
	char state[TGM_HANG_LINE];
	char report[sizeof(kernel->fault)];
	u32 killed = 0;
	int i = 0;

	for (i = 0; i < n; i++) {
		if (clients[i]->isolating)
			continue;
		tgm_chan_describe(clients[i], state, sizeof(state));
		scnprintf(report, sizeof(report),
			  "CTXSW_TIMEOUT: on %s while %s's channel %u was stuck there again: %s",
			  engine, kernel->owner, kernel->chid, state);
		tgm_chan_isolate(clients[i], report, now);
		tgm_recover_fault(tgm_gpu.device, (int)clients[i]->chid);
		killed++;
	}
	return killed;
}

enum tgm_ctxsw tgm_kernel_stall_locked(struct tgm_chan *chan, const char *engine, int again,
				       int load, int fatal_ok, u32 *killed)
{
	struct tgm_chan *clients[4];
	char state[TGM_HANG_LINE];
	char engines[TGM_HANG_LINE];
	char regs[TGM_HANG_LINE];
	const u64 now = nvxnu_nanotime();
	const u32 done = tgm_chan_completed(chan);
	const int idle = !load && tgm_switch_unowned(READ_ONCE(chan->error) != 0, done,
						     READ_ONCE(chan->submitted));
	const int not_back =
		tgm_engine_not_back(chan->ctxsw_clean_ns, chan->ctxsw_clean_done, now, done);
	const int n = resident_clients(clients, ARRAY_SIZE(clients));
	const enum tgm_ctxsw verdict = tgm_kernel_stall_verdict(idle, load, not_back, n);

	*killed = 0;
	tgm_chan_describe(chan, state, sizeof(state));
	tgm_engines_line(engines, sizeof(engines));
	if (verdict == TGM_CTXSW_FATAL) {
		if (!fatal_ok)
			return verdict;
		tgm_engine_state(regs, sizeof(regs));
		nvxnu_panic("tegra-gm20b: hardware failure: %s did not come back after a reset "
			    "with no client "
			    "context resident: stuck %s %s's channel %u again %llu ms after it, "
			    "nothing "
			    "completed since: %s; engines %s; %s",
			    engine, load ? "loading" : "switching off", chan->owner, chan->chid,
			    (now - chan->ctxsw_clean_ns) / 1000000, state, engines, regs);
	}
	if (tgm_kernel_stall_kills(idle, again, not_back, n))
		*killed = kill_clients(chan, engine, clients, n, now);
	if (verdict == TGM_CTXSW_REDO)
		tgm_ctxsw_redo_mark(chan);
	if (load) {
		chan->load_reset = 1;
		chan->load_reset_done = done;
	}
	chan->ctxsw_clean_ns = n ? 0 : now;
	chan->ctxsw_clean_done = done;
	scnprintf(load_line, sizeof(load_line),
		  "tegra-gm20b: %s stuck %s %s's channel %u%s: the engine reset%s, %u client "
		  "channels killed of "
		  "%d resident: %s; engines %s\n",
		  engine, load ? "loading" : "switching off", chan->owner, chan->chid,
		  again ? " again" : "",
		  verdict == TGM_CTXSW_REDO ? ", its work redone on the GPU" : "", *killed, n,
		  state, engines);
	switch_linux_log_line(load_line);
	if (chan == tgm_present.chan)
		tgm_present_redo_kick();
	return verdict;
}

/* tgm_live_lock held */
static enum tgm_ctxsw load_verdict(struct tgm_chan *chan, const char *engine, const char *who)
{
	char state[TGM_HANG_LINE];
	char engines[TGM_HANG_LINE];
	char report[sizeof(chan->fault)];
	const u32 done = chan ? tgm_chan_completed(chan) : 0;
	const enum tgm_ctxsw verdict = tgm_load_verdict(
		chan != NULL, chan && READ_ONCE(chan->error) != 0,
		chan && tgm_load_reloaded(chan->load_reset, chan->load_reset_done, done));
	u32 killed = 0;

	if (verdict != TGM_CTXSW_RELOAD && verdict != TGM_CTXSW_FATAL)
		return verdict;
	if (chan->kernel)
		return tgm_kernel_stall_locked(chan, engine, verdict == TGM_CTXSW_FATAL, 1, 1,
					       &killed);
	tgm_chan_describe(chan, state, sizeof(state));
	tgm_engines_line(engines, sizeof(engines));
	if (verdict == TGM_CTXSW_FATAL) {
		/* the client's own context: the engine cannot load it even after a reset under the load */
		if (!chan->isolating) {
			scnprintf(report, sizeof(report),
				  "CTXSW_TIMEOUT: %s stuck loading its context again after an "
				  "engine reset "
				  "under it, nothing completed since: %s",
				  engine, state);
			tgm_chan_isolate(chan, report, nvxnu_nanotime());
		}
		scnprintf(load_line, sizeof(load_line),
			  "tegra-gm20b: %s stuck loading %s's channel %u context again after an "
			  "engine reset "
			  "under it, nothing completed since: the channel killed: %s; engines %s\n",
			  engine, chan->owner, chan->chid, state, engines);
		switch_linux_log_line(load_line);
		return TGM_CTXSW_KILL;
	}
	chan->load_reset = 1;
	chan->load_reset_done = done;
	scnprintf(load_line, sizeof(load_line),
		  "tegra-gm20b: %s stuck loading %s's channel %u context (%s): the engine reset "
		  "under it, nothing killed: %s; engines %s\n",
		  engine, chan->owner, chan->chid, who, state, engines);
	switch_linux_log_line(load_line);
	return verdict;
}

enum tgm_ctxsw tgm_ctxsw_load_decide(int chid, const char *engine)
{
	enum tgm_ctxsw verdict = TGM_CTXSW_RESET;

	mutex_lock(&tgm_live_lock);
	verdict = load_verdict(live_by(0, chid), engine, "context-switch timeout");
	mutex_unlock(&tgm_live_lock);
	return verdict;
}

int tgm_load_recover_locked(struct tgm_chan *chan, const char *who)
{
	const char *engine = tgm_recover_loading(tgm_gpu.device, (int)chan->chid);

	if (!engine || load_verdict(chan, engine, who) != TGM_CTXSW_RELOAD)
		return 0;
	return tgm_recover_reload(tgm_gpu.device, (int)chan->chid) > 0;
}

int tgm_chid_live_client(int chid)
{
	const struct tgm_chan *chan = live_by(0, chid);

	return chan && !chan->kernel && !READ_ONCE(chan->error);
}

void tgm_chid_cut(int chid)
{
	struct tgm_chan *chan = live_by(0, chid);

	if (chan && chan->kernel && !READ_ONCE(chan->error))
		tgm_ctxsw_redo_mark(chan);
}

/* tgm_live_lock held: the live client channels whose contexts the engines of hardware channel chid's runlist hold,
 * VALID or switching, or whose methods its PBDMAs are on */
static int held_clients(int chid, struct tgm_chan **out, int max)
{
	int chids[8];
	const int held = tgm_recover_held(tgm_gpu.device, chid, chids, ARRAY_SIZE(chids));
	int n = 0;
	int i = 0;

	for (i = 0; i < held && n < max; i++) {
		if (tgm_chid_live_client(chids[i]))
			out[n++] = live_by(0, chids[i]);
	}
	return n;
}

enum tgm_hang tgm_kernel_hang_locked(struct tgm_chan *stuck, struct tgm_chan *late, const char *who,
				     u32 seqno, u64 waited_ns, u64 moved_ns)
{
	static const char *const steps[] = {
		[TGM_KERNEL_HANG_OWN] = "its own engines",
		[TGM_KERNEL_HANG_ALL] = "every engine of its runlist",
	};
	struct tgm_kernel_hang_state *hang = &stuck->kernel_hang;
	struct tgm_chan *clients[8];
	char state[TGM_HANG_LINE];
	char engines[TGM_HANG_LINE];
	char host[TGM_HANG_LINE];
	const u64 now = nvxnu_nanotime();
	const u32 done = tgm_chan_completed(stuck);
	int its = 0;
	const int other = tgm_recover_preempting(tgm_gpu.device, (int)stuck->chid, &its);
	const int pbdma = tgm_recover_pbdma_chid(tgm_gpu.device);
	const enum tgm_kernel_hang step = tgm_kernel_hang_next(
		hang, done, tgm_kernel_hang_moved(hang, moved_ns, its, pbdma), now);
	u32 killed = 0;
	int reset = -1;
	int n = 0;

	if (step == TGM_KERNEL_HANG_WAIT)
		return TGM_HANG_RECOVERING;
	tgm_chan_describe(stuck, state, sizeof(state));
	tgm_engines_line(engines, sizeof(engines));
	tgm_recover_host_line(tgm_gpu.device, host, sizeof(host));
	if (step == TGM_KERNEL_HANG_FATAL) {
		nvxnu_panic("tegra-gm20b: hardware failure: the engines of %s's channel %u "
			    "did not come back after a reset with no client context resident: "
			    "%s waited %llu ms for %s fence %u, no channel completed anything "
			    "since and no preempt took it off its PBDMA: %s; engines %s; %s",
			    stuck->owner, stuck->chid, who, waited_ns / 1000000, late->owner, seqno,
			    state, engines, host);
	}
	if (!tgm_kernel_hang_put_off(hang, step, other, done, now)) {
		if (step == TGM_KERNEL_HANG_ALL) {
			n = held_clients((int)stuck->chid, clients, ARRAY_SIZE(clients));
			killed =
				kill_clients(stuck, "every engine of its runlist", clients, n, now);
		}
		tgm_ctxsw_redo_mark(stuck);
		reset = tgm_recover_kernel(tgm_gpu.device, (int)stuck->chid,
					   step == TGM_KERNEL_HANG_ALL);
		tgm_kernel_hang_took(hang, step, n, pbdma,
				     !other && pbdma == (int)stuck->chid && reset > 0, done, now);
	}
	scnprintf(
		load_line, sizeof(load_line),
		"tegra-gm20b: GPU hang on kernel channel: %s waited %llu ms for %s fence %u; %s's "
		"channel %u stuck, %s %s (%d), %u client channels killed of %d resident: %s; "
		"engines %s; %s\n",
		who, waited_ns / 1000000, late->owner, seqno, stuck->owner, stuck->chid,
		steps[step], reset < 0 ? "put off, a preempt in flight" : "reset, its work redone",
		reset, killed, n, state, engines, host);
	switch_linux_log_line(load_line);
	if (stuck == tgm_present.chan)
		tgm_present_redo_kick();
	return TGM_HANG_RECOVERING;
}
