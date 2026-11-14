// SPDX-License-Identifier: MIT
// A model of GM20B's runlist 0 when one client's channel is killed while other clients have work on it: GR and the copy
// engine behind PBDMA0, PFIFO_PREEMPT taking one preempt at a time, nvkm_runl_rc with recover_engine.c's hold and
// release around each engine reset, and the present's hang checks, each decision taken with TegraGM20BRecover.h as
// recover_engine.c, recovery.c and gpu_hang.c take it.
//
// Hardware rules:
//  - NVIDIA's gv100 dev_pbdma.ref (NV_PPBDMA_INTR_0_ENG_RESET), taken to hold for GM20B: an engine reset while the PBDMA
//    processes a channel of its runlist raises ENG_RESET, which stalls the PBDMA on that channel until it is cleared; a
//    halted PBDMA raises none. dev_fifo.ref's channel teardown: a preempt completes once the PBDMA's interrupts are
//    handled. Boot 21's 48 resets each found METHOD0 empty at the hold and raised none.
//  - Inferred: a preempt asked for the PBDMA's channel halts it. A preempt lands once no engine runs or still saves its
//    channel's context: work in flight finishes first, and a context an engine cannot save (a faulted channel's) holds
//    it until that engine is reset. It takes the channel off the PBDMA with its method kept, and its context off the
//    engines.
//  - Inferred, as gm20b_kernel_hang_model.h infers it from boot 20: an ENG_RESET acknowledged after a reset of the engine
//    that was switching to the PBDMA's channel leaves the PBDMA on that channel's method with no engine switching to
//    take it, until a preempt of the channel lands.
// nvkm_runl_rc's wait for the preempt in flight is a 2 s BUG_ON: a preempt still pending at its end is a panic.
#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <vector>
#include "drivers/TegraGM20B/TegraGM20BRecover.h"

namespace bystander
{
constexpr uint64_t kMs = 1000 * 1000;
constexpr int kGr = 0, kCe = 1;
constexpr int kPresent = 1;  // the present's kernel channel
constexpr uint64_t kPresentCheckNs = 1000 * kMs;

struct Job {
	int engine;
	int steps = 1;  // the engine's time for it, in 1 ms steps
	bool faults = false;
};

struct Chan {
	bool kernel = false, alive = true;
	std::deque<Job> jobs;
	uint32_t submitted = 0, completed = 0, failed = 0;
	uint64_t since = 0;  // its last completion, or when it last went from idle to work pending
	uint32_t strikes = 0, seen = 0;
	int reopens = 0;
};

struct Engine {
	int ctx = -1, next = -1;  // the context it holds (VALID) and the one a switch loads
	bool switching = false;
	bool fecs = false;   // FECS began the load: engn_cxid names NEXT_ID then
	int busy = 0;        // steps left of ctx's job
	bool stuck = false;  // ctx's context cannot be saved until the engine is reset
};

enum class Rule {
	Before,  // 3006711cb: a hold under another channel's preempt in flight leaves the PBDMA's live channel as it is
	Shipped,
};

struct Gpu {
	Rule rule;
	std::map<int, Chan> chans;
	Engine eng[2];
	int pbdma = -1;
	bool halted = false, stalled = false, wedged = false;
	uint32_t preempt = 0;  // PFIFO_PREEMPT
	int landIn = 0;  // steps before a preempt whose channel is free lands (its context save)
	uint32_t holds = 0;
	int owed = -1;
	uint64_t now = 0, presentChecked = 0;
	int raised = 0, engineResets[2] = {0, 0}, panics = 0;
	bool raisedNow = false,
	     holdOwed = false;  // this hold's reset raised ENG_RESET; this hold owed a preempt
	// against dev_fifo.ref's teardown: a preempt of a channel ENG_RESET was raised on never lands
	bool engResetStuck = false;
	std::vector<int> tookEngReset;
	std::vector<int> killed;
	// a live channel an engine reset found on a PBDMA no preempt halted, and whether by the end of nvkm_runl_rc a preempt
	// was asked for it or it had left the PBDMA by itself
	int rodeThrough = -1;
	bool rodeThroughSettled = false;

	explicit Gpu(Rule r) : rule(r)
	{
		chans[kPresent].kernel = true;
	}

	bool alive(int chid) const
	{
		auto it = chans.find(chid);
		return it != chans.end() && it->second.alive;
	}

	void submit(int chid, Job job)
	{
		Chan &c = chans[chid];
		if (c.jobs.empty())
			c.since = now;
		c.jobs.push_back(job);
		c.submitted++;
	}

	uint32_t status(int e) const
	{
		const Engine &x = eng[e];
		if (x.switching && x.ctx >= 0)
			return PFIFO_ENG_BUSY | PFIFO_ENG_CHSW |
			       uint32_t(PFIFO_ENG_CTX_SWITCH) << 13 | uint32_t(x.next) << 16 |
			       uint32_t(x.ctx);
		if (x.switching)
			return PFIFO_ENG_CHSW | uint32_t(PFIFO_ENG_CTX_LOAD) << 13 |
			       uint32_t(x.next) << 16;
		if (x.ctx >= 0)
			return (x.busy ? PFIFO_ENG_BUSY : 0) | uint32_t(PFIFO_ENG_CTX_VALID) << 13 |
			       uint32_t(x.ctx) << 16 | uint32_t(x.ctx);
		return 0;
	}

	// recover.c engn_cxid; the copy engine has no FECS
	int cxid(int e) const
	{
		int tsg = 0;
		const uint32_t s = status(e);
		return tgm_engine_chid(
			s, PFIFO_ENG_CTX_STATUS(s) == PFIFO_ENG_CTX_SWITCH && eng[e].fecs, &tsg);
	}

	uint32_t pbdmaStatus() const
	{
		return pbdma >= 0 ? uint32_t(1) << 13 | uint32_t(pbdma) << 16 | uint32_t(pbdma) : 0;
	}

	// PFIFO_PREEMPT's channel off the PBDMA and the engines, once nothing of it runs or waits to be saved and its save
	// took landIn steps (tick: a step passed)
	void land(bool tick)
	{
		if (!(preempt & PFIFO_PREEMPT_PENDING))
			return;
		const int chid = int(PFIFO_PREEMPT_ID(preempt));
		if (stalled && pbdma == chid)
			return;
		if (engResetStuck &&
		    std::find(tookEngReset.begin(), tookEngReset.end(), chid) != tookEngReset.end())
			return;
		for (const Engine &x : eng)
			if (x.ctx == chid && (x.busy || x.stuck))
				return;
		if (landIn > 0) {
			if (tick)
				landIn--;
			return;
		}
		for (Engine &x : eng) {
			if (x.ctx == chid && !x.switching)
				x.ctx = -1;
			if (x.switching && x.next == chid) {
				x.switching = false;
				x.next = -1;
			}
		}
		if (pbdma == chid) {
			pbdma = -1;
			halted = false;
			wedged = false;
		}
		preempt = 0;
	}

	void ask(int chid)
	{
		preempt = PFIFO_PREEMPT_PENDING | uint32_t(chid);
		halted = pbdma == chid;
	}

	// recover_engine.c hold_pbdmas
	void hold()
	{
		if (!tgm_reset_hold_enter(&holds))
			return;
		raisedNow = false;
		const enum tgm_reset_pbdma was = tgm_reset_pbdma(pbdmaStatus(), alive(pbdma));
		const int due = tgm_reset_owed(was, preempt, pbdma);
		if (was == TGM_RESET_PBDMA_LIVE && !(preempt & PFIFO_PREEMPT_PENDING))
			ask(pbdma);
		holdOwed = rule == Rule::Shipped && due >= 0;
		if (holdOwed)
			owed = due;
	}

	// recover_engine.c owed_preempt
	void owedStep()
	{
		const enum tgm_owed step = tgm_owed_step(owed, preempt, pbdma == owed, alive(owed));
		if (step == TGM_OWED_ASK)
			ask(owed);
		if (step != TGM_OWED_WAITS && step != TGM_OWED_NONE && owed == rodeThrough)
			rodeThroughSettled = true;
		if (step != TGM_OWED_WAITS)
			owed = -1;
	}

	// recover_engine.c tgm_recover_reset_release
	void release()
	{
		if (!tgm_reset_hold_leave(&holds))
			return;
		if (stalled && !alive(pbdma))
			chans[pbdma]
				.jobs.clear();  // the killed channel's method replaced with a NOP
		stalled = false;
		if (holdOwed && !raisedNow)
			owed = -1;
		if (rule == Rule::Shipped)
			owedStep();
	}

	// a reset under the hold: ENG_RESET for a PBDMA on a channel's method (a killed channel has none left)
	void reset(int e)
	{
		hold();
		if (pbdma >= 0 && !halted && !chans[pbdma].jobs.empty()) {
			stalled = true;
			raised++;
			raisedNow = true;
			tookEngReset.push_back(pbdma);
			rodeThrough = pbdma;
			rodeThroughSettled = (preempt & PFIFO_PREEMPT_PENDING) &&
					     int(PFIFO_PREEMPT_ID(preempt)) == pbdma;
			wedged = eng[e].switching && eng[e].next == pbdma;
		}
		eng[e] = Engine();
		engineResets[e]++;
		land(false);
		release();
		land(false);
	}

	void kill(int chid)
	{
		Chan &c = chans[chid];
		if (!c.alive)
			return;
		c.alive = false;
		c.failed += uint32_t(c.jobs.size());
		c.jobs.clear();
		killed.push_back(chid);
	}

	// nouveau's nvkm_runl_rc for a killed client: the engines on its context reset, then its wait for the preempt in
	// flight, then the owed preempt (tgm_recover_owed_preempt)
	void recover(int chid)
	{
		kill(chid);
		for (int e : {kGr, kCe})
			if (cxid(e) == chid)
				reset(e);
		for (int i = 0; i < 2000 && (preempt & PFIFO_PREEMPT_PENDING); i++)
			step();
		if (preempt & PFIFO_PREEMPT_PENDING)
			panics++;
		if (rule == Rule::Shipped)
			owedStep();
	}

	void complete(int chid)
	{
		Chan &c = chans[chid];
		c.jobs.pop_front();
		c.completed++;
		c.since = now;
	}

	int pick() const
	{
		for (const auto &[chid, c] : chans)
			if (c.alive && !c.jobs.empty() && chid != kPresent)
				return chid;
		return alive(kPresent) && !chans.at(kPresent).jobs.empty() ? kPresent : -1;
	}

	// One millisecond of the engines and PBDMA0; a faulting job's kill comes from step()'s caller
	int step()
	{
		now += kMs;
		for (Engine &x : eng) {
			if (x.busy && --x.busy == 0 && x.ctx >= 0 && alive(x.ctx))
				complete(x.ctx);
		}
		land(true);
		if (stalled || wedged || halted)
			return -1;
		if (pbdma >= 0 && (!alive(pbdma) || chans[pbdma].jobs.empty()))
			pbdma = -1;
		if (pbdma < 0)
			pbdma = pick();
		if (pbdma < 0)
			return -1;
		const Job &job = chans[pbdma].jobs.front();
		Engine &x = eng[job.engine];
		if (x.busy)
			return -1;
		if (x.switching) {
			if (x.ctx >= 0 && x.stuck)
				return -1;
			x.ctx = x.next;
			x.switching = false;
			x.stuck = false;
			return -1;
		}
		if (x.ctx != pbdma) {
			x.switching = true;
			x.next = pbdma;
			return -1;
		}
		if (job.faults) {
			x.stuck = true;
			return pbdma;
		}
		x.busy = job.steps;
		return -1;
	}

	// gpu_hang.c tgm_hang_recover for the present's late fence: the owed preempt first, then the blame
	void check()
	{
		if (rule == Rule::Shipped && owed >= 0) {
			const int before = owed;
			owedStep();
			if (preempt == (PFIFO_PREEMPT_PENDING | uint32_t(before)))
				return;
		}
		std::vector<int> chids;
		for (int e : {kGr, kCe})
			if (status(e) & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW))
				chids.push_back(cxid(e));
		if (pbdma >= 0)
			chids.push_back(pbdma);
		const int resident = int(chids.size());
		chids.push_back(kPresent);
		std::vector<tgm_hang_candidate> c(chids.size());
		bool progress = false;
		for (const auto &[chid, ch] : chans)
			progress |= ch.alive && now - ch.since < TGM_STUCK_NS;
		std::map<int, uint8_t> looked;
		for (size_t i = 0; i < chids.size(); i++) {
			Chan &ch = chans[chids[i]];
			c[i].live = ch.alive;
			c[i].dead = !ch.alive;
			c[i].kernel = ch.kernel;
			c[i].resident = int(i) < resident;
			c[i].moving = ch.alive && now - ch.since < TGM_STUCK_NS;
			if (!looked.count(chids[i]))
				looked[chids[i]] = uint8_t(tgm_hang_confirm(
					ch.alive &&
						tgm_stuck(0, ch.completed, ch.submitted, now,
							  ch.since) &&
						!c[i].moving,
					ch.completed, &ch.strikes, &ch.seen));
			c[i].stuck = looked[chids[i]];
		}
		int b = -1;
		const enum tgm_hang verdict =
			tgm_hang_blame(c.data(), int(c.size()), 0, 1, progress, &b);
		if (verdict == TGM_HANG_RECOVERING && b >= 0 && !chans[chids[b]].kernel) {
			const int blamed = chids[b];
			recover(blamed);
			chans[blamed].reopens++;
			// a later reset's release replaces the dead channel's method with a NOP
			if (pbdma == blamed) {
				pbdma = -1;
				wedged = halted = false;
			}
		}
	}

	// steps until every live channel's work is done or `ms` passed; the present's late fence is checked each second
	void run(int ms)
	{
		for (int i = 0; i < ms && !panics; i++) {
			const int faulted = step();
			if (faulted >= 0)
				recover(faulted);
			const Chan &p = chans[kPresent];
			if (!p.jobs.empty() &&
			    now - std::max(p.since, presentChecked) >= kPresentCheckNs) {
				presentChecked = now;
				check();
			}
			if (pick() < 0 && !eng[kGr].busy && !eng[kCe].busy)
				return;
		}
	}

	bool done(int chid) const
	{
		const Chan &c = chans.at(chid);
		return c.alive && c.completed == c.submitted && c.failed == 0 && c.reopens == 0;
	}
};
}  // namespace bystander
