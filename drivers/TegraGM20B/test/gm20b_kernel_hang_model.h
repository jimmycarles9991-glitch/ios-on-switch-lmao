// SPDX-License-Identifier: MIT
// A model of GM20B's runlist 0 for T-v5a's panic: GR and the copy engine behind PBDMA0, clients whose first GR job
// faults (each kill: nvkm's recovery resets the engines on its context under recover_engine.c's hold, its pool memory
// queued to the scrubber), the scrubber's copy jobs and its deadline deciding with TegraGM20BRecover.h and
// TegraGM20BKernelHang.h, as gpu_scrub.c, gpu_hang.c and gpu_reload.c take those decisions.
//
// Two hardware rules are inferred, not shown by the device (boot 20's panic line prints ENG0 and PBDMA0 only):
//  - an engine reset while it switches to the live channel PBDMA0 is on, that channel not preempted off PBDMA0 first
//    (another preempt was in flight, so the hold asked for none), leaves PBDMA0 on that channel's method with the
//    engine idle and no switch pending: boot 20's state, with no interrupt to say so;
//  - a preempt of that channel that lands (the hold of a later reset, with no other preempt in flight) takes it off
//    PBDMA0, and its next load delivers the method.
// Another preempt (a compositor kick's, a hold's) is in flight in one 10 ms step in eight.
#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <map>
#include <random>
#include <vector>
#include "drivers/TegraGM20B/TegraGM20BRecover.h"

constexpr uint64_t kMs = 1000 * 1000;
constexpr uint64_t kTick = 10 * kMs;
constexpr uint64_t kScrubNs = 1000 * kMs;  // gpu_scrub.c SCRUB_NS
constexpr int kGr = 0, kCe = 1, kScrub = 0;

struct Job {
	int engine;
	bool faults;
};

struct Chan {
	bool kernel = false, alive = true;
	uint32_t submitted = 0, completed = 0;
	uint64_t progress = 0,
		 busy = 0;  // its last completion; when it last went from idle to work pending
	uint32_t strikes = 0, seen = 0;
	tgm_kernel_hang_state hang = {};
	bool sticky = false;  // its method is never taken again after a wedge (stickyWedge)
	int lastResort = 0;
	uint32_t lastResortDone = 0;
	std::deque<Job> jobs;
	std::deque<uint64_t> queued;  // the scrubber's: each job's queue time
};

struct Engine {
	int ctx = -1, next = -1;
	bool valid = false, switching = false;
};

enum class Rule {
	Before,  // 6079249be: a stuck kernel channel is TGM_HANG_STUCK, the last resort resets busy engines once
	Shipped,
};

struct Gpu {
	Rule rule;
	std::mt19937 rng;
	bool brokenEngine = false;  // no reset frees PBDMA0 and no preempt of its channel lands
	int stuckPreempt =
		-1;  // brokenEngine: the channel a preempt that never lands is pending for
	bool stickyWedge =
		false;  // a wedged channel reloaded after a preempt wedges PBDMA0 again at its method
	int waiters =
		1;  // hang checks that find the stall at each deadline (the present's, CPU waits, ...)
	std::map<int, Chan> chans;
	Engine eng[2];
	int pbdma = -1;
	bool wedged = false, inflight = false;
	uint64_t now = 0, deadlineAt = 0, recovered = 0;
	uint32_t redoThrough = 0;
	bool redo = false;
	int nextChid = 1;
	std::vector<int> rc;
	int kills = 0, wedges = 0, dirty = 0, panics = 0;
	std::vector<int> steps;
	std::vector<uint64_t> stepNs;
	bool panicState = false;  // at the panic: PBDMA0 on a method, no engine busy or switching

	explicit Gpu(Rule r, unsigned seed) : rule(r), rng(seed)
	{
		chans[kScrub].kernel = true;
		for (int i = 0; i < 4; i++)
			client();
	}

	bool chance(unsigned in)
	{
		return rng() % in == 0;
	}

	void client()
	{
		Chan &c = chans[nextChid++];
		c.jobs = {{kCe, false}, {kGr, true}};
		c.submitted = 2;
		c.busy = now;
	}

	bool alive(int chid)
	{
		auto it = chans.find(chid);
		return it != chans.end() && it->second.alive;
	}

	void scrubSubmit()
	{
		Chan &s = chans[kScrub];
		s.jobs.push_back({kCe, false});
		s.queued.push_back(now);
		s.submitted++;
		if (s.jobs.size() == 1)
			s.busy = now;
		deadlineAt = now + kScrubNs;
	}

	// PFIFO_ENG_STATUS as the model's engine would read: a switch from a context is SWITCH (BUSY), one from none is a
	// LOAD with BUSY clear
	uint32_t status(int e) const
	{
		const Engine &x = eng[e];
		if (x.switching && x.valid)
			return PFIFO_ENG_BUSY | PFIFO_ENG_CHSW |
			       uint32_t(PFIFO_ENG_CTX_SWITCH) << 13 | uint32_t(x.next) << 16 |
			       uint32_t(x.ctx);
		if (x.switching)
			return PFIFO_ENG_CHSW | uint32_t(PFIFO_ENG_CTX_LOAD) << 13 |
			       uint32_t(x.next) << 16;
		if (x.valid)
			return uint32_t(PFIFO_ENG_CTX_VALID) << 13 | uint32_t(x.ctx) << 16 |
			       uint32_t(x.ctx);
		return 0;
	}

	// the channel nvkm's recovery reads off an engine (recover.c engn_cxid; the copy engine has no FECS)
	int cxid(int e) const
	{
		const Engine &x = eng[e];
		if (x.switching)
			return x.valid ? x.ctx : x.next;
		return x.valid ? x.ctx : -1;
	}

	// recover_engine.c's hold around one engine reset: a live channel on PBDMA0 preempted off it unless another
	// preempt is in flight, a killed one's method dropped at the release
	void reset(int e)
	{
		if (pbdma >= 0 && wedged && brokenEngine) {
			stuckPreempt = alive(pbdma) && !inflight ? pbdma : stuckPreempt;
		} else if (pbdma >= 0 && (!alive(pbdma) || !inflight)) {
			if (wedged && stickyWedge && alive(pbdma) && chans[pbdma].kernel)
				chans[pbdma].sticky = true;
			pbdma = -1;
			wedged = false;
		}
		if (pbdma >= 0 && alive(pbdma) && eng[e].switching && eng[e].next == pbdma &&
		    !chans[pbdma].jobs.empty() && chans[pbdma].jobs.front().engine == e) {
			wedged = true;
			wedges++;
		}
		eng[e] = Engine();
	}

	// nvkm_runl_rc for a killed channel: the engines on its context (engn_cxid: a SWITCH names ID) reset, it leaves
	void recover(int chid)
	{
		for (int e : {kGr, kCe}) {
			if (cxid(e) == chid)
				reset(e);
		}
		if (pbdma == chid && !wedged)
			pbdma = -1;
		chans[chid].jobs.clear();
	}

	void kill(int chid)
	{
		Chan &c = chans[chid];
		if (!c.alive || c.kernel)
			return;
		c.alive = false;
		rc.push_back(chid);
		kills++;
		scrubSubmit();
		client();
	}

	int pick()
	{
		std::vector<int> ready;
		for (auto &[chid, c] : chans)
			if (c.alive && !c.jobs.empty())
				ready.push_back(chid);
		return ready.empty() ? -1 : ready[rng() % ready.size()];
	}

	void complete(int chid)
	{
		Chan &c = chans[chid];
		c.jobs.pop_front();
		c.completed++;
		c.progress = now;
		if (chid != kScrub)
			return;
		const bool backDirty = redo && int32_t(redoThrough - c.completed) >= 0;
		c.queued.pop_front();
		if (redo && int32_t(c.completed - redoThrough) >= 0)
			redo = false;
		if (backDirty) {
			dirty++;
			scrubSubmit();  // the scrubber claims the dirty range again
		}
		if (c.jobs.empty())
			deadlineAt = 0;
	}

	// One step of PBDMA0 and the engine its channel's next method goes to; a fault interrupts after the PBDMA moved on
	void run()
	{
		if (wedged)
			return;
		if (pbdma >= 0 && (!alive(pbdma) || chans[pbdma].jobs.empty() || chance(3)))
			pbdma = -1;
		if (pbdma < 0)
			pbdma = pick();
		if (pbdma < 0)
			return;
		if (chans[pbdma].sticky) {
			wedged = true;
			return;
		}
		Engine &e = eng[chans[pbdma].jobs.front().engine];
		if (e.switching) {
			e.ctx = e.next;
			e.valid = true;
			e.switching = false;
			return;
		}
		if (!e.valid || e.ctx != pbdma) {
			e.switching = true;
			e.next = pbdma;
			return;
		}
		if (chans[pbdma].jobs.front().faults) {
			const int faulted = pbdma;
			pbdma = pick();
			kill(faulted);
			if (pbdma == faulted)
				pbdma = -1;
			if (pbdma >= 0) {
				Engine &n = eng[chans[pbdma].jobs.front().engine];
				if (!(n.valid && n.ctx == pbdma) && !n.switching) {
					n.switching = true;
					n.next = pbdma;
				}
			}
			return;
		}
		complete(pbdma);
	}

	void killFor(int stuck)
	{
		for (int e : {kGr, kCe}) {
			int held[2];
			const int k = tgm_engine_contexts_held(status(e), held);
			for (int i = 0; i < k; i++)
				if (held[i] != stuck && alive(held[i]))
					kill(held[i]);
		}
		if (pbdma >= 0 && pbdma != stuck)
			kill(pbdma);
	}

	int heldClients(int stuck)
	{
		int n = 0;
		for (int e : {kGr, kCe}) {
			int held[2];
			const int k = tgm_engine_contexts_held(status(e), held);
			for (int i = 0; i < k; i++)
				n += held[i] != stuck && alive(held[i]) && !chans[held[i]].kernel;
		}
		return n + (pbdma >= 0 && pbdma != stuck && alive(pbdma) && !chans[pbdma].kernel);
	}

	// recover_held.c reset_for: the engines tgm_kernel_hang_resets names for chid (all: every one)
	void resetFor(int chid, bool all)
	{
		for (int e : {kGr, kCe}) {
			int held[2];
			const uint32_t s = status(e);
			const int h = tgm_engine_contexts_held(s, held);
			int holdsChid = 0, liveClient = 0;
			for (int i = 0; i < h; i++) {
				holdsChid |= held[i] == chid;
				liveClient |= alive(held[i]) && !chans[held[i]].kernel;
			}
			if (all || tgm_kernel_hang_resets(s, holdsChid, liveClient, e == kCe))
				reset(e);
		}
	}

	// recover_held.c tgm_recover_residue: the engines still on dead chid
	bool residue(int chid)
	{
		int reset = 0;
		for (int e : {kGr, kCe})
			if ((status(e) & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW)) && cxid(e) == chid) {
				this->reset(e);
				reset++;
			}
		return reset != 0;
	}

	// gpu_reload.c tgm_kernel_hang_locked: the scrubber's ranges back dirty, then the step's resets
	void kernelHang(int stuck)
	{
		Chan &k = chans[stuck];
		uint64_t moved = 0;
		for (auto &[chid, c] : chans)
			if (c.alive)
				moved = std::max(moved, c.progress);
		const tgm_kernel_hang step = tgm_kernel_hang_next(
			&k.hang, k.completed,
			tgm_kernel_hang_moved(&k.hang, moved, stuckPreempt == stuck, pbdma), now);
		if (step == TGM_KERNEL_HANG_WAIT)
			return;
		if (step == TGM_KERNEL_HANG_FATAL) {
			steps.push_back(step);
			panics++;
			return;
		}
		if (tgm_kernel_hang_put_off(&k.hang, step, inflight, k.completed, now))
			return;
		steps.push_back(step);
		stepNs.push_back(now);
		int n = 0;
		if (step == TGM_KERNEL_HANG_ALL) {
			n = heldClients(stuck);
			killFor(stuck);
		}
		if (stuck == kScrub && !k.jobs.empty()) {
			redo = true;
			redoThrough = k.submitted;
		}
		const int on = pbdma;
		resetFor(stuck, step == TGM_KERNEL_HANG_ALL);
		tgm_kernel_hang_took(&k.hang, step, n, on, !inflight && on == stuck, k.completed,
				     now);
	}

	// 6079249be's gpu_reload.c tgm_hang_last_resort_locked and recover_engine.c tgm_recover_engines
	bool lastResort(Chan &late)
	{
		if (late.lastResort && late.completed == late.lastResortDone)
			return false;
		late.lastResort = 1;
		late.lastResortDone = late.completed;
		for (int e : {kGr, kCe})
			if (status(e) & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW))
				reset(e);
		return true;
	}

	// gpu_hang.c chan_progress_ns
	static uint64_t since(const Chan &c)
	{
		return std::max(c.progress, c.busy);
	}

	// gpu_scrub.c scrub_deadline with gpu_hang.c tgm_hang_recover's candidates (no acquires in this model)
	void deadline()
	{
		Chan &s = chans[kScrub];
		if (!deadlineAt || now < deadlineAt || s.jobs.empty())
			return;
		const uint64_t age = now - std::max(s.queued.front(), recovered);
		if (age < kScrubNs) {
			deadlineAt = now + kScrubNs - age;
			return;
		}
		for (int w = 0; w < waiters && !panics; w++)
			check();
		recovered = now;
		deadlineAt = now + kScrubNs;
	}

	// gpu_hang.c tgm_hang_recover for one waiter on the scrubber, and what the scrubber's deadline does with it
	void check()
	{
		Chan &s = chans[kScrub];
		std::vector<int> chids;
		for (int e : {kGr, kCe}) {
			if (status(e) & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW))
				chids.push_back(cxid(e));
		}
		if (pbdma >= 0)
			chids.push_back(pbdma);
		const int resident = int(chids.size());
		chids.push_back(kScrub);
		std::vector<tgm_hang_candidate> c(chids.size());
		std::map<int, uint8_t> looked;
		bool progress = false;
		for (auto &[chid, ch] : chans)
			progress |= ch.alive && now - since(ch) < TGM_STUCK_NS;
		for (size_t i = 0; i < chids.size(); i++) {
			Chan &ch = chans[chids[i]];
			c[i].live = ch.alive;
			c[i].dead = !ch.alive;
			c[i].kernel = ch.kernel;
			c[i].resident = int(i) < resident;
			c[i].moving = ch.alive && now - since(ch) < TGM_STUCK_NS;
			if (!looked.count(chids[i]))
				looked[chids[i]] = uint8_t(tgm_hang_confirm(
					ch.alive &&
						tgm_stuck(0, ch.completed, ch.submitted, now,
							  since(ch)) &&
						!c[i].moving,
					ch.completed, &ch.strikes, &ch.seen));
			c[i].stuck = looked[chids[i]];
		}
		int b = -1;
		tgm_hang verdict = TGM_HANG_PROGRESSING;
		for (;;) {
			verdict = tgm_hang_blame(c.data(), int(c.size()), 0, 1, progress, &b);
			if (verdict != TGM_HANG_RESIDUE)
				break;
			if (residue(chids[b]))
				break;
			c[b].dead = 0;
		}
		if (verdict == TGM_HANG_RECOVERING && b >= 0) {
			kill(chids[b]);
		} else if (verdict == TGM_HANG_KERNEL && rule == Rule::Shipped) {
			kernelHang(chids[b]);
		} else if (verdict == TGM_HANG_KERNEL && !lastResort(s)) {
			panics++;
			panicState = wedged && pbdma >= 0 &&
				     !(status(kGr) & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW)) &&
				     !(status(kCe) & (PFIFO_ENG_BUSY | PFIFO_ENG_CHSW));
		}
	}

	void tick()
	{
		now += kTick;
		inflight = chance(8);
		for (int chid : rc)
			recover(chid);
		rc.clear();
		run();
		deadline();
	}

	// kills until `want`, then until the scrubber has nothing left; false: a panic, or 10 minutes without draining
	bool play(int want)
	{
		const uint64_t limit = now + 600 * 1000 * kMs;
		while (!panics && now < limit && (kills < want || !chans[kScrub].jobs.empty())) {
			if (kills >= want)
				for (auto &[chid, c] : chans)
					if (!c.kernel)
						for (Job &j : c.jobs)
							j.faults = false;
			tick();
		}
		return !panics && chans[kScrub].jobs.empty();
	}
};
