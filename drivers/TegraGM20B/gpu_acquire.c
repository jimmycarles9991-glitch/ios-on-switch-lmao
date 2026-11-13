// SPDX-License-Identifier: MIT
// RAMFC words, and the PBDMA's semaphore acquire timeout as the trigger for a hang behind an acquire (TegraGM20BAcquire.h).
//
// Every channel's RAMFC ACQUIRE carries a TGM_ACQUIRE_NS timeout. An acquire that has not passed by then interrupts
// (PBDMA_INTR_0 ACQUIRE, recover.c), which runs gpu_hang.c's rule with the waiter as the late channel: what it waits on
// is read from its RAMFC (tgm_chan_wait_source), so the producer of a semaphore that is never released is the one blamed,
// never the waiter for having waited. The interrupt turns the timeout off in the PBDMA (as nvgpu's handler does), so it
// cannot fire again for that wait; the channel's RAMFC gets it back the next time it is idle and off the PBDMA
// (tgm_chan_ramfc_due). While the rule finds the GPU progressing, a deadline repeats the check (the rule's second look).
#include "gpu_internal.h"
#include "TegraGM20BAcquire.h"

#define ACQUIRE_CHIDS 512 /* GM20B's channels (CCSR_CHANNEL__SIZE_1) */
#define ACQUIRE_WORDS (ACQUIRE_CHIDS / 64)

struct acquire_record {
	u32 method0, data0;
	u64 ns;
};

static struct {
	void *work, *recheck;
	u64 pending
		[ACQUIRE_WORDS]; /* atomic: channels whose acquire timed out, set by the interrupt pass */
	struct acquire_record record[ACQUIRE_CHIDS];
	u32 storm; /* atomic: the interrupt came again with the timeout already off: masked */
} acq;

/* nvkm_runl_chan_get_chid returns holding spinlocks with interrupts off, and nvkm_kmap may sleep (the gk20a instobj's
 * L2 flush and invalidate take ltc->mutex), so RAMFC is mapped after nvkm_chan_put. The instance block is freed only
 * with the caller's channel object (chan->user), which a caller holding the channel, or tgm_live_lock with the channel on
 * the live list, keeps. */
static struct nvkm_gpuobj *chan_inst(u32 chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	struct nvkm_gpuobj *inst = NULL;
	unsigned long flags = 0;

	nvkm_runl_foreach(runl, tgm_gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (nchan) {
			inst = nchan->inst;
			nvkm_chan_put(&nchan, flags);
			break;
		}
	}
	return inst;
}

void tgm_ramfc_write(u32 chid, u32 offset, u32 value)
{
	struct nvkm_gpuobj *inst = chan_inst(chid);

	if (!inst)
		return;
	nvkm_kmap(inst);
	nvkm_wo32(inst, offset, value);
	nvkm_done(inst);
}

/* RAMFC SEMAPHOREA..D and SYNCPOINTA/B under one map (each map flushes the L2) */
static int ramfc_read_waits(u32 chid, u32 sem[4], u32 sync[2])
{
	struct nvkm_gpuobj *inst = chan_inst(chid);
	u32 i = 0;

	if (!inst)
		return -ENOENT;
	nvkm_kmap(inst);
	for (i = 0; i < 4; i++)
		sem[i] = nvkm_ro32(inst, TGM_RAMFC_SEMAPHOREA + 4 * i);
	sync[0] = nvkm_ro32(inst, TGM_RAMFC_SYNCPOINTA);
	sync[1] = nvkm_ro32(inst, TGM_RAMFC_SYNCPOINTB);
	nvkm_done(inst);
	return 0;
}

static u32 acquire_word(void)
{
	return tgm_acquire_word(tgm_acquire_ptimer_ns(TGM_ACQUIRE_NS));
}

/* Before the channel's first submission, or once it is idle and off the PBDMA: the host loads RAMFC with the channel. */
void tgm_acquire_arm(u32 chid)
{
	if (acq.work)
		tgm_ramfc_write(chid, TGM_RAMFC_ACQUIRE, acquire_word());
}

/* From recover.c's PBDMA interrupt handler (nvkm's interrupt pass, its spinlock held): the timeout goes off before the
 * interrupt is cleared, so the same acquire cannot raise it again. */
void tgm_acquire_intr(struct nvkm_device *device, int runq)
{
	const u32 word = nvkm_rd32(device, TGM_PBDMA_ACQUIRE(runq));
	const u32 chid = PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(runq)));

	nvkm_wr32(device, TGM_PBDMA_ACQUIRE(runq), word & ~TGM_ACQUIRE_TIMEOUT_EN);
	if (!(word & TGM_ACQUIRE_TIMEOUT_EN)) {
		nvkm_mask(device, PBDMA_INTR_EN_0(runq), TGM_PBDMA_INTR_ACQUIRE, 0);
		if (!__atomic_fetch_add(&acq.storm, 1, __ATOMIC_RELAXED))
			nvxnu_log(
				"tegra-gm20b: PBDMA%d raised ACQUIRE again for channel %u with its "
				"timeout off (ACQUIRE %08x): the interrupt is masked, waits on the "
				"hang "
				"checks\n",
				runq, chid, word);
		return;
	}
	if (chid >= ACQUIRE_CHIDS || !acq.work)
		return;
	acq.record[chid].method0 = nvkm_rd32(device, PBDMA_METHOD0(runq));
	acq.record[chid].data0 = nvkm_rd32(device, PBDMA_DATA0(runq));
	acq.record[chid].ns = nvxnu_nanotime();
	__atomic_fetch_or(&acq.pending[chid / 64], 1ull << (chid % 64), __ATOMIC_RELEASE);
	nvxnu_work_schedule(acq.work);
}

/* tgm_live_lock held: the channel whose fence word `va` is in `chan`'s address space, and that word */
static struct tgm_chan *fence_owner(struct tgm_chan *chan, u64 va, const volatile u32 **word)
{
	s64 off = tgm_acquire_offset(va, chan->vm->fences_va, TGM_FENCE_BYTES);
	struct tgm_chan *other = NULL;

	if (off < 0 && chan == tgm_present.chan)
		off = tgm_acquire_offset(va, tgm_present.fences_gpu, TGM_FENCE_BYTES);
	*word = NULL;
	list_for_each_entry(other, &tgm_live_chans, live)
	{
		if (off >= 0 ? (uintptr_t)other->fence_cpu - (uintptr_t)tgm_fence_pages.va ==
				       (u64)off
			     : other->vm == chan->vm && other->fence_gpu == va) {
			*word = other->fence_cpu;
			return other;
		}
	}
	if (off >= 0)
		*word = (const volatile u32 *)((const u8 *)tgm_fence_pages.va + (off & ~3ll));
	return NULL;
}

/* tgm_live_lock held. A semaphore acquire from `chan`'s RAMFC still blocked: on a live channel's fence (*source), on a
 * gone channel's fence word or the address space's bind epoch (VICTIM), or on an address of the channel's own pushes
 * (UNKNOWN, only once parked). NONE: words left from an acquire that passed. */
static enum tgm_wait_source semaphore_source(struct tgm_chan *chan,
					     const struct tgm_acquire_wait *w, int parked,
					     struct tgm_chan **source, char *what, unsigned int len)
{
	const volatile u32 *word = NULL;
	struct tgm_chan *owner = fence_owner(chan, w->va, &word);
	const volatile u32 *bind = __atomic_load_n(&chan->vm->bind_cpu, __ATOMIC_ACQUIRE);
	u32 at = 0;

	if (word) {
		at = READ_ONCE(*word);
		if (!tgm_acquire_blocked(w->operation, at, w->payload))
			return TGM_WAITS_NONE;
		scnprintf(what, len, "%s ch %d fence %u (at %u)", owner ? owner->owner : "a gone",
			  owner ? (int)owner->chid : -1, w->payload, at);
		*source = owner;
		return owner ? TGM_WAITS_CHANNEL : TGM_WAITS_VICTIM;
	}
	if (bind && w->va == READ_ONCE(chan->vm->bind_va)) {
		at = READ_ONCE(*bind);
		if (!tgm_acquire_blocked(w->operation, at, w->payload))
			return TGM_WAITS_NONE;
		scnprintf(what, len, "bind epoch %u (at %u)", w->payload, at);
		return TGM_WAITS_VICTIM;
	}
	if (!parked)
		return TGM_WAITS_NONE;
	scnprintf(what, len, "semaphore %010llx op %u payload %u (no fence the kernel knows)",
		  (unsigned long long)w->va, w->operation, w->payload);
	return TGM_WAITS_UNKNOWN;
}

/* CCSR says the PBDMA switched the channel out on an acquire, or PBDMA0 is on it at a semaphore acquire or a syncpoint
 * wait (a waiter it never left: its RAMFC is not current) */
static int chan_parked(u32 chid, u32 ccsr)
{
	struct nvkm_device *device = tgm_gpu.device;
	u32 method0 = 0;
	u32 data0 = 0;

	if (tgm_ccsr_acquiring(ccsr))
		return 1;
	if (PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(0))) != chid)
		return 0;
	method0 = nvkm_rd32(device, PBDMA_METHOD0(0));
	data0 = nvkm_rd32(device, PBDMA_DATA0(0));
	return tgm_pbdma_acquiring(method0, data0) ||
	       (PBDMA_METHOD_ADDR(method0) == TGM_HOST_SYNCPOINTB && !(data0 & 0x3u));
}

/* What the channel's push waits on: a live channel's fence (*source), something the kernel or a host1x engine releases
 * (VICTIM: never its fault), an acquire on an address only its own pushes name (UNKNOWN), or nothing (NONE). From RAMFC,
 * which holds the acquire once the PBDMA switched the channel out (CCSR acquire pending); else the order acquire
 * tgm_chan_order queued last. A channel parked on an acquire (CCSR, or an ACQUIRE interrupt since its last progress)
 * whose RAMFC is stale or names nothing the kernel can read is a victim too: only an acquire decoded on an address
 * outside every kernel-known word is its own. tgm_live_lock held. */
enum tgm_wait_source tgm_chan_wait_source(struct tgm_chan *chan, struct tgm_chan **source,
					  char *what, unsigned int len)
{
	const u32 ccsr = nvkm_rd32(tgm_gpu.device, TGM_CCSR_CHANNEL(chan->chid));
	const int parked = chan_parked(chan->chid, ccsr) ||
			   chan->acquire_fired_ns >
				   max(READ_ONCE(chan->progress_ns), READ_ONCE(chan->busy_ns));
	const struct tgm_order *o = &chan->waited;
	enum tgm_wait_source kind = TGM_WAITS_NONE;
	struct tgm_acquire_wait w = {0, 0, 0, 0, 0};
	char none[1];
	u32 sem[4];
	u32 sync[2];

	*source = NULL;
	if (!what || !len) {
		what = none;
		len = sizeof(none);
	}
	what[0] = 0;
	if (!ramfc_read_waits(chan->chid, sem, sync)) {
		w = tgm_acquire_decode(sem, sync[0], sync[1], parked);
		if (w.kind == TGM_ACQUIRE_SEMAPHORE)
			kind = semaphore_source(chan, &w, parked, source, what, len);
		if (kind == TGM_WAITS_NONE && parked) {
			w = tgm_acquire_decode((const u32[4]){0, 0, 0, 0}, sync[0], sync[1], 1);
			if (w.kind == TGM_ACQUIRE_SYNCPOINT) {
				scnprintf(what, len, "host1x syncpoint %u >= %u", w.syncpoint,
					  w.payload);
				return TGM_WAITS_VICTIM;
			}
		}
		if (kind != TGM_WAITS_NONE)
			return kind;
	}
	if (o->cpu && (s32)(READ_ONCE(*o->cpu) - o->seqno) < 0) {
		*source = live_by(o->id, 0);
		scnprintf(what, len, "order wait on ch %u fence %u (at %u)", o->chid, o->seqno,
			  READ_ONCE(*o->cpu));
		if (*source)
			return TGM_WAITS_CHANNEL;
	}
	if (parked) {
		scnprintf(what, len, "an acquire RAMFC does not name yet (CCSR %08x)", ccsr);
		return TGM_WAITS_UNREAD;
	}
	return TGM_WAITS_NONE;
}

/* tgm_live_lock held: a channel parked on an acquire (chan_parked) that has waited past its timeout with no interrupt
 * for it. Logged once a channel; the hang checks go on deciding it. */
void tgm_acquire_unfired(struct tgm_chan *chan, u64 now)
{
	const u64 since = max(READ_ONCE(chan->progress_ns), READ_ONCE(chan->busy_ns));

	if (chan->acquire_unfired || chan->acquire_due || chan->acquire_fired_ns >= since ||
	    now - since < TGM_ACQUIRE_NS + TGM_HANG_NS ||
	    !chan_parked(chan->chid, nvkm_rd32(tgm_gpu.device, TGM_CCSR_CHANNEL(chan->chid))))
		return;
	chan->acquire_unfired = 1;
	nvxnu_log("tegra-gm20b: %s (channel %u) parked on an acquire %llu ms with its %llu ms "
		  "acquire timeout armed and no ACQUIRE interrupt\n",
		  chan->owner, chan->chid, (now - since) / NSEC_PER_MSEC,
		  (unsigned long long)(TGM_ACQUIRE_NS / NSEC_PER_MSEC));
}

#if TGM_STATS
/* The ACQUIRE probe, under tgm_live_lock: the looks this window that found a channel parked on an acquire (a hang
 * check's candidates, the stats window's live channels) against the ACQUIRE interrupts that came. Boot 18 had two ~2 s
 * promise parks and no "semaphore acquire timed out" line. A park seen at one look and still there TGM_ACQUIRE_NS
 * later with no interrupt since is logged once a boot, with whether the PBDMA was timing it. A line, not a panic: the
 * lost-interrupt rule applies once a boot shows which it is. */
static struct {
	u32 parks, fired;
	int logged;
} probe;

static const char *const probe_where_names[] = {
	"switched out", "interrupt lost", "on PBDMA0 with its timeout off",
	"neither on PBDMA0 at an acquire nor switched out on one"};

void tgm_acquire_probe_look(struct tgm_chan *chan, u64 now)
{
	struct nvkm_device *device = tgm_gpu.device;
	const u32 done = tgm_chan_completed(chan);
	u32 ccsr = 0;
	u32 method0 = 0;
	u32 data0 = 0;
	u32 acquire = 0;
	u32 pbdma = 0;

	if (READ_ONCE(chan->error) || (s32)(done - chan->submitted) >= 0) {
		chan->acquire_probe_ns = 0;
		return;
	}
	ccsr = nvkm_rd32(device, TGM_CCSR_CHANNEL(chan->chid));
	if (!chan_parked(chan->chid, ccsr)) {
		chan->acquire_probe_ns = 0;
		return;
	}
	probe.parks++;
	if (!chan->acquire_probe_ns || chan->acquire_probe_done != done ||
	    chan->acquire_fired_ns >= chan->acquire_probe_ns) {
		chan->acquire_probe_ns = now;
		chan->acquire_probe_done = done;
		return;
	}
	if (probe.logged || now - chan->acquire_probe_ns < TGM_ACQUIRE_NS)
		return;
	probe.logged = 1;
	pbdma = PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(0)));
	method0 = nvkm_rd32(device, PBDMA_METHOD0(0));
	data0 = nvkm_rd32(device, PBDMA_DATA0(0));
	acquire = nvkm_rd32(device, TGM_PBDMA_ACQUIRE(0));
	nvxnu_log("tegra-gm20b: acquire probe: %s (channel %u) parked on an acquire "
		  "over %llu ms with no ACQUIRE interrupt: %s (CCSR %08x; PBDMA0 on "
		  "channel %u at method %08x data %08x, ACQUIRE_SWITCH %s, ACQUIRE %08x)\n",
		  chan->owner, chan->chid, (now - chan->acquire_probe_ns) / NSEC_PER_MSEC,
		  probe_where_names[tgm_acquire_probe_where(
			  chan->chid, ccsr, pbdma, tgm_pbdma_acquiring(method0, data0), acquire)],
		  ccsr, pbdma, method0, data0, data0 & (1u << 12) ? "on" : "off", acquire);
}

static const char *const probe_giveup_names[] = {
	"switched out", "on PBDMA0 at the acquire with its timeout armed",
	"on PBDMA0 at the acquire with its timeout off",
	"neither on PBDMA0 at an acquire nor switched out on one"};

/* A GPU reader still parked on a writer promise at its give-up (SwitchGpuPromiseGiveUp.h), which comes about
 * TGM_ACQUIRE_NS after the park, too soon for a look above to judge the timeout: where the PBDMA has it, and whether an
 * ACQUIRE interrupt came since the park, once a boot */
void tgm_acquire_probe_giveup(struct tgm_chan *chan, u64 parked_ns, u64 now)
{
	struct nvkm_device *device = tgm_gpu.device;
	static int logged;
	u32 ccsr = 0;
	u32 method0 = 0;
	u32 data0 = 0;
	u32 acquire = 0;
	u32 pbdma = 0;

	if (logged || READ_ONCE(chan->error))
		return;
	logged = 1;
	ccsr = nvkm_rd32(device, TGM_CCSR_CHANNEL(chan->chid));
	pbdma = PBDMA_CHID(nvkm_rd32(device, PBDMA_CHANNEL(0)));
	method0 = nvkm_rd32(device, PBDMA_METHOD0(0));
	data0 = nvkm_rd32(device, PBDMA_DATA0(0));
	acquire = nvkm_rd32(device, TGM_PBDMA_ACQUIRE(0));
	nvxnu_log("tegra-gm20b: acquire probe: %s (channel %u) still parked on a writer promise at "
		  "its give-up "
		  "%llu ms after the park, %s ACQUIRE interrupt since: %s (CCSR %08x; PBDMA0 on "
		  "channel %u at "
		  "method %08x data %08x, ACQUIRE_SWITCH %s, ACQUIRE %08x)\n",
		  chan->owner, chan->chid, (now - parked_ns) / NSEC_PER_MSEC,
		  chan->acquire_fired_ns >= parked_ns ? "an" : "no",
		  probe_giveup_names[tgm_acquire_probe_where(
			  chan->chid, ccsr, pbdma, tgm_pbdma_acquiring(method0, data0), acquire)],
		  ccsr, pbdma, method0, data0, data0 & (1u << 12) ? "on" : "off", acquire);
}

void tgm_acquire_probe_window(char *line, unsigned int len, u64 now)
{
	struct tgm_chan *chan = NULL;

	line[0] = 0;
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		tgm_acquire_probe_look(chan, now);
	}
	if (probe.parks || probe.fired)
		scnprintf(
			line, len,
			"%u looks found a channel parked on an acquire, %u ACQUIRE interrupts came",
			probe.parks, probe.fired);
	probe.parks = 0;
	probe.fired = 0;
	mutex_unlock(&tgm_live_lock);
}

#endif
/* tgm_live_lock held: the rule for a channel parked on an acquire past its timeout. While it waits on another channel and
 * the GPU progresses, a check is due again (the rule's second look at the producer); a wait on what the kernel or a
 * host1x engine releases is left to that releaser, with no check repeating. Returns the verdict; the caller panics on
 * TGM_HANG_STUCK. */
static enum tgm_hang acquire_check(struct tgm_chan *chan, enum tgm_wait_source kind, u64 now)
{
	const u32 done = tgm_chan_completed(chan);
	enum tgm_hang verdict = TGM_HANG_PROGRESSING;

	chan->acquire_check_ns = 0;
	if (READ_ONCE(chan->error) || chan->isolating || (s32)(done - chan->submitted) >= 0 ||
	    kind == TGM_WAITS_VICTIM)
		return verdict;
	if (kind == TGM_WAITS_UNREAD && chan->acquire_unread++)
		return verdict;
	verdict = tgm_hang_recover(chan, "semaphore acquire timeout", done + 1,
				   chan->submit_ns ? now - chan->submit_ns : 0);
	if (verdict == TGM_HANG_PROGRESSING &&
	    (kind == TGM_WAITS_CHANNEL || kind == TGM_WAITS_UNREAD)) {
		chan->acquire_check_ns = now;
		chan->acquire_done = done;
		nvxnu_work_schedule_after(acq.recheck, TGM_HANG_NS);
	}
	return verdict;
}

static void acquire_timed_out(u32 chid, const struct acquire_record *r)
{
	struct tgm_chan *chan = NULL;
	struct tgm_chan *source = NULL;
	enum tgm_wait_source kind = TGM_WAITS_NONE;
	char what[112];
	u64 now = nvxnu_nanotime();

	mutex_lock(&tgm_live_lock);
	chan = live_by(0, (int)chid);
	if (!chan || READ_ONCE(chan->error) || chan->isolating) {
		mutex_unlock(&tgm_live_lock);
		return;
	}
	WRITE_ONCE(chan->acquire_due, 1);
	chan->acquire_fired_ns = r->ns;
#if TGM_STATS
	probe.fired++;
#endif
	chan->acquire_unread = 0;
	kind = tgm_chan_wait_source(chan, &source, what, sizeof(what));
	if (PBDMA_METHOD_ADDR(r->method0) == TGM_HOST_SYNCPOINTB) {
		kind = TGM_WAITS_VICTIM;
		scnprintf(what, sizeof(what), "a host1x syncpoint (PBDMA at SYNCPOINTB)");
	}
	nvxnu_log("tegra-gm20b: semaphore acquire timed out on %s (channel %u) %llu ms after its "
		  "kick (timeout %llu ms), PBDMA at mthd %08x data %08x: waits on %s\n",
		  chan->owner, chid,
		  chan->submit_ns && r->ns > chan->submit_ns
			  ? (r->ns - chan->submit_ns) / NSEC_PER_MSEC
			  : 0ull,
		  (unsigned long long)(TGM_ACQUIRE_NS / NSEC_PER_MSEC), r->method0, r->data0,
		  what[0] ? what : "nothing RAMFC names yet");
	if (acquire_check(chan, kind, now) == TGM_HANG_STUCK)
		tgm_chan_hung(chan, "semaphore acquire timeout", tgm_chan_completed(chan) + 1,
			      chan->submit_ns ? now - chan->submit_ns : 0);
	mutex_unlock(&tgm_live_lock);
}

static void acquire_work(void *arg)
{
	struct acquire_record r;
	u64 bits = 0;
	u32 i = 0;
	u32 chid = 0;

	(void)arg;
	for (i = 0; i < ACQUIRE_WORDS; i++) {
		for (bits = __atomic_exchange_n(&acq.pending[i], 0, __ATOMIC_ACQUIRE); bits;
		     bits &= bits - 1) {
			chid = i * 64 + (u32)__builtin_ctzll(bits);
			r = acq.record[chid];
			acquire_timed_out(chid, &r);
		}
	}
}

static void acquire_recheck(void *arg)
{
	struct tgm_chan *chan = NULL;
	struct tgm_chan *source = NULL;
	u64 now = nvxnu_nanotime();
	u64 wait = 0;

	(void)arg;
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (!chan->acquire_check_ns)
			continue;
		if (tgm_chan_completed(chan) != chan->acquire_done) {
			chan->acquire_check_ns = 0;
		} else if (now - chan->acquire_check_ns + TGM_STUCK_NS < TGM_HANG_NS) {
			wait = TGM_HANG_NS - (now - chan->acquire_check_ns);
		} else if (acquire_check(chan, tgm_chan_wait_source(chan, &source, NULL, 0), now) ==
			   TGM_HANG_STUCK) {
			tgm_chan_hung(chan, "semaphore acquire timeout", chan->acquire_done + 1,
				      chan->submit_ns ? now - chan->submit_ns : 0);
		}
	}
	mutex_unlock(&tgm_live_lock);
	if (wait)
		nvxnu_work_schedule_after(acq.recheck, wait);
}

void tgm_acquire_init(void)
{
	if (!acq.work)
		acq.work = nvxnu_work_alloc(acquire_work, NULL);
	if (!acq.recheck)
		acq.recheck = nvxnu_work_alloc(acquire_recheck, NULL);
}

void tgm_acquire_suspend(void)
{
	u32 i = 0;

	nvxnu_work_cancel_wait(acq.work);
	nvxnu_work_cancel_wait(acq.recheck);
	for (i = 0; i < ACQUIRE_WORDS; i++)
		__atomic_store_n(&acq.pending[i], 0, __ATOMIC_RELAXED);
}
