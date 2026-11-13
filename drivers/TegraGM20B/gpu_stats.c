// SPDX-License-Identifier: MIT
// The statistics the rest of the driver feeds (TGM_STATS): counters, memory, imports, waits and profiles.
#include "gpu_internal.h"
#include "TegraGM20BCostHist.h"

#if TGM_STATS
struct stats_state tgm_stats;

#endif
/* stats: the "mem" line's window (tgm_stats_bo_alloc and the rest, in TegraGM20BGpu.h). Times in ns; every
 * field is summed atomically and taken (zeroed) when the window's line is logged. `recent` holds the last host imports
 * released, by first page, so an import of a surface released less than REIMPORT_NS ago counts as a re-import: what
 * keeping a released import around for a while would have saved. */
#define REIMPORT_NS (2 * NSEC_PER_SEC)
#define REIMPORT_RING 128

static struct {
	u64 cached_n, cached_bytes, cached_ns, cached_max, cached_phase[TGM_PHASES];
	u64 pool_n, pool_bytes, pool_ns, pool_max, small_n;
	u64 import_n, import_bytes, import_ns, import_max, import_phase[TGM_PHASES], import_ro,
		import_skipped, reimports;
	u64 map_n, map_pages, map_ns, map_max, unmap_n, unmap_ns, free_n, free_ns, free_max,
		prefault_n, prefault_bytes;
	u64 spare_n, spare_bytes, spare_ns, spare_max;
} mstat;

/* Who paid the window's slowest cached BoAlloc made on its own thread (best effort: the raiser of cached_max writes it). */
static char mstat_max_owner[17];
#if TGM_STATS
u64 tgm_compositor_pool; /* the compositor's pool BOs (tgm_stats_compositor_pool) */

#endif
static struct {
	u64 pa, ns;
} recent[REIMPORT_RING];

static u32 recent_next;
static DEFINE_MUTEX(recent_lock);

void tgm_stats_import(int64_t bytes, uint64_t pa)
{
	u64 now = 0;
	u32 i = 0;
	if (!stats_on())
		return;
	if (bytes > 0)
		__atomic_fetch_add(&tgm_stats.imports, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_stats.live, bytes > 0 ? 1 : -1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_stats.live_bytes, bytes, __ATOMIC_RELAXED);
	now = nvxnu_nanotime();
	mutex_lock(&recent_lock);
	if (bytes < 0) {
		/* NOLINTNEXTLINE(readability-redundant-parentheses): the compound literal needs typeof(...) */
		recent[recent_next++ % REIMPORT_RING] = (typeof(recent[0])){pa, now};
	} else {
		for (i = 0; i < REIMPORT_RING; i++) {
			if (recent[i].pa == pa && recent[i].ns &&
			    now - recent[i].ns < REIMPORT_NS) {
				recent[i].ns = 0;
				mstat.reimports++;
				break;
			}
		}
	}
	mutex_unlock(&recent_lock);
}

void tgm_stats_compositor_pool(int64_t bytes)
{
	if (stats_on())
		__atomic_fetch_add(&tgm_compositor_pool, (u64)bytes, __ATOMIC_RELAXED);
}

/* The frame timeline (SwitchFrameTimeline.h): since the display last took them, the compositor's submissions and every
 * other process's count, and every process's new-surface work (BoImport, VmBind, BoAlloc, BoFree, the imports' cache
 * cleans). Callers on several connections at once: each field on its own atomics. */
struct frame_gpu_state tgm_frame_gpu;

void tgm_stats_map(u64 small, u64 big, u64 could_be_big)
{
	if (!stats_on())
		return;
	__atomic_fetch_add(&tgm_stats.map_small, small, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_stats.map_big, big, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_stats.map_could, could_be_big, __ATOMIC_RELAXED);
}

void tgm_stats_bo_alloc(uint64_t bytes, int kind, uint64_t ns, const uint64_t phase_ns[TGM_PHASES],
			const char *owner)
{
	int i = 0;
	if (!stats_on())
		return;
	__atomic_fetch_add(&tgm_frame_gpu.allocs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_frame_gpu.alloc_ns, ns, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_stats.allocs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_stats.alloc_bytes, bytes, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_stats.alloc_ns, ns, __ATOMIC_RELAXED);
	if (bytes <= (64 << 10))
		__atomic_fetch_add(&mstat.small_n, 1, __ATOMIC_RELAXED);
	if (kind == TGM_BO_POOL) {
		__atomic_fetch_add(&mstat.pool_n, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&mstat.pool_bytes, bytes, __ATOMIC_RELAXED);
		stats_add(&mstat.pool_ns, &mstat.pool_max, ns);
		return;
	}
	if (kind == TGM_BO_SPARE) {
		__atomic_fetch_add(&mstat.spare_n, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&mstat.spare_bytes, bytes, __ATOMIC_RELAXED);
		stats_add(&mstat.spare_ns, &mstat.spare_max, ns);
		return;
	}
	__atomic_fetch_add(&mstat.cached_n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.cached_bytes, bytes, __ATOMIC_RELAXED);
	if (owner && ns > __atomic_load_n(&mstat.cached_max, __ATOMIC_RELAXED))
		strlcpy(mstat_max_owner, owner, sizeof(mstat_max_owner));
	stats_add(&mstat.cached_ns, &mstat.cached_max, ns);
	for (i = 0; i < TGM_PHASES; i++)
		__atomic_fetch_add(&mstat.cached_phase[i], phase_ns[i], __ATOMIC_RELAXED);
}

void tgm_stats_bo_import(uint64_t bytes, int read_only, int skipped, uint64_t ns,
			 const uint64_t phase_ns[TGM_PHASES])
{
	int i = 0;
	if (!stats_on())
		return;
	__atomic_fetch_add(&tgm_frame_gpu.imports, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_frame_gpu.import_ns, ns, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_frame_gpu.clean_ns, phase_ns[3], __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_frame_gpu.clean_bytes, bytes, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_bytes, bytes, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_ro, !!read_only, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_skipped, !!skipped, __ATOMIC_RELAXED);
	stats_add(&mstat.import_ns, &mstat.import_max, ns);
	for (i = 0; i < TGM_PHASES; i++)
		__atomic_fetch_add(&mstat.import_phase[i], phase_ns[i], __ATOMIC_RELAXED);
}
#if TGM_STATS
/* The "imports" line's histograms (TegraGM20BCostHist.h's buckets) and sums. */
#define SLOW_IMPORT_NS (4 * NSEC_PER_MSEC)
#define SLOW_IMPORT_LINES 8 /* a window's "slow import" lines at most */

extern uint64_t thread_get_runtime_self(void); /* DEVELOPMENT kernels export every global */
extern void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);

static struct {
	u32 total[TGM_COST_BUCKETS], wire[TGM_COST_BUCKETS], bind[TGM_COST_BUCKETS];
	u64 n, bytes, total_max, wire_ns, wire_max, wire_cpu_ns, zero_fills, pages_ns, mem_ns,
		lock_ns, lock_max, coh_ns, clean_ns, slow, slow_logged;
} icost;

uint64_t tgm_stats_cpu_ns(void)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(thread_get_runtime_self(), &ns);
	return ns;
}

uint64_t tgm_stats_zero_fills(void)
{
	struct nvxnu_faults f;
	nvxnu_vm_faults(&f);
	return f.zero_fill;
}

void tgm_stats_import_cost(const struct tgm_import_cost *c)
{
	__atomic_fetch_add(&icost.total[tgm_cost_bucket(c->total_ns)], 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&icost.wire[tgm_cost_bucket(c->wire_ns)], 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&icost.n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&icost.bytes, c->bytes, __ATOMIC_RELAXED);
	stats_add(&icost.wire_ns, &icost.wire_max, c->wire_ns);
	__atomic_fetch_add(&icost.wire_cpu_ns, min(c->wire_cpu_ns, c->wire_ns), __ATOMIC_RELAXED);
	__atomic_fetch_add(&icost.zero_fills, c->zero_fills, __ATOMIC_RELAXED);
	__atomic_fetch_add(&icost.pages_ns, c->pages_ns, __ATOMIC_RELAXED);
	__atomic_fetch_add(&icost.mem_ns, c->mem_ns, __ATOMIC_RELAXED);
	stats_add(&icost.lock_ns, &icost.lock_max, c->lock_ns);
	__atomic_fetch_add(&icost.coh_ns, c->coh_ns, __ATOMIC_RELAXED);
	__atomic_fetch_add(&icost.clean_ns, c->clean_ns, __ATOMIC_RELAXED);
	if (c->total_ns > __atomic_load_n(&icost.total_max, __ATOMIC_RELAXED))
		__atomic_store_n(&icost.total_max, c->total_ns, __ATOMIC_RELAXED);
	if (c->total_ns < SLOW_IMPORT_NS)
		return;
	__atomic_fetch_add(&icost.slow, 1, __ATOMIC_RELAXED);
	if (__atomic_fetch_add(&icost.slow_logged, 1, __ATOMIC_RELAXED) >= SLOW_IMPORT_LINES)
		return;
	nvxnu_stats("tegra-gm20b: slow import %s %llu KiB%s surface %u: %llu us = wire %llu "
		    "(%llu on CPU, %llu zero-fill faults) + pages %llu + nvkm %llu (gpu.lock wait "
		    "%llu) + coherency %llu + clean %llu us",
		    c->owner, c->bytes >> 10, c->read_only ? " read-only" : "", c->surface_id,
		    c->total_ns / 1000, c->wire_ns / 1000, min(c->wire_cpu_ns, c->wire_ns) / 1000,
		    c->zero_fills, c->pages_ns / 1000, c->mem_ns / 1000, c->lock_ns / 1000,
		    c->coh_ns / 1000, c->clean_ns / 1000);
}

/* The window's "imports" line (tgm_stats_mem): its histograms and sums taken, as mstat's are. */
static void stats_imports(u64 ms, u64 map_n)
{
	u32 total[TGM_COST_BUCKETS];
	u32 wire[TGM_COST_BUCKETS];
	u32 bind[TGM_COST_BUCKETS];
	u64 n = 0;
	u64 bytes = 0;
	u64 total_max = 0;
	u64 wire_ns = 0;
	u64 wire_max = 0;
	u64 wire_cpu = 0;
	u64 zero_fills = 0;
	u64 pages = 0;
	u64 mem = 0;
	u64 lock = 0;
	u64 lock_max = 0;
	u64 coh = 0;
	u64 clean = 0;
	u64 slow = 0;
	unsigned int b = 0;
	for (b = 0; b < TGM_COST_BUCKETS; b++) {
		total[b] = __atomic_exchange_n(&icost.total[b], 0, __ATOMIC_RELAXED);
		wire[b] = __atomic_exchange_n(&icost.wire[b], 0, __ATOMIC_RELAXED);
		bind[b] = __atomic_exchange_n(&icost.bind[b], 0, __ATOMIC_RELAXED);
	}
#define TAKE(field) __atomic_exchange_n(&icost.field, 0, __ATOMIC_RELAXED)
	n = TAKE(n);
	bytes = TAKE(bytes);
	total_max = TAKE(total_max);
	wire_ns = TAKE(wire_ns);
	wire_max = TAKE(wire_max);
	wire_cpu = TAKE(wire_cpu_ns);
	zero_fills = TAKE(zero_fills);
	pages = TAKE(pages_ns);
	mem = TAKE(mem_ns);
	lock = TAKE(lock_ns);
	lock_max = TAKE(lock_max);
	coh = TAKE(coh_ns);
	clean = TAKE(clean_ns);
	slow = TAKE(slow);
	TAKE(slow_logged);
#undef TAKE
	if (!n && !map_n)
		return;
	nvxnu_stats("tegra-gm20b: imports %llu in %llu ms (%llu KiB): total p50 %llu p99 %llu max "
		    "%llu us; wire p50 %llu p99 %llu max %llu us, %llu%% on CPU, %llu zero-fill "
		    "faults; mean pages %llu, nvkm %llu (gpu.lock wait %llu, max %llu), coherency "
		    "%llu, clean %llu us; %llu over 4 ms; VmBind %llu: p50 %llu p99 %llu us",
		    n, ms, bytes >> 10, tgm_cost_percentile(total, n, 500),
		    tgm_cost_percentile(total, n, 990), total_max / 1000,
		    tgm_cost_percentile(wire, n, 500), tgm_cost_percentile(wire, n, 990),
		    wire_max / 1000, wire_ns ? wire_cpu * 100 / wire_ns : 0, zero_fills,
		    n ? pages / n / 1000 : 0, n ? mem / n / 1000 : 0, n ? lock / n / 1000 : 0,
		    lock_max / 1000, n ? coh / n / 1000 : 0, n ? clean / n / 1000 : 0, slow, map_n,
		    tgm_cost_percentile(bind, map_n, 500), tgm_cost_percentile(bind, map_n, 990));
}

#endif
void tgm_stats_bind(int map, uint64_t pages, uint64_t ns)
{
	if (!stats_on())
		return;
	__atomic_fetch_add(&tgm_frame_gpu.binds, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_frame_gpu.bind_ns, ns, __ATOMIC_RELAXED);
	if (map) {
#if TGM_STATS
		__atomic_fetch_add(&icost.bind[tgm_cost_bucket(ns)], 1, __ATOMIC_RELAXED);
#endif
		__atomic_fetch_add(&mstat.map_n, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&mstat.map_pages, pages, __ATOMIC_RELAXED);
		stats_add(&mstat.map_ns, &mstat.map_max, ns);
	} else {
		__atomic_fetch_add(&mstat.unmap_n, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&mstat.unmap_ns, ns, __ATOMIC_RELAXED);
	}
}

void tgm_stats_bo_free(uint64_t ns)
{
	if (!stats_on())
		return;
	__atomic_fetch_add(&tgm_frame_gpu.frees, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.free_n, 1, __ATOMIC_RELAXED);
	stats_add(&mstat.free_ns, &mstat.free_max, ns);
}

void tgm_stats_wait(uint32_t waiter, int woken, int done)
{
	u64 lat = 0;

	if (!stats_on())
		return;
	__atomic_fetch_add(&tgm_wstat.slept, 1, __ATOMIC_RELAXED);
	if (!woken) {
		__atomic_fetch_add(&tgm_wstat.deadline, 1, __ATOMIC_RELAXED);
		return;
	}
	__atomic_fetch_add(&tgm_wstat.woken, 1, __ATOMIC_RELAXED);
	if (!done) {
		__atomic_fetch_add(&tgm_wstat.other, 1, __ATOMIC_RELAXED);
		return;
	}
	lat = nvxnu_nanotime() -
	      __atomic_load_n(&tgm_waiter_woken_ns[waiter % TGM_WAITERS], __ATOMIC_RELAXED);
	__atomic_fetch_add(&tgm_wstat.lat_sum, lat, __ATOMIC_RELAXED);
	if (lat > __atomic_load_n(&tgm_wstat.lat_max, __ATOMIC_RELAXED))
		__atomic_store_n(&tgm_wstat.lat_max, lat, __ATOMIC_RELAXED);
}

#if TGM_STATS
/* The "waits" line: GPU interrupt passes by line and per job kicked, channel wakes, and how the fence waits that slept
 * woke: at their seqno, early (another seqno reached, a kill, a teardown) or at their deadline. */
void tgm_stats_waits(u64 ms)
{
	u64 stall = 0;
	u64 nonstall = 0;
	u64 ds = 0;
	u64 dn = 0;
	u64 sum = 0;
	u64 max = 0;
	u64 per_job = 0;
	u32 wakes = 0;
	u32 slept = 0;
	u32 woken = 0;
	u32 other = 0;
	u32 deadline = 0;
	u32 jobs = 0;
	u32 quiet = 0;
	u32 asked = 0;
	u32 pushed = 0;
	u32 settled = 0;
	u32 per = ms ? (u32)ms : 1;
	char line[400];

	nvxnu_irq_passes(&stall, &nonstall);
	ds = stall - tgm_wstat.stall;
	dn = nonstall - tgm_wstat.nonstall;
	tgm_wstat.stall = stall;
	tgm_wstat.nonstall = nonstall;
	wakes = __atomic_exchange_n(&tgm_wstat.wakes, 0, __ATOMIC_RELAXED);
	slept = __atomic_exchange_n(&tgm_wstat.slept, 0, __ATOMIC_RELAXED);
	woken = __atomic_exchange_n(&tgm_wstat.woken, 0, __ATOMIC_RELAXED);
	other = __atomic_exchange_n(&tgm_wstat.other, 0, __ATOMIC_RELAXED);
	deadline = __atomic_exchange_n(&tgm_wstat.deadline, 0, __ATOMIC_RELAXED);
	jobs = __atomic_exchange_n(&tgm_wstat.jobs, 0, __ATOMIC_RELAXED);
	quiet = __atomic_exchange_n(&tgm_wstat.quiet, 0, __ATOMIC_RELAXED);
	asked = __atomic_exchange_n(&tgm_wstat.asked, 0, __ATOMIC_RELAXED);
	pushed = __atomic_exchange_n(&tgm_wstat.pushed, 0, __ATOMIC_RELAXED);
	settled = __atomic_exchange_n(&tgm_wstat.settled, 0, __ATOMIC_RELAXED);
	sum = __atomic_exchange_n(&tgm_wstat.lat_sum, 0, __ATOMIC_RELAXED);
	max = __atomic_exchange_n(&tgm_wstat.lat_max, 0, __ATOMIC_RELAXED);
	per_job = jobs ? (ds + dn) * 100 / jobs : 0;
	scnprintf(
		line, sizeof(line),
		"%llu ms: interrupt passes %llu/s stall, %llu/s nonstall, %llu.%02llu per job "
		"(%u jobs); channel wakes %llu/s; fence waits slept %llu/s: %u woken, %u at their "
		"seqno (wake to waiter mean %llu us max %llu us), %u at their deadline; %u quiet "
		"jobs, %u wake asks, %u wake entries, %u idle settles",
		ms, ds * 1000 / per, dn * 1000 / per, per_job / 100, per_job % 100, jobs,
		(u64)wakes * 1000 / per, (u64)slept * 1000 / per, woken, woken - other,
		woken > other ? sum / (woken - other) / 1000 : 0, max / 1000, deadline, quiet,
		asked, pushed, settled);
	nvxnu_stats("tegra-gm20b: waits %s", line);
}

#endif
void tgm_stats_prefault(uint64_t bytes)
{
	if (!stats_on())
		return;
	__atomic_fetch_add(&mstat.prefault_n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.prefault_bytes, bytes, __ATOMIC_RELAXED);
}

#if TGM_STATS
/* The window's "mem" line: BoAllocs (cached: mean/max and each phase's total; pool; how many were 64 KiB or less),
 * BoFrees, prefaulted views, and the system's page faults over the window (every vm_fault, and those that zero-filled a
 * page: XNU's own counters; with the kernel's user-fault timing, their mean and longest); and its "mem2" line: host
 * imports (mean/max, phase totals, read-only ones, cleans the registry skipped, re-imports of a surface released < 2 s
 * before) and VmBind maps (pages, mean/max) and unmaps. Each only when something happened. */
void tgm_stats_mem(u64 ms)
{
	u64 v[sizeof(mstat) / sizeof(u64)];
	struct nvxnu_faults f;
	static struct nvxnu_faults last;
	static char line
		[800]; /* one caller at a time: the submission that won the window (tgm_stats_tick) */
	char user[96] = "";
	unsigned int k = 0;
#define MS(field) v[offsetof(typeof(mstat), field) / sizeof(u64)]
#define US(x) ((x) / 1000)
#define MEAN_US(sum, n) ((n) ? (sum) / (n) / 1000 : 0)
	for (k = 0; k < ARRAY_SIZE(v); k++)
		v[k] = __atomic_exchange_n(&((u64 *)&mstat)[k], 0, __ATOMIC_RELAXED);
	nvxnu_vm_faults(&f);
	if (!last.faults)
		last = f;
	if (f.user_n > last.user_n) {
		scnprintf(user, sizeof(user), "; user faults %llu mean %llu us max %llu us",
			  f.user_n - last.user_n,
			  (f.user_ns - last.user_ns) / (f.user_n - last.user_n) / 1000,
			  f.user_max_ns / 1000);
	}
	if (MS(cached_n) || MS(pool_n) || MS(free_n) || MS(spare_n)) {
		uint64_t kept = 0;
		uint64_t ready = 0;
		uint64_t zeroing = 0;
		tgm_spare_counts(&kept, &ready, &zeroing);
		scnprintf(
			line, sizeof(line),
			"BoAlloc spare %llu (%llu KiB) mean %llu us max %llu us, cached %llu (%llu "
			"KiB) mean %llu us max %llu us (%s) [alloc+wire %llu, clean %llu, pages "
			"%llu, "
			"nvkm %llu us], pool %llu (%llu KiB) mean %llu us max %llu us, %llu of <= "
			"64 "
			"KiB; spares %llu KiB in %llu, pool %llu KiB zeroing; BoFree %llu "
			"mean %llu us max %llu us; %llu prefaulted maps (%llu KiB); faults "
			"%llu/s (%llu/s zero-fill)%s",
			MS(spare_n), MS(spare_bytes) >> 10, MEAN_US(MS(spare_ns), MS(spare_n)),
			US(MS(spare_max)), MS(cached_n), MS(cached_bytes) >> 10,
			MEAN_US(MS(cached_ns), MS(cached_n)), US(MS(cached_max)),
			MS(cached_n) ? mstat_max_owner : "-", US(MS(cached_phase[0])),
			US(MS(cached_phase[1])), US(MS(cached_phase[2])), US(MS(cached_phase[3])),
			MS(pool_n), MS(pool_bytes) >> 10, MEAN_US(MS(pool_ns), MS(pool_n)),
			US(MS(pool_max)), MS(small_n), (unsigned long long)(kept >> 10),
			(unsigned long long)ready, (unsigned long long)(zeroing >> 10), MS(free_n),
			MEAN_US(MS(free_ns), MS(free_n)), US(MS(free_max)), MS(prefault_n),
			MS(prefault_bytes) >> 10, (f.faults - last.faults) * 1000 / (ms ? ms : 1),
			(f.zero_fill - last.zero_fill) * 1000 / (ms ? ms : 1), user);
		nvxnu_stats("tegra-gm20b: mem %s", line);
	}
	if (MS(import_n) || MS(map_n) || MS(unmap_n)) {
		scnprintf(line, sizeof(line),
			  "BoImport %llu (%llu KiB, %llu read-only, %llu clean skipped, %llu "
			  "re-imports) mean "
			  "%llu us max %llu us [wire %llu, pages %llu, nvkm %llu, clean %llu us]; "
			  "VmBind %llu maps (%llu pages) mean "
			  "%llu us max %llu us, %llu unmaps %llu us",
			  MS(import_n), MS(import_bytes) >> 10, MS(import_ro), MS(import_skipped),
			  MS(reimports), MEAN_US(MS(import_ns), MS(import_n)), US(MS(import_max)),
			  US(MS(import_phase[0])), US(MS(import_phase[1])), US(MS(import_phase[2])),
			  US(MS(import_phase[3])), MS(map_n), MS(map_pages),
			  MEAN_US(MS(map_ns), MS(map_n)), US(MS(map_max)), MS(unmap_n),
			  US(MS(unmap_ns)));
		nvxnu_stats("tegra-gm20b: mem2 %s", line);
	}
#if TGM_STATS
	stats_imports(ms, MS(map_n));
#endif
	if (tgm_owner_stats(line, sizeof(line)))
		nvxnu_stats("tegra-gm20b: mem3 BoAllocs by process: %s", line);
	last = f;
#undef MS
#undef US
#undef MEAN_US
}

const char *tgm_prof_name(unsigned int c, char *buf, unsigned int len)
{
	static const char *const size[] = {"full", "mid", "small"};
	static const char *const other[] = {"pass", "sync",  "compute", "copy",
					    "2D",   "state", "fence"};
	if (c < TGM_PC_PASS) {
		scnprintf(buf, len, "%s %s %s%s", c < TGM_PC_CLEAR ? "draw" : "clear",
			  size[(c % 12) / 4], c & 2 ? "pitch" : "block", c & 1 ? " MSAA" : "");
	} else {
		scnprintf(buf, len, "%s", other[c - TGM_PC_PASS]);
	}
	return buf;
}
#endif
