// SPDX-License-Identifier: MIT
// See TegraGM20BViews.h. Every step runs under the surface entry's lock, before IOSurface's own lock or unlock trap:
//  - the first lock of a block-linear surface (any task) takes a scratch from the pool, has the copy engine clear it and
//    write the texels into it as rows behind the surface's GPU writers (semaphore acquires: no CPU wait for them), sleeps
//    on that copy's fence interrupt, and drops the CPU's stale lines of the scratch;
//  - a task's first lock then replaces its mapping of the surface by one of the scratch at the same address
//    (kIOMapOverwrite: every thread of the task sees rows; other tasks, the GPU and the display keep the surface's pages);
//  - a task's last unlock puts its mapping back;
//  - the last unlock (any task) after a lock that may have written cleans the scratch's lines and has the copy engine
//    write the rows back into the texels, behind the GPU's writers, before IOSurface's unlock moves the seed, then gives
//    the scratch back;
//  - a GPU submission that writes the surface while a write lock holds its rows (tgm_views_gpu_writes) first reads the
//    texels back as rows once more (base: what the lock found, the GPU's write not in yet), so that unlock lays only what
//    the CPU changed over what the GPU wrote (TegraGM20BViewMerge.h): a CPU lock does not hold the GPU back, as on
//    Apple's GPUs.
// A view whose user client died (its task exited holding the lock) is dropped at the next trap of the surface. While any
// lock is held the entry holds a reference of its own, so its surface stays block-linear and its scratch stays put.
#include <IOKit/IOLib.h>
#include <os/log.h>
#include <stdint.h>
#include <string.h>
#include "TegraGM20BRelayout.h"
#include "TegraGM20BViewMerge.h"
#include "TegraGM20BViews.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"
extern "C" void proc_selfname(char *buf, int size);
extern "C" int proc_selfpid(void);

namespace
{
void drain(tgm_coh *c, tgm_tiled &t);

constexpr uint32_t kLock = 2, kLockReadOnly = 0x1, kLockAvoidSync = 0x2;
#if TGM_STATS
constexpr uint32_t kBusyLocks =
	4;  // debug: more locks than this in a second name the surface (a CPU producer)
#endif

int findView(const tgm_tiled &t, const tgm_cpu_view &v)
{
	for (uint32_t i = 0; i < t.nviews; i++) {
		if (t.views[i].view.task == v.task && t.views[i].view.address == v.address)
			return static_cast<int>(i);
	}
	return -1;
}

void removeView(tgm_tiled &t, uint32_t i)
{
	tgm_cpu_view_hold(&t.views[i].view, 0);
	t.views[i] = t.views[--t.nviews];
}

// Locks held through user clients that died: their mappings died with their tasks
void dropDead(tgm_coh *c, tgm_tiled &t, bool &released)
{
	for (uint32_t i = 0; i < t.nviews;) {
		if (tgm_cpu_view_live(&t.views[i].view)) {
			i++;
			continue;
		}
		t.locks -= t.views[i].locks < t.locks ? t.views[i].locks : t.locks;
		removeView(t, i);
	}
	if (!t.locks && t.scratch) {
		drain(c, t);
		released = true;
	}
}

void noteBusy(tgm_tiled &t, uint32_t id)
{
#if TGM_STATS
	const uint64_t now = tgm_timestamp_ns();
	if (now - t.window_ns > 1000000000ULL) {
		t.window_ns = now;
		t.window_locks = 0;
	}
	if (++t.window_locks > kBusyLocks && !t.named) {
		t.named = 1;
		char name[32];
		proc_selfname(name, sizeof(name));
		os_log(OS_LOG_DEFAULT,
		       "tegra-gm20b: IOSurface %u is block-linear and CPU-locked more than %u "
		       "times a second (%s[%d]): "
		       "a lock after each GPU write moves its texels",
		       id, kBusyLocks, name, proc_selfpid());
	}
#else
	(void)t;
	(void)id;
#endif
}

void noteReused(const tgm_coh *c, bool same)
{
#if TGM_STATS
	static int announced;
	if (!same || __atomic_exchange_n(&announced, 1, __ATOMIC_RELAXED))
		return;
	char name[32];
	proc_selfname(name, sizeof(name));
	os_log(OS_LOG_DEFAULT,
	       "tegra-gm20b: IOSurface %u: a CPU lock reused the rows its last lock moved, no "
	       "texels moved (%s[%d])",
	       tgm_coh_surface_id(c), name, proc_selfpid());
#else
	(void)c;
	(void)same;
#endif
}

// A move the copy engine could not make is the CPU's (tgm_relayout_by_cpu cannot fail)
void move(tgm_coh *c, const tgm_tiled &t, const tgm_relayout_pages &rows, int op)
{
	const tgm_relayout_pages pages = tgm_coh_pages(c);
	tgm_relayout_writer writers[TGM_RELAYOUT_MAX_WAITS];
	const uint32_t n = tgm_coh_writers(c, writers, TGM_RELAYOUT_MAX_WAITS);
	if (const int ret =
		    tgm_relayout(&pages, &rows, &t.layout, t.bytes_per_row, op, writers, n)) {
		switch_refusal(
			"iosurface-layout", "ce-move-failed",
			"IOSurface %u: the copy engine did not move its texels for the CPU (%d): "
			"the CPU did",
			tgm_coh_surface_id(c), ret);
		tgm_coh_wait_visible(c);
		tgm_relayout_by_cpu(&pages, &rows, &t.layout, t.bytes_per_row, op, writers, n);
	}
}

// The rows the last read-only locks gave back, if no GPU submission may have written the texels since they were moved
// and the scratch still holds them (iconservicesagent reads each icon render back with three locks in a row): no move.
tgm_scratch *reuse(tgm_coh *c, tgm_tiled &t, uint64_t written, tgm_relayout_pages &rows, bool &same)
{
	same = false;
	tgm_scratch *kept = t.kept;
	t.kept = nullptr;
	int intact = 0;
	tgm_scratch *scratch = kept ? tgm_scratch_reclaim(kept, t.kept_handout, &intact) : nullptr;
	if (!scratch)
		return nullptr;
	tgm_scratch_pages(scratch, &rows);
	same = intact && t.kept_written == written;
	noteReused(c, same);
	return scratch;
}

// The first lock: the texels into a cleared scratch as rows, behind the GPU's writers. Nonzero: refused, with nothing
// done: kIOSurfaceLockAvoidSync while the GPU writes it, or no scratch to hold the rows (the CPU never sees texels).
int fill(tgm_coh *c, tgm_tiled &t, uint32_t options)
{
	tgm_relayout_writer writers[TGM_RELAYOUT_MAX_WAITS];
	if ((options & kLockAvoidSync) && tgm_coh_writers(c, writers, TGM_RELAYOUT_MAX_WAITS))
		return 1;
	const uint64_t written = tgm_coh_written(c);
	tgm_relayout_pages rows;
	bool same = false;
	tgm_scratch *scratch = reuse(c, t, written, rows, same);
	t.filled_written = written;
	if (same) {
		t.scratch = scratch;
		return 0;
	}
	if (!scratch)
		scratch = tgm_scratch_get(static_cast<uint64_t>(tgm_coh_pages(c).npages) << 12, 1,
					  &rows);
	if (!scratch) {
		switch_refusal("iosurface-layout", "no-scratch",
			       "IOSurface %u: no memory for a CPU lock's rows: the lock is refused",
			       tgm_coh_surface_id(c));
		return 1;
	}
	tgm_cpu_clean_pages(rows.pas, rows.npages);
	move(c, t, rows, TGM_RELAYOUT_TO_ROWS);
	tgm_cpu_clean_pages(rows.pas, rows.npages);
	t.scratch = scratch;
	return 0;
}

// The texels as the GPU left them, as rows, with every byte the CPU changed since base laid over them, back into the
// texels. False, with nothing moved back, when there is no memory for it.
bool mergeBack(tgm_coh *c, const tgm_tiled &t)
{
	const uint64_t bytes = static_cast<uint64_t>(tgm_coh_pages(c).npages) << 12;
	tgm_relayout_pages gpuRows;
	tgm_scratch *gpu = tgm_scratch_get(bytes, 0, &gpuRows);
	if (!gpu)
		return false;
	tgm_cpu_clean_pages(gpuRows.pas, gpuRows.npages);
	move(c, t, gpuRows, TGM_RELAYOUT_TO_ROWS);
	tgm_cpu_clean_pages(gpuRows.pas, gpuRows.npages);
	void *maps[3] = {};
	auto *merged = static_cast<uint64_t *>(tgm_scratch_map(gpu, &maps[0]));
	const auto *rows = static_cast<const uint64_t *>(tgm_scratch_map(t.scratch, &maps[1]));
	const auto *base = static_cast<const uint64_t *>(tgm_scratch_map(t.base, &maps[2]));
	const bool mapped = merged && rows && base;
	if (mapped)
		tgm_view_merge(merged, rows, base, bytes / sizeof(uint64_t));
	for (void *map : maps)
		tgm_scratch_unmap(map);
	if (mapped) {
		tgm_cpu_clean_pages(gpuRows.pas, gpuRows.npages);
		move(c, t, gpuRows, TGM_RELAYOUT_TO_TILED);
	}
	tgm_scratch_put(gpu);
	return mapped;
}

// The last unlock: what a writer left in the scratch back into the texels, behind the GPU's writers
void drain(tgm_coh *c, tgm_tiled &t)
{
	if (t.wrote) {
		tgm_relayout_pages rows;
		tgm_scratch_pages(t.scratch, &rows);
		tgm_cpu_clean_pages(rows.pas, rows.npages);
		const bool merged = t.base && mergeBack(c, t);
		if (t.base && !merged) {
			switch_refusal("iosurface-layout", "merge-failed",
				       "IOSurface %u: no memory or mapping to merge a GPU write "
				       "made under a "
				       "CPU write lock: the unlock writes the lock's rows over it",
				       tgm_coh_surface_id(c));
		}
		if (!merged)
			move(c, t, rows, TGM_RELAYOUT_TO_TILED);
	}
	if (t.base) {
		tgm_scratch_put(t.base);
		t.base = nullptr;
	}
	if (t.stranded) {
		tgm_scratch_discard(t.scratch);
	} else {
		t.kept = t.wrote ? nullptr : t.scratch;
		t.kept_handout = tgm_scratch_handout(t.scratch);
		t.kept_written = t.filled_written;
		tgm_scratch_put(t.scratch);
	}
	t.scratch = nullptr;
	t.wrote = t.stranded = 0;
}

// Every refusal comes before anything is held, so a refused lock leaves the surface as it found it, and a lock that goes
// on always shows its task rows
int lock(tgm_coh *c, tgm_tiled &t, void *client, uint32_t id, uint32_t options, bool &held)
{
	tgm_cpu_view v;
	const int mapped = tgm_cpu_view_of(client, id, &v);
	if (mapped < 0) {
		switch_refusal("iosurface-layout", "partial-map",
			       "IOSurface %u: a task maps part of its memory: the lock is refused",
			       id);
		return 1;
	}
	int i = mapped ? findView(t, v) : -1;
	if (mapped && i < 0 && t.nviews == TGM_VIEWS) {
		switch_refusal(
			"iosurface-layout", "too-many-tasks",
			"IOSurface %u: more than %u tasks lock it at once: the lock is refused", id,
			TGM_VIEWS);
		return 1;
	}
	if (!t.locks && !t.scratch) {
		if (const int refused = fill(c, t, options))
			return refused;
	}
	if (mapped && (i < 0 || !t.views[i].locks)) {
		if (const int ret = tgm_cpu_view_remap(&v, tgm_scratch_memory(t.scratch), 0, 1)) {
			switch_refusal("iosurface-layout", "remap-failed",
				       "IOSurface %u: a task's mapping could not show its rows "
				       "(%d): the lock is "
				       "refused",
				       id, ret);
			if (!t.locks) {
				tgm_scratch_put(t.scratch);
				t.scratch = nullptr;
			}
			return 1;
		}
	}
	held = ((t.locks++) == 0u);
	if ((options & kLockReadOnly) == 0)
		t.wrote = 1;
	noteBusy(t, id);
	if (!mapped)
		return 0;
	if (i < 0) {
		i = static_cast<int>(t.nviews++);
		t.views[i].view = v;
		t.views[i].locks = 0;
		tgm_cpu_view_hold(&t.views[i].view, 1);
	}
	t.views[i].locks++;
	return 0;
}

void unlock(tgm_coh *c, tgm_tiled &t, void *client, uint32_t id, uint32_t options, bool &released)
{
	if (!t.locks)
		return;
	if (!(options & kLockReadOnly))
		t.wrote = 1;
	tgm_cpu_view v;
	const int i = tgm_cpu_view_of(client, id, &v) > 0 ? findView(t, v) : -1;
	if (i >= 0 && !--t.views[i].locks) {
		void *own = tgm_surface_memory(client, id);
		const int ret = own ? tgm_cpu_view_remap(&v, own, v.offset, 0) : -2;
		tgm_memory_release(own);
		if (ret) {
			switch_refusal("iosurface-layout", "restore-failed",
				       "IOSurface %u: a task's mapping could not be given its "
				       "surface back (%d): it "
				       "keeps the rows of its last lock",
				       id, ret);
			t.stranded = 1;
		}
		removeView(t, static_cast<uint32_t>(i));
	}
	if (!--t.locks) {
		if (t.scratch)
			drain(c, t);
		released = true;
	}
}

// A deferred surface (TegraGM20BCoherency.cpp: its last GPU import went while it was block-linear) has its texels moved
// back to rows in place at its first lock, and is pitch from then on: the move its last import's release would have made.
// A deferred entry another surface's ID finds is that surface's no longer: nothing to do for this lock.
// A kIOSurfaceLockAvoidSync lock while its GPU writers still run goes to the view, which refuses it.
bool lockedDeferred(tgm_coh *c, uint32_t index, uint32_t id, uint32_t options)
{
	const int deferred = tgm_coh_deferred_for(c, id);
	if (deferred <= 0 || index != kLock)
		return deferred < 0;
	tgm_relayout_writer writers[TGM_RELAYOUT_MAX_WAITS];
	if ((options & kLockAvoidSync) && tgm_coh_writers(c, writers, TGM_RELAYOUT_MAX_WAITS))
		return false;
	if (tgm_tiled_untile_deferred(c))
		return false;
	tgm_coh_undefer(c);
	return true;
}
}  // namespace

extern "C" void tgm_views_deferred_lock(uint32_t id, uint32_t options)
{
	if (!tgm_coh_any_deferred())
		return;
	if (tgm_coh *c = tgm_coh_find_tiled(id)) {
		lockedDeferred(c, kLock, id, options);
		tgm_coherency_put(c);
	}
}

extern "C" int tgm_views_trap(void *client, uint32_t index, uint32_t id, uint32_t options)
{
	tgm_coh *c = tgm_coh_find_tiled(id);
	if (!c)
		return 0;
	if (lockedDeferred(c, index, id, options)) {
		tgm_coherency_put(c);
		return 0;
	}
	int refused = 0;
	bool held = false;
	bool released = false;
	tgm_coh_lock(c);
	tgm_tiled *t = __atomic_load_n(tgm_coh_tiled(c), __ATOMIC_ACQUIRE);
	if (t && t->tiled) {
		const bool wasHeld = t->locks != 0u;
		dropDead(c, *t, released);
		if (index == kLock) {
			refused = lock(c, *t, client, id, options, held);
		} else {
			unlock(c, *t, client, id, options, released);
		}
		released = released && wasHeld;
		if (held) {
			tgm_coh_hold(
				c);  // (the entry lock, then the registry's: the order the decision takes them in)
		}
	}
	tgm_coh_unlock(c);
	if (released)
		tgm_coherency_put(c);
	tgm_coherency_put(c);
	return refused;
}

extern "C" void tgm_views_gpu_writes(struct tgm_coh *c)
{
	const tgm_tiled *seen = c ? __atomic_load_n(tgm_coh_tiled(c), __ATOMIC_ACQUIRE) : nullptr;
	if (!seen || !__atomic_load_n(&seen->locks, __ATOMIC_ACQUIRE) ||
	    !__atomic_load_n(&seen->wrote, __ATOMIC_ACQUIRE))
		return;
	tgm_coh_lock(c);
	tgm_tiled *t = __atomic_load_n(tgm_coh_tiled(c), __ATOMIC_ACQUIRE);
	if (t && t->tiled && t->locks && t->wrote && t->scratch && !t->base) {
		tgm_relayout_pages rows;
		tgm_scratch *base = tgm_scratch_get(
			static_cast<uint64_t>(tgm_coh_pages(c).npages) << 12, 0, &rows);
		if (base) {
			tgm_cpu_clean_pages(rows.pas, rows.npages);
			move(c, *t, rows, TGM_RELAYOUT_TO_ROWS);
			tgm_cpu_clean_pages(rows.pas, rows.npages);
			t->base = base;
			if (!t->merged) {
				t->merged = 1;
				char name[32];
				proc_selfname(name, sizeof(name));
				os_log(OS_LOG_DEFAULT,
				       "tegra-gm20b: IOSurface %u written on the GPU by %s while a "
				       "CPU "
				       "write lock holds it: its unlock keeps both writers' bytes",
				       tgm_coh_surface_id(c), name);
			}
		} else {
			switch_refusal(
				"iosurface-layout", "no-merge-scratch",
				"IOSurface %u: no memory to keep a CPU lock's rows beside a GPU "
				"write: its unlock writes the rows over it",
				tgm_coh_surface_id(c));
		}
	}
	tgm_coh_unlock(c);
}

extern "C" void tgm_views_job_writes(struct tgm_coh *const *written, uint32_t count)
{
	for (uint32_t i = 0; i < count; ++i)
		tgm_views_gpu_writes(written[i]);
}

extern "C" int tegra_gm20b_iosurface_view(void *client, uint32_t index, uint32_t surface_id,
					  uint32_t options)
{
	return tgm_views_trap(client, index, surface_id, options);
}

extern "C" int tegra_gm20b_surface_rows(uint64_t pa, void **rows, void **memory, uint64_t *offset)
{
	tgm_coh *c = tgm_coh_find_tiled_pa(pa, offset);
	if (!c)
		return 0;
	int got = 0;
	tgm_coh_lock(c);
	const tgm_tiled *t = __atomic_load_n(tgm_coh_tiled(c), __ATOMIC_ACQUIRE);
	if (t && t->tiled) {
		tgm_relayout_pages pages;
		tgm_scratch *scratch =
			tgm_scratch_get(uint64_t(tgm_coh_pages(c).npages) << 12, 0, &pages);
		if (scratch) {
			tgm_cpu_clean_pages(pages.pas, pages.npages);
			move(c, *t, pages, TGM_RELAYOUT_TO_ROWS);
			tgm_cpu_clean_pages(pages.pas, pages.npages);
			*rows = scratch;
			*memory = tgm_scratch_memory(scratch);
			got = 1;
		} else {
			switch_refusal(
				"iosurface-layout", "no-scratch",
				"IOSurface %u: no memory for a kernel reader's rows: the read is "
				"refused",
				tgm_coh_surface_id(c));
			got = -12;
		}
	}
	tgm_coh_unlock(c);
	tgm_coherency_put(c);
	return got;
}

extern "C" void tegra_gm20b_surface_rows_done(void *rows)
{
	tgm_scratch_put(static_cast<tgm_scratch *>(rows));
}

extern "C" void tgm_views_sweep(struct tgm_coh *c)
{
	bool released = false;
	tgm_coh_lock(c);
	tgm_tiled *t = __atomic_load_n(tgm_coh_tiled(c), __ATOMIC_ACQUIRE);
	if (t && t->tiled && t->locks)
		dropDead(c, *t, released);
	tgm_coh_unlock(c);
	if (released)
		tgm_coherency_put(c);
}
