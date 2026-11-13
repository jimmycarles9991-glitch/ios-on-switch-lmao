/* SPDX-License-Identifier: MIT */
/* The push profile's slowest submission of a window, push by push in order (gpu_fenced.h "push profile"): each push's class,
 * its pass's colour target 0 in thousands of samples, its draws and interlock enables, and its GPU time. With the jobs
 * line's batches it attributes a frame's GPU time pass by pass. */
#ifndef SWITCH_GPU_PROFILE_SEQUENCE_H
#define SWITCH_GPU_PROFILE_SEQUENCE_H
#include <stdint.h>
#include "TegraGM20BGpu.h"

#define SWITCH_GPU_PROF_SEQ 64

struct switch_gpu_prof_push {
	uint8_t cls, ilk;
	uint16_t draws, kpx;
	uint32_t us;
};

/* The interrupt pass offers, the window's line takes; `gen` is odd while a slower one is copied in. */
struct switch_gpu_prof_seq {
	uint32_t gen, first, last, n, pushes, behind; /* behind: its kick found an app on GR */
	uint64_t ns;
	struct switch_gpu_prof_push push[SWITCH_GPU_PROF_SEQ];
};

static inline void switch_gpu_prof_seq_offer(struct switch_gpu_prof_seq *slow,
					     const struct switch_gpu_prof_seq *run)
{
	const uint32_t n = run->n < SWITCH_GPU_PROF_SEQ ? run->n : SWITCH_GPU_PROF_SEQ;

	if (run->ns <= __atomic_load_n(&slow->ns, __ATOMIC_RELAXED))
		return;
	__atomic_fetch_add(&slow->gen, 1, __ATOMIC_ACQ_REL);
	slow->first = run->first;
	slow->last = run->last;
	slow->n = n;
	slow->pushes = run->pushes;
	slow->behind = run->behind;
	for (uint32_t i = 0; i < n; i++)
		slow->push[i] = run->push[i];
	__atomic_store_n(&slow->ns, run->ns, __ATOMIC_RELAXED);
	__atomic_fetch_add(&slow->gen, 1, __ATOMIC_RELEASE);
}

/* Takes the window's slowest into `out` (out->n 0: none, or one was being copied in) and starts the next window. */
static inline void switch_gpu_prof_seq_take(struct switch_gpu_prof_seq *slow,
					    struct switch_gpu_prof_seq *out)
{
	const uint32_t gen = __atomic_load_n(&slow->gen, __ATOMIC_ACQUIRE);

	out->first = slow->first;
	out->last = slow->last;
	out->n = slow->n < SWITCH_GPU_PROF_SEQ ? slow->n : SWITCH_GPU_PROF_SEQ;
	out->pushes = slow->pushes;
	out->behind = slow->behind;
	for (uint32_t i = 0; i < out->n; i++)
		out->push[i] = slow->push[i];
	out->ns = __atomic_exchange_n(&slow->ns, 0, __ATOMIC_RELAXED);
	if ((gen & 1) || __atomic_load_n(&slow->gen, __ATOMIC_ACQUIRE) != gen || !out->ns)
		out->n = 0;
}

/* Whether a profiled job stamps after push `i` of `count`. A stamp waits for idle, so a push that may continue into the
 * next (incomplete) or that the scan could not read (TGM_PC_SKIP) gets none, and the compositor's pushes get one only
 * where its job already waits for idle (a batch's end, or its last push before the fence): a profiled compositor frame
 * drains no more often than any other. An unstamped push's time is counted with the next stamped one. */
static inline int switch_gpu_prof_stamped(int compositor, const struct tgm_push *push, uint32_t i,
					  uint32_t count, uint8_t cls)
{
	return !push->incomplete && cls != TGM_PC_SKIP &&
	       (!compositor || push->fence || i + 1 == count);
}

/* An owner's stamped jobs in a window, its spans in ns: how many were kicked with their stamps and how many stamps; the
 * profiled ones read back and their spans (first start to last end); and every job span the window counted, the
 * profiled ones among them. */
struct switch_gpu_prof_cost {
	uint32_t stamped, stamps, profiled, jobs;
	uint64_t profiled_ns, jobs_ns;
};

/* " N stamped (S stamps): A us each vs B us unstamped (+D us)": what the stamps' waits added, as the profiled jobs' mean
 * span less the mean of the owner's other jobs in the window (one estimate over different jobs, not the same job timed
 * twice). Returns 0 with nothing stamped. */
static inline int switch_gpu_prof_cost_format(char *buf, unsigned long len,
					      const struct switch_gpu_prof_cost *c)
{
	uint64_t with = 0, without = 0;

	if (!c->stamped)
		return 0;
	if (!c->profiled || c->jobs <= c->profiled || c->jobs_ns < c->profiled_ns)
		return snprintf(buf, len, " %u stamped (%u stamps), no span to compare", c->stamped,
				c->stamps);
	with = c->profiled_ns / c->profiled / 1000;
	without = (c->jobs_ns - c->profiled_ns) / (c->jobs - c->profiled) / 1000;
	return snprintf(buf, len,
			" %u stamped (%u stamps): %llu us each vs %llu us unstamped (%+lld us)",
			c->stamped, c->stamps, (unsigned long long)with,
			(unsigned long long)without, (long long)with - (long long)without);
}

/* A push: D draw, C clear, P pass, S sync, X compute, Y copy, 2 2D engine, T state; then for draws and clears its
 * target's thousands of samples (a multisampled target counts its sample grid: 720x1280 at 4x is 3686k), p if
 * pitch-linear, m if multisampled, /draws, i and the interlock enables (conflict detection on); then its microseconds. */
static inline int switch_gpu_prof_push_format(char *buf, unsigned long len,
					      const struct switch_gpu_prof_push *p)
{
	static const char other[] = "PSXY2T";

	if (p->cls >= TGM_PC_PASS)
		return snprintf(buf, len, " %c %u",
				p->cls - TGM_PC_PASS < 6 ? other[p->cls - TGM_PC_PASS] : '?',
				p->us);
	return snprintf(buf, len, " %c%uk%s%s/%u%s%.0u %u", p->cls < TGM_PC_CLEAR ? 'D' : 'C',
			p->kpx, p->cls & 2 ? "p" : "", p->cls & 1 ? "m" : "", p->draws,
			p->ilk ? "i" : "", p->ilk, p->us);
}

static inline int switch_gpu_prof_seq_format(char *buf, unsigned long len, const char *owner,
					     const struct switch_gpu_prof_seq *seq)
{
	unsigned long at = 0;
	int put = 0;

	if (!seq->n)
		return 0;
	put = snprintf(buf, len, "%s seqno %u-%u, %llu us%s:", owner, seq->first, seq->last,
		       (unsigned long long)(seq->ns / 1000),
		       seq->behind ? " (kicked behind an app on GR)" : "");
	at = put > 0 ? (unsigned long)put : 0;
	for (uint32_t i = 0; i < seq->n && at < len; i++) {
		put = switch_gpu_prof_push_format(buf + at, len - at, &seq->push[i]);
		at += put > 0 ? (unsigned long)put : 0;
	}
	if (seq->pushes > seq->n && at < len)
		snprintf(buf + at, len - at, " +%u pushes", seq->pushes - seq->n);
	if (at >= len && len)
		buf[len - 1] = 0;
	return 1;
}

#endif
