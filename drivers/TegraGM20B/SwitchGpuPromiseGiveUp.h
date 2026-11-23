/* SPDX-License-Identifier: MIT */
/* The give-up of a writer promise nobody keeps (SwitchGpuWriterPromise.h): the one place a promise is given up
 * (switch_promise_giveup_site_test fails on a second).
 *
 * A reader parks on a pending promise word in one of three ways: a GPU job that acquires it, once kicked; a CPU reader
 * asleep behind it; the display's direct present watching it. A promise whose job is not submitted yet belongs to a
 * command buffer its maker's runtime holds on a shared event. The runtime fails that command buffer at Metal's bound
 * and withdraws the promise, and it hands the same bound to the kernel when the wait begins (method 34 Arm), because
 * Metal's bound is the kernel's: it fires whatever state the app is in, a suspended or stopped one included. A promise
 * past its holder's bound is given up whether or not its maker ran, and a keep that comes later is refused
 * (switch_gpu_giveup_unhold), so the command buffer fails with Metal's timeout and never half-runs. A promise nobody
 * armed (its holder stopped between the make and the arm, or a maker with no command buffer) is given up
 * TegraGM20BPromiseBoundNs after its first reader parked, the same bound, taken at the park so the make costs nothing;
 * a keep after that is accepted, and its job writes the surface for the frames after. A keep under way ends the bound
 * (keeping) until the job is kept or refused (switch_gpu_giveup_rehold), and a release ends it.
 *
 * A kept promise is never given up by time. Its job's end releases the word (its fence interrupt's pass finds it
 * moved); a job that does not end is the hang rule's (tgm_chan_check_progress: TGM_HANG_NS without progress, then the
 * channel's recovery or kill), and a kill's retirement releases the promise (AGXDeviceUserClient::releasePromises).
 * The call is that rule's waiter for the promised job and for a job ahead of it on its channel, so its only timer for a
 * kept promise is the rule's next look. While the GPU is powered down nothing is looked at: the power-down drained every
 * channel first (sleep_fences), and the power-up looks again.
 *
 * One call is armed for the earliest due: a promise's bound, or the hang rule's next look. It is cancelled when every
 * word waited on moved or the last reader left, and reads the word again whenever it runs, so a missed cancel costs a
 * run, never a give-up. A promise given up is marked abandoned, its word raised by a plain store with the fence page's
 * owner unchanged, and one line names its maker, its first reader and the time since that reader parked. A present of
 * the surface is then dropped and the panel keeps the frame before it. */
#pragma once
#include <stdint.h>
#include "SwitchGpuWriterPromise.h"

/* The hang rule's interval (TGM_HANG_NS; gpu_promise_giveup.c asserts they agree): a kept promise is first looked at
 * one interval after its keep, since the rule finds no stall sooner */
#define SWITCH_GPU_GIVEUP_LOOK_NS (2000ull * 1000 * 1000)
/* armed_ns while the direct present alone waits, on an unkept promise: the interrupt pass looks for its word to move
 * (a keep the present never heard of) and no call is scheduled */
#define SWITCH_GPU_GIVEUP_WATCHING 1ull
/* One record for every promise slot that can exist: each fence user page's TegraGM20BMaxPromises slots
 * (gpu_promise_giveup.c asserts both), so no promise is ever left without one */
#define SWITCH_GPU_GIVEUP_PAGES 64
#define SWITCH_GPU_GIVEUP_PER_PAGE 32
#define SWITCH_GPU_GIVEUPS (SWITCH_GPU_GIVEUP_PAGES * SWITCH_GPU_GIVEUP_PER_PAGE)
#define SWITCH_GPU_GIVEUP_USED_WORDS (SWITCH_GPU_GIVEUPS / 64)
/* GPU reader channels parked at once that a record tells apart; a promise read from more is given up at its holder's
 * bound all the same, the untold ones taken as still parked */
#define SWITCH_GPU_GIVEUP_READERS 4
/* Reader sets for the promises GPU jobs wait on, taken at the first GPU reader's park and given back when the record
 * is freed. A promise whose first GPU reader finds every set taken counts its readers as parked until its give-up. */
#define SWITCH_GPU_GIVEUP_READER_SETS 64

/* A GPU job: its channel's uid and the first seqno it releases (its acquires come before it) */
struct switch_gpu_giveup_job {
	uint32_t uid, first;
};

/* GPU reader channels, each with its latest reader job */
struct switch_gpu_giveup_readers {
	uint32_t count;
	struct switch_gpu_giveup_job job[SWITCH_GPU_GIVEUP_READERS];
};

/* One per promise slot, its fields widest first so none pads */
struct switch_gpu_giveup {
	/* deferred: the promised job's channel fence and the seqno of the job ahead of it */
	const volatile uint32_t *ahead;
	uint64_t parked_ns, due_ns;
	const char *who;
	uint32_t offset; /* 0: free (no promise word is at offset 0) */
	uint32_t value, owner;
	uint32_t waiters; /* CPU readers and the direct present: each leaves itself */
	struct switch_gpu_giveup_job job;
	uint32_t job_last, ahead_seqno;
	uint16_t readers; /* 1 + its reader set, 0: none */
	/* a GPU reader beyond what a reader set tells apart: taken as parked until the give-up */
	uint8_t overflow;
	uint8_t kept;    /* job is the promised job */
	uint8_t display; /* the direct present is a waiter */
	char waiter[18];
};

/* The job that keeps a slot's promise of `value` on its page's `owner`, remembered at keptPromises for a reader that
 * parks after it */
struct switch_gpu_giveup_kept {
	uint32_t value, owner, last;
	struct switch_gpu_giveup_job job;
};

/* The bound of a slot's promise of `value` on its page's `owner`. `armed`: its holder's (method 34 Arm), at due_ns on
 * the kernel's clock, `fails` when its command buffer fails there (a keep after it is refused); not armed, the default
 * from its first reader's park. `keeping` from a keep or a release, which end the bound, until a refused job's
 * rehold; given_up once the call took the promise past its bound. */
struct switch_gpu_giveup_held {
	uint64_t due_ns;
	uint32_t value, owner;
	uint8_t armed, fails, keeping, given_up;
};

/* entry[i], kept[i] and held[i] are promise slot i's (switch_gpu_giveup_ops.slot) */
struct switch_gpu_giveups {
	struct switch_gpu_giveup entry[SWITCH_GPU_GIVEUPS];
	/* tgm_live_lock, never the interrupt pass's: all zero is every set free */
	struct switch_gpu_giveup_readers reader_set[SWITCH_GPU_GIVEUP_READER_SETS];
	uint64_t reader_sets_taken[SWITCH_GPU_GIVEUP_READER_SETS / 64];
	/* under the call's simple lock */
	struct switch_gpu_giveup_kept kept[SWITCH_GPU_GIVEUPS];
	struct switch_gpu_giveup_held held[SWITCH_GPU_GIVEUPS];
	/* entries in use, read by the pass with plain loads */
	uint64_t used[SWITCH_GPU_GIVEUP_USED_WORDS];
	/* when the call is due; 0: not armed (written under the call's simple lock) */
	uint64_t armed_ns;
};

/* Where the promised job is (switch_gpu_giveup_ops.job_ahead) */
enum {
	SWITCH_GPU_GIVEUP_HEAD = 0,   /* everything before it on its channel is complete */
	SWITCH_GPU_GIVEUP_BEHIND = 1, /* a job ahead of it is not */
	SWITCH_GPU_GIVEUP_GONE = 2,   /* its channel died */
	SWITCH_GPU_GIVEUP_DOWN =
		3, /* the GPU is powered down: nothing is looked at until the power-up */
};

/* The kernel under the table's lock (tgm_live_lock) */
struct switch_gpu_giveup_ops {
	const struct switch_gpu_promise_fence *fence;
	/* the promise slot whose word is at `offset`, -1 for any other word */
	int (*slot)(uint32_t offset);
	/* the reader's first seqno not complete on a live channel: its acquire has not passed */
	int (*reader_parked)(const struct switch_gpu_giveup_job *reader);
	/* BEHIND or HEAD: *seqno is the job waited for (the job ahead, else the promised job's last, `last`), whose interrupt is
	 * asked for, *fence its channel's fence (NULL once it completed) and *check_ns the channel's next look by the hang
	 * rule, this caller its waiter */
	int (*job_ahead)(const struct switch_gpu_giveup_job *job, uint32_t last,
			 const volatile uint32_t **fence, uint32_t *seqno, uint64_t *check_ns);
	/* a channel asked for the interrupt at `seqno`: a quiet job raises none unless asked */
	void (*need)(uint32_t uid, uint32_t seqno);
	/* each step of a give-up (SWITCH_GPU_PROMISE_RAISING, then RAISED or KEPT_FIRST) */
	void (*stepped)(uint32_t offset, uint32_t value, int step, void *entry);
	/* the word moved while the direct present still waited on it: a job it never heard of kept it */
	void (*moved)(uint32_t offset, uint32_t value);
	/* the promised job keptPromises remembered while the call ran, taken into an entry that has none */
	void (*recall)(struct switch_gpu_giveup *e);
	/* the promised job's channel was killed or destroyed */
	int (*killed)(const struct switch_gpu_giveup_job *job);
	/* an unkept entry due at `now`: 1 when its bound passed and no keep came first (it is taken as given up); 0, its due
	 * set again (0 while a keep is under way) */
	int (*expire)(struct switch_gpu_giveup *e, uint64_t now);
};

/* The call's arming, under its simple lock */
struct switch_gpu_giveup_call {
	void (*after)(uint64_t ns);
	void (*cancel)(void);
};

static inline int switch_gpu_giveup_pending(const struct switch_gpu_promise_fence *fence,
					    uint32_t offset, uint32_t value, uint32_t owner)
{
	const volatile uint32_t *at = fence->word(offset);

	return at && fence->owner(offset) == owner &&
	       (int32_t)(__atomic_load_n(at, __ATOMIC_ACQUIRE) - value) < 0;
}

static inline void switch_gpu_giveup_use(struct switch_gpu_giveups *g, int slot, int used)
{
	const uint64_t bit = 1ull << (slot % 64);
	uint64_t *word = &g->used[slot / 64];

	__atomic_store_n(word, used ? *word | bit : *word & ~bit, __ATOMIC_RELEASE);
}

/* The entry's reader set, NULL while it has none */
static inline struct switch_gpu_giveup_readers *
switch_gpu_giveup_readers_of(struct switch_gpu_giveups *g, const struct switch_gpu_giveup *e)
{
	return e->readers ? &g->reader_set[e->readers - 1] : 0;
}

/* A free reader set, emptied, as 1 + its index; 0 when every one is taken */
static inline uint16_t switch_gpu_giveup_reader_set_take(struct switch_gpu_giveups *g)
{
	for (int w = 0; w < SWITCH_GPU_GIVEUP_READER_SETS / 64; ++w) {
		const uint64_t free = ~g->reader_sets_taken[w];
		int at = 0;

		if (!free)
			continue;
		at = w * 64 + __builtin_ctzll(free);
		g->reader_sets_taken[w] |= 1ull << (at % 64);
		g->reader_set[at].count = 0;
		return (uint16_t)(at + 1);
	}
	return 0;
}

/* The one place an entry is freed, and its reader set given back */
static inline void switch_gpu_giveup_free(struct switch_gpu_giveups *g, struct switch_gpu_giveup *e)
{
	if (e->readers) {
		g->reader_sets_taken[(e->readers - 1) / 64] &= ~(1ull << ((e->readers - 1) % 64));
		e->readers = 0;
	}
	__atomic_store_n(&e->offset, 0u, __ATOMIC_RELEASE);
	__atomic_store_n(&e->ahead, (const volatile uint32_t *)0, __ATOMIC_RELAXED);
	switch_gpu_giveup_use(g, (int)(e - g->entry), 0);
}

/* The next entry in use from slot `from`, or SWITCH_GPU_GIVEUPS */
static inline int switch_gpu_giveup_next_used(const struct switch_gpu_giveups *g, int from)
{
	for (int w = from / 64; w < SWITCH_GPU_GIVEUP_USED_WORDS; ++w) {
		uint64_t bits = __atomic_load_n(&g->used[w], __ATOMIC_ACQUIRE);

		if (w == from / 64)
			bits &= ~0ull << (from % 64);
		if (bits)
			return w * 64 + __builtin_ctzll(bits);
	}
	return SWITCH_GPU_GIVEUPS;
}

#define switch_gpu_giveup_each(g, i)                                              \
	for (int i = switch_gpu_giveup_next_used((g), 0); i < SWITCH_GPU_GIVEUPS; \
	     i = switch_gpu_giveup_next_used((g), i + 1))

/* An entry whose word moved: the direct present, when it still waits on it, hears that it did */
static inline void switch_gpu_giveup_drop_moved(struct switch_gpu_giveups *g,
						const struct switch_gpu_giveup_ops *ops,
						struct switch_gpu_giveup *e)
{
	if (e->display)
		ops->moved(e->offset, e->value);
	switch_gpu_giveup_free(g, e);
}

/* GPU readers whose acquire passed, or whose channel died, are dropped; how many are left */
static inline uint32_t switch_gpu_giveup_prune(struct switch_gpu_giveups *g,
					       const struct switch_gpu_giveup_ops *ops,
					       const struct switch_gpu_giveup *e)
{
	struct switch_gpu_giveup_readers *r = switch_gpu_giveup_readers_of(g, e);
	uint32_t kept = 0;

	if (!r)
		return 0;
	for (uint32_t i = 0; i < r->count; ++i)
		if (ops->reader_parked(&r->job[i]))
			r->job[kept++] = r->job[i];
	r->count = kept;
	return kept;
}

static inline void switch_gpu_giveup_add(struct switch_gpu_giveups *g,
					 const struct switch_gpu_giveup_ops *ops,
					 struct switch_gpu_giveup *e,
					 const struct switch_gpu_giveup_job *reader, int display)
{
	struct switch_gpu_giveup_readers *r = 0;

	if (!reader) {
		e->waiters++;
		e->display |= (uint8_t)(display != 0);
		return;
	}
	if (!e->readers)
		e->readers = switch_gpu_giveup_reader_set_take(g);
	r = switch_gpu_giveup_readers_of(g, e);
	if (!r) {
		e->overflow = 1;
		return;
	}
	/* a channel runs its jobs in order: while its latest reader has not passed its acquire, it waits */
	for (uint32_t i = 0; i < r->count; ++i) {
		if (r->job[i].uid != reader->uid)
			continue;
		if ((int32_t)(reader->first - r->job[i].first) > 0)
			r->job[i].first = reader->first;
		return;
	}
	if (r->count == SWITCH_GPU_GIVEUP_READERS)
		switch_gpu_giveup_prune(g, ops, e);
	if (r->count == SWITCH_GPU_GIVEUP_READERS)
		e->overflow = 1;
	else
		r->job[r->count++] = *reader;
}

/* The call's simple lock held: a kept promise remembered on its slot for a reader that parks on it after keptPromises
 * ran */
static inline void switch_gpu_giveup_remember(struct switch_gpu_giveups *g, int slot,
					      uint32_t value, uint32_t owner,
					      const struct switch_gpu_giveup_job *job,
					      uint32_t last)
{
	struct switch_gpu_giveup_kept *k = &g->kept[slot];

	k->value = value;
	k->owner = owner;
	k->last = last;
	k->job = *job;
}

/* The call's simple lock held: the promised job remembered for the entry's promise, taken into it; 1 if found */
static inline int switch_gpu_giveup_recall(const struct switch_gpu_giveups *g,
					   struct switch_gpu_giveup *e)
{
	const struct switch_gpu_giveup_kept *k = &g->kept[e - g->entry];

	if (!k->job.uid || k->value != e->value || k->owner != e->owner)
		return 0;
	e->kept = 1;
	e->job = k->job;
	e->job_last = k->last;
	return 1;
}

/* A reader parks on the promise at `now`: a GPU job (reader), or a waiter that leaves itself. Its caller gives a new
 * entry its due: the promise's bound (switch_gpu_giveup_bound), or the hang rule's look once kept. The entry, or -1 when
 * there is nothing to wait for (no promise slot's word, the word moved, or its page has another owner). A record an
 * earlier promise of the slot left is dropped: its word moved or its page changed hands, or the slot could not have been
 * made again. */
static inline int switch_gpu_giveup_park(struct switch_gpu_giveups *g,
					 const struct switch_gpu_giveup_ops *ops, uint32_t offset,
					 uint32_t value, const char *waiter, const char *who,
					 const struct switch_gpu_giveup_job *reader, int display,
					 uint64_t now)
{
	const uint32_t owner = ops->fence->owner(offset);
	const int slot = ops->slot(offset);
	struct switch_gpu_giveup *e = slot < 0 ? 0 : &g->entry[slot];

	if (!e || !switch_gpu_giveup_pending(ops->fence, offset, value, owner))
		return -1;
	if (e->offset && (e->value != value || e->owner != owner))
		switch_gpu_giveup_drop_moved(g, ops, e);
	if (e->offset) {
		switch_gpu_giveup_add(g, ops, e, reader, display);
		return slot;
	}
	__builtin_memset(e, 0, sizeof(*e));
	e->value = value;
	e->owner = owner;
	e->parked_ns = now;
	e->who = who;
	for (uint32_t i = 0; waiter && i < sizeof(e->waiter) - 1 && waiter[i]; ++i)
		e->waiter[i] = waiter[i];
	switch_gpu_giveup_add(g, ops, e, reader, display);
	__atomic_store_n(&e->offset, offset, __ATOMIC_RELEASE);
	switch_gpu_giveup_use(g, slot, 1);
	return slot;
}

/* A waiter that parked without a GPU job goes on. The entry is dropped once no reader is left. */
static inline void switch_gpu_giveup_leave(struct switch_gpu_giveups *g,
					   const struct switch_gpu_giveup_ops *ops, uint32_t offset,
					   uint32_t value, int display)
{
	const int slot = ops->slot(offset);
	struct switch_gpu_giveup *e = slot < 0 ? 0 : &g->entry[slot];

	if (!e || e->offset != offset || e->value != value || !e->waiters)
		return;
	e->waiters--;
	if (display)
		e->display = 0;
	if (!e->waiters && !switch_gpu_giveup_prune(g, ops, e) && !e->overflow)
		switch_gpu_giveup_free(g, e);
}

/* A kept promise's entry is due for the hang rule's first look from `now`, once */
static inline void switch_gpu_giveup_due(struct switch_gpu_giveup *e, uint64_t now)
{
	if (e->kept && !e->due_ns)
		e->due_ns = now + SWITCH_GPU_GIVEUP_LOOK_NS;
}

/* The call's simple lock held: slot `slot`'s record of its promise of `value` on `owner`'s page, made afresh (no bound
 * armed, no keep) where it held an earlier promise's */
static inline struct switch_gpu_giveup_held *
switch_gpu_giveup_held_for(struct switch_gpu_giveups *g, int slot, uint32_t value, uint32_t owner)
{
	struct switch_gpu_giveup_held *h = &g->held[slot];

	if (h->value != value || h->owner != owner) {
		__builtin_memset(h, 0, sizeof(*h));
		h->value = value;
		h->owner = owner;
	}
	return h;
}

/* The call's simple lock held: the holder's bound on slot `slot`'s promise of `value` on `owner`'s page, due at
 * `due_ns`. An arm again (a later wait of the same command buffer) replaces it, and it replaces the default. */
static inline void switch_gpu_giveup_hold(struct switch_gpu_giveups *g, int slot, uint32_t value,
					  uint32_t owner, uint64_t due_ns, int fails)
{
	struct switch_gpu_giveup_held *h = switch_gpu_giveup_held_for(g, slot, value, owner);

	h->due_ns = due_ns;
	h->armed = 1;
	h->fails = fails != 0;
	h->keeping = 0;
	h->given_up = 0;
}

/* The call's simple lock held: when the entry's unkept promise is given up: its holder's bound, else `fallback` (its
 * first reader's park and the default bound); 0 while a keep is under way */
static inline uint64_t switch_gpu_giveup_bound(const struct switch_gpu_giveups *g,
					       const struct switch_gpu_giveup *e, uint64_t fallback)
{
	const struct switch_gpu_giveup_held *h = &g->held[e - g->entry];

	if (h->value != e->value || h->owner != e->owner)
		return fallback;
	if (h->keeping)
		return 0;
	return h->armed ? h->due_ns : fallback;
}

/* The call's simple lock held: a keep of slot `slot`'s promise of `value` on `owner`'s page at `now`, or its release,
 * ends its bound. 1 when a bound its command buffer fails at passed first: the promise was given up, or is due to be,
 * and a keep of it is refused. */
static inline int switch_gpu_giveup_unhold(struct switch_gpu_giveups *g, int slot, uint32_t value,
					   uint32_t owner, uint64_t now)
{
	struct switch_gpu_giveup_held *h = switch_gpu_giveup_held_for(g, slot, value, owner);

	h->keeping = 1;
	return h->armed && h->fails && (h->given_up || (int64_t)(now - h->due_ns) >= 0);
}

/* The call's simple lock held: the job that was to keep slot `slot`'s promise of `value` on `owner`'s page was refused,
 * so the promise is unkept again and its bound runs again. 1 when a keep was under way. */
static inline int switch_gpu_giveup_rehold(struct switch_gpu_giveups *g, int slot, uint32_t value,
					   uint32_t owner)
{
	struct switch_gpu_giveup_held *h = &g->held[slot];

	if (h->value != value || h->owner != owner || !h->keeping)
		return 0;
	h->keeping = 0;
	return 1;
}

/* The call's simple lock held: the call takes the entry's unkept promise at its due `now`. 1: given up, past its
 * holder's bound or the default one; 0: it waits on, its due set again (0 while a keep is under way). */
static inline int switch_gpu_giveup_held_expire(struct switch_gpu_giveups *g,
						struct switch_gpu_giveup *e, uint64_t now)
{
	struct switch_gpu_giveup_held *h =
		switch_gpu_giveup_held_for(g, (int)(e - g->entry), e->value, e->owner);

	if (h->keeping) {
		e->due_ns = 0;
		return 0;
	}
	if (h->armed && (int64_t)(now - h->due_ns) < 0) {
		e->due_ns = h->due_ns;
		return 0;
	}
	h->given_up = 1;
	return 1;
}

/* Whether promise slot `slot` has an entry: plain loads, the slot's own bit */
static inline int switch_gpu_giveup_in_use(const struct switch_gpu_giveups *g, int slot)
{
	return (__atomic_load_n(&g->used[slot / 64], __ATOMIC_ACQUIRE) >> (slot % 64)) & 1;
}

/* The promise's job is submitted at `now` (remembered first): an entry judges it from now on, its bound replaced by the
 * hang rule's first look, and its end is asked for, so the interrupt pass after it finds the word moved and cancels the
 * call. */
static inline void switch_gpu_giveup_kept(struct switch_gpu_giveups *g,
					  const struct switch_gpu_giveup_ops *ops, uint32_t offset,
					  uint32_t value, const struct switch_gpu_giveup_job *job,
					  uint32_t last, uint64_t now)
{
	const int slot = ops->slot(offset);
	struct switch_gpu_giveup *e = slot < 0 ? 0 : &g->entry[slot];

	if (!e || e->offset != offset || e->value != value || e->kept)
		return;
	e->kept = 1;
	e->job = *job;
	e->job_last = last;
	e->due_ns = now + SWITCH_GPU_GIVEUP_LOOK_NS;
	ops->need(job->uid, last);
}

/* Whether a reader is still parked */
static inline int switch_gpu_giveup_waited(struct switch_gpu_giveups *g,
					   const struct switch_gpu_giveup_ops *ops,
					   const struct switch_gpu_giveup *e)
{
	return switch_gpu_giveup_prune(g, ops, e) || e->waiters || e->overflow;
}

/* The earliest due among the kept entries still waited on, entries whose word moved dropped; 0: none */
static inline uint64_t switch_gpu_giveup_next(struct switch_gpu_giveups *g,
					      const struct switch_gpu_giveup_ops *ops)
{
	uint64_t next = 0;

	switch_gpu_giveup_each(g, i)
	{
		struct switch_gpu_giveup *e = &g->entry[i];

		if (!switch_gpu_giveup_pending(ops->fence, e->offset, e->value, e->owner)) {
			switch_gpu_giveup_drop_moved(g, ops, e);
			continue;
		}
		if (e->due_ns && (!next || (int64_t)(e->due_ns - next) < 0))
			next = e->due_ns;
	}
	return next;
}

/* Whether the direct present waits on an unkept promise's word */
static inline int switch_gpu_giveup_watched(const struct switch_gpu_giveups *g)
{
	switch_gpu_giveup_each(g, i)
	{
		if (g->entry[i].display && !g->entry[i].due_ns)
			return 1;
	}
	return 0;
}

static inline void switch_gpu_giveup_defer(struct switch_gpu_giveup *e,
					   const volatile uint32_t *fence, uint32_t seqno,
					   uint64_t check_ns, uint64_t now)
{
	__atomic_store_n(&e->ahead_seqno, seqno, __ATOMIC_RELAXED);
	__atomic_store_n(&e->ahead, fence, __ATOMIC_RELEASE);
	e->due_ns = now + check_ns;
}

/* The GPU powered up again at `now` (system sleep's wake, rail-up): every kept promise still waited on is looked at
 * again by the hang rule's interval. A holder's bound runs on through a power-down. */
static inline void switch_gpu_giveup_resume(struct switch_gpu_giveups *g, uint64_t now)
{
	switch_gpu_giveup_each(g, i)
	{
		struct switch_gpu_giveup *e = &g->entry[i];

		if (!e->kept)
			continue;
		__atomic_store_n(&e->ahead, (const volatile uint32_t *)0, __ATOMIC_RELAXED);
		e->due_ns = now + SWITCH_GPU_GIVEUP_LOOK_NS;
	}
}

/* 1: the entry is to be given up at `now`; 0: it waits on, its due set again (0: until a keep or the power-up sets one) */
static inline int switch_gpu_giveup_judge(const struct switch_gpu_giveup_ops *ops,
					  struct switch_gpu_giveup *e, uint64_t now)
{
	const volatile uint32_t *fence = 0;
	uint32_t seqno = 0;
	uint64_t check = 0;
	int where = SWITCH_GPU_GIVEUP_HEAD;
	const volatile uint32_t *ahead = __atomic_load_n(&e->ahead, __ATOMIC_ACQUIRE);
	const int went_on =
		ahead && (int32_t)(__atomic_load_n(ahead, __ATOMIC_ACQUIRE) - e->ahead_seqno) >= 0;

	if (!went_on && (int64_t)(now - e->due_ns) < 0)
		return 0;
	if (!e->kept)
		return ops->expire(e, now);
	where = ops->job_ahead(&e->job, e->job_last, &fence, &seqno, &check);
	if (where == SWITCH_GPU_GIVEUP_GONE)
		return 1;
	if (where == SWITCH_GPU_GIVEUP_DOWN) {
		__atomic_store_n(&e->ahead, (const volatile uint32_t *)0, __ATOMIC_RELAXED);
		e->due_ns = 0;
		return 0;
	}
	switch_gpu_giveup_defer(e, fence, seqno, check, now);
	return 0;
}

/* The call runs at `now`: each entry still waited on is judged, and those due are given up. */
static inline void switch_gpu_giveup_run(struct switch_gpu_giveups *g,
					 const struct switch_gpu_giveup_ops *ops, uint64_t now)
{
	switch_gpu_giveup_each(g, i)
	{
		struct switch_gpu_giveup *e = &g->entry[i];

		if (!switch_gpu_giveup_pending(ops->fence, e->offset, e->value, e->owner)) {
			switch_gpu_giveup_drop_moved(g, ops, e);
			continue;
		}
		if (!e->kept)
			ops->recall(e);
		if (!switch_gpu_giveup_waited(g, ops, e)) {
			switch_gpu_giveup_free(g, e);
			continue;
		}
		switch_gpu_giveup_due(e, now);
		if (!e->due_ns || !switch_gpu_giveup_judge(ops, e, now))
			continue;
		if (!e->kept || !ops->killed(&e->job))
			switch_gpu_promise_give_up(ops->fence, e->offset, e->value, e->owner,
						   ops->stepped, e);
		switch_gpu_giveup_free(g, e);
	}
}

/* The call's simple lock held: armed for `next` (0: cancelled; SWITCH_GPU_GIVEUP_WATCHING: none scheduled, the pass
 * looking), from `now` */
static inline void switch_gpu_giveup_arm_locked(struct switch_gpu_giveups *g,
						const struct switch_gpu_giveup_call *call,
						uint64_t next, uint64_t now)
{
	if (next <= SWITCH_GPU_GIVEUP_WATCHING) {
		if (g->armed_ns > SWITCH_GPU_GIVEUP_WATCHING)
			call->cancel();
		__atomic_store_n(&g->armed_ns, next, __ATOMIC_RELEASE);
		return;
	}
	if (g->armed_ns == next)
		return;
	__atomic_store_n(&g->armed_ns, next, __ATOMIC_RELEASE);
	call->after((int64_t)(next - now) > 0 ? next - now : 0);
}

enum {
	SWITCH_GPU_GIVEUP_PASS_NONE = 0,
	SWITCH_GPU_GIVEUP_PASS_RUN =
		1, /* a deferred promise's job ahead completed, or a word the direct present waits on
					 * moved: the call runs now */
	SWITCH_GPU_GIVEUP_PASS_CANCEL = 2, /* every word waited on moved: the call is cancelled */
};

/* Whether the call is armed: what the end of every GPU interrupt pass looks at first, and all it does while nothing is
 * waited on */
static inline int switch_gpu_giveup_armed(const struct switch_gpu_giveups *g)
{
	return __atomic_load_n(&g->armed_ns, __ATOMIC_ACQUIRE) != 0;
}

/* The end of a GPU interrupt pass: plain loads, no lock, and one load while the call is not armed (nothing waited on,
 * or the call running, which looks at every entry itself). An entry being filled or dropped meanwhile is read again under
 * the call's lock before a cancel, and its writer arms the call under that lock after it. */
static inline int switch_gpu_giveup_pass(const struct switch_gpu_giveups *g,
					 const struct switch_gpu_promise_fence *fence)
{
	int waited = 0;

	if (!__atomic_load_n(&g->armed_ns, __ATOMIC_ACQUIRE))
		return SWITCH_GPU_GIVEUP_PASS_NONE;
	switch_gpu_giveup_each(g, i)
	{
		const struct switch_gpu_giveup *e = &g->entry[i];
		const uint32_t offset = __atomic_load_n(&e->offset, __ATOMIC_ACQUIRE);
		const volatile uint32_t *ahead = __atomic_load_n(&e->ahead, __ATOMIC_ACQUIRE);

		if (!offset)
			continue;
		if (!switch_gpu_giveup_pending(fence, offset,
					       __atomic_load_n(&e->value, __ATOMIC_RELAXED),
					       __atomic_load_n(&e->owner, __ATOMIC_RELAXED))) {
			if (__atomic_load_n(&e->display, __ATOMIC_RELAXED))
				return SWITCH_GPU_GIVEUP_PASS_RUN;
			continue;
		}
		if (ahead && (int32_t)(__atomic_load_n(ahead, __ATOMIC_ACQUIRE) -
				       __atomic_load_n(&e->ahead_seqno, __ATOMIC_RELAXED)) >= 0)
			return SWITCH_GPU_GIVEUP_PASS_RUN;
		waited = 1;
	}
	return waited ? SWITCH_GPU_GIVEUP_PASS_NONE : SWITCH_GPU_GIVEUP_PASS_CANCEL;
}
