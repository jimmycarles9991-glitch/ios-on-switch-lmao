// SPDX-License-Identifier: MIT
// The periodic statistics lines (TGM_STATS): job stamps, fences, clocks, submissions and owners.
#include "gpu_internal.h"

#if TGM_STATS
/* TegraVIC (drivers/TegraVIC/SwitchVICMapping.cpp, debug kernels) */
extern int switch_vic_mapping_describe(nvxnu_u32 iova, char *out, unsigned long size)
	__attribute__((weak_import));

/* stats:
 * GPU-side times come from 16-byte semaphore releases, which write PTIMER. On this Tegra PTIMER is not in nanoseconds:
 * its source is 19.2 MHz while its 32 ns step assumes 31.25 MHz (nvgpu gm20b_tegra_platform.ptimer_src_freq,
 * os/linux/platform_gk20a_tegra.c; PTIMER_REF_FREQ_HZ, include/nvgpu/ptimer.h), so it runs at 0.6144 x real time and nvgpu
 * scales every PTIMER timeout by it; nouveau's gk20a timer programs no rate (subdev/timer/gk20a.c has no init). The
 * user fence latency (CPU clock) measures 1.63-1.65 x the stamped queue + exec in every window. Each window's
 * stamps are converted to nanoseconds with PTIMER's rate measured over that window against the CPU clock, which the
 * line prints ("PTIMER xR"). */
#define STATS_WINDOW_NS (5 * NSEC_PER_SEC)

/* A process's SubmitAddress calls over the window, by phase (ns), with the longest call */
struct stats_submits {
	u64 ns[TGM_SUBMIT_PHASES], total, max;
	u32 n;
};

static void stats_submits_merge(struct stats_submits *into, const struct stats_submits *from)
{
	unsigned int p = 0;

	for (p = 0; p < TGM_SUBMIT_PHASES; p++)
		into->ns[p] += from->ns[p];
	into->total += from->total;
	into->n += from->n;
	if (from->max > into->max)
		into->max = from->max;
}

void tgm_stats_submit(struct tgm_chan *chan, const uint64_t ns[TGM_SUBMIT_PHASES])
{
	u64 total = 0;
	unsigned int p = 0;

	for (p = 0; p < TGM_SUBMIT_PHASES; p++) {
		__atomic_fetch_add(&chan->stat_submit_ns[p], ns[p], __ATOMIC_RELAXED);
		total += ns[p];
	}
	stats_add(&chan->stat_submit_total, &chan->stat_submit_max, total);
	__atomic_fetch_add(&chan->stat_submits, 1, __ATOMIC_RELAXED);
}

/* tgm_live_lock held */
static void stats_submits_take(struct tgm_chan *chan, struct stats_submits *s)
{
	unsigned int p = 0;

	for (p = 0; p < TGM_SUBMIT_PHASES; p++)
		s->ns[p] = __atomic_exchange_n(&chan->stat_submit_ns[p], 0, __ATOMIC_RELAXED);
	s->total = __atomic_exchange_n(&chan->stat_submit_total, 0, __ATOMIC_RELAXED);
	s->max = __atomic_exchange_n(&chan->stat_submit_max, 0, __ATOMIC_RELAXED);
	s->n = __atomic_exchange_n(&chan->stat_submits, 0, __ATOMIC_RELAXED);
}

static unsigned int stats_submits_line(char *buf, unsigned int len, const struct stats_submits *s)
{
	if (!s->n)
		return 0;
	return scnprintf(buf, len,
			 ", SubmitAddress %u x mean %llu us (resolve %llu, lock %llu, exec %llu, "
			 "commit %llu) max %llu us",
			 s->n, s->total / s->n / 1000, s->ns[TGM_SUBMIT_RESOLVE] / s->n / 1000,
			 s->ns[TGM_SUBMIT_LOCK] / s->n / 1000, s->ns[TGM_SUBMIT_EXEC] / s->n / 1000,
			 s->ns[TGM_SUBMIT_COMMIT] / s->n / 1000, s->max / 1000);
}

/* The window's "exec" line: each process's stamped GPU time per submission (start to end stamp, the L2 flush included:
 * what its work held the GPU for, other channels' time slices inside it included), summed over its channels and taken from
 * them; the window's first EXEC_OWNERS - 1 processes by name, the rest as "(others)". `rate` converts PTIMER units to ns
 * (16.16, tgm_stats_tick). Called from the submission that won the window, never with tgm_live_lock held. */
#define EXEC_OWNERS 5

static void stats_exec_owners(char *line, unsigned int len, u64 rate)
{
	/* static: one caller at a time (the window's winner, tgm_stats_tick) */
	static struct {
		char name[17];
		u64 sum, max;
		u32 n, subs, stamped, stamps, spans;
		u64 span;
		u64 prof_ns[TGM_PC_COUNT];
		u32 prof_n[TGM_PC_COUNT];
		u64 prof_dn[TGM_PC_COUNT], prof_sdn[TGM_PC_COUNT];
		struct stats_submits submits;
		struct switch_gpu_job_window jobs;
		struct switch_gpu_prof_seq seq;
	} o[EXEC_OWNERS];
	struct switch_gpu_job_window taken;
	static struct switch_gpu_prof_seq seq; /* one caller at a time, as o */
	struct tgm_chan *chan = NULL;
	unsigned int used = 0;
	unsigned int i = 0;
	unsigned int c = 0;
	unsigned int at = 0;
	char name[32];

	memset(o, 0, sizeof(o));
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		u32 n = __atomic_exchange_n(&chan->stat_exec_n, 0, __ATOMIC_RELAXED);
		u64 sum = __atomic_exchange_n(&chan->stat_exec, 0, __ATOMIC_RELAXED);
		u64 max = __atomic_exchange_n(&chan->stat_exec_max, 0, __ATOMIC_RELAXED);
		u32 subs = __atomic_exchange_n(&chan->prof_subs, 0, __ATOMIC_RELAXED);
		u32 stamped = __atomic_exchange_n(&chan->prof_stamped, 0, __ATOMIC_RELAXED);
		struct stats_submits submits = {0};

		stats_submits_take(chan, &submits);
		if ((!n && !subs && !stamped && !submits.n) || chan->kernel)
			continue;
		for (i = 0; i < used && strncmp(o[i].name, chan->owner, sizeof(o[i].name)); i++)
			;
		if (i == used && used < EXEC_OWNERS - 1) {
			strlcpy(o[used++].name, chan->owner, sizeof(o[0].name));
		} else if (i == used) {
			i = EXEC_OWNERS - 1;
			strlcpy(o[i].name, "(others)", sizeof(o[0].name));
		}
		o[i].n += n;
		o[i].sum += sum;
		if (max > o[i].max)
			o[i].max = max;
		o[i].subs += subs;
		o[i].stamped += stamped;
		o[i].stamps += __atomic_exchange_n(&chan->prof_stamps, 0, __ATOMIC_RELAXED);
		o[i].spans += __atomic_exchange_n(&chan->prof_spans, 0, __ATOMIC_RELAXED);
		o[i].span += __atomic_exchange_n(&chan->prof_span, 0, __ATOMIC_RELAXED);
		stats_submits_merge(&o[i].submits, &submits);
		switch_gpu_job_take(&chan->stat_jobs, &taken);
		switch_gpu_job_merge(&o[i].jobs, &taken);
		switch_gpu_prof_seq_take(&chan->prof_slow, &seq);
		if (seq.n && seq.ns > o[i].seq.ns)
			o[i].seq = seq;
		for (c = 0; c < TGM_PC_COUNT; c++) {
			o[i].prof_ns[c] +=
				__atomic_exchange_n(&chan->prof_ns[c], 0, __ATOMIC_RELAXED);
			o[i].prof_n[c] +=
				__atomic_exchange_n(&chan->prof_n[c], 0, __ATOMIC_RELAXED);
			o[i].prof_dn[c] +=
				__atomic_exchange_n(&chan->prof_dn[c], 0, __ATOMIC_RELAXED);
			o[i].prof_sdn[c] +=
				__atomic_exchange_n(&chan->prof_sdn[c], 0, __ATOMIC_RELAXED);
		}
	}
	mutex_unlock(&tgm_live_lock);
	line[0] = 0;
	for (i = 0; i < EXEC_OWNERS; i++) {
		if (!o[i].n && !o[i].submits.n)
			continue;
		at += scnprintf(line + at, len - at, "%s%s", at ? "; " : "", o[i].name);
		if (o[i].n)
			at += scnprintf(line + at, len - at, " %u x mean %llu us max %llu us",
					o[i].n, ((o[i].sum / o[i].n) * rate >> 16) / 1000,
					(o[i].max * rate >> 16) / 1000);
		at += stats_submits_line(line + at, len - at, &o[i].submits);
	}
	if (at)
		nvxnu_stats("tegra-gm20b: exec by process: %s", line);
	for (i = 0; i < EXEC_OWNERS; i++)
		if (o[i].jobs.batches > o[i].jobs.jobs &&
		    switch_gpu_job_format(line, len, o[i].name, &o[i].jobs))
			nvxnu_stats("tegra-gm20b: jobs %s", line);
	/* each process's profiled submissions: their GPU time by push class, the largest first, per submission */
	for (i = 0; i < EXEC_OWNERS; i++) {
		u64 total = 0;
		u64 left = 0;
		u32 shown = 0;
		if (!o[i].subs)
			continue;
		for (c = 0; c < TGM_PC_COUNT; c++)
			total += o[i].prof_ns[c];
		at = scnprintf(line, len,
			       "%s: %u profiled submits, %llu us of GPU each:", o[i].name,
			       o[i].subs, ((total / o[i].subs) * rate >> 16) / 1000);
		left = total;
		while (shown < 10 && left) {
			unsigned int best = 0;
			for (c = 1; c < TGM_PC_COUNT; c++) {
				if (o[i].prof_ns[c] > o[i].prof_ns[best])
					best = c;
			}
			if (!o[i].prof_ns[best])
				break;
			at += scnprintf(
				line + at, len - at, "%s %s %u x %llu us (%llu us/submit",
				shown ? ";" : "", tgm_prof_name(best, name, sizeof(name)),
				o[i].prof_n[best],
				((o[i].prof_ns[best] / max_t(u32, o[i].prof_n[best], 1)) * rate >>
				 16) / 1000,
				((o[i].prof_ns[best] / o[i].subs) * rate >> 16) / 1000);
			if (o[i].prof_dn[best]) {
				at += scnprintf(
					line + at, len - at, ", %llu draws/push, %llu per-sample",
					o[i].prof_dn[best] / max_t(u32, o[i].prof_n[best], 1),
					o[i].prof_sdn[best] / max_t(u32, o[i].prof_n[best], 1));
			}
			at += scnprintf(line + at, len - at, ")");
			left -= o[i].prof_ns[best];
			o[i].prof_ns[best] = 0;
			shown++;
		}
		nvxnu_stats("tegra-gm20b: gpuprof %s", line);
		if (switch_gpu_prof_seq_format(line, len, o[i].name, &o[i].seq))
			nvxnu_stats("tegra-gm20b: gpuprof sequence %s", line);
	}
	/* what the profile's stamps cost each owner, so a pacing change between kernels with and without them is told apart */
	for (i = 0; i < EXEC_OWNERS; i++) {
		const struct switch_gpu_prof_cost cost = {
			o[i].stamped,           o[i].stamps,          o[i].spans, o[i].n,
			o[i].span * rate >> 16, o[i].sum * rate >> 16};

		if (switch_gpu_prof_cost_format(line, len, &cost))
			nvxnu_stats("tegra-gm20b: gpuprof cost %s:%s", o[i].name, line);
	}
}

/* The compositor's deadline (deadline.c, hold.c), and each app channel's preemption latency and the one-period turns it
 * got, those above the minimum or with a turn, by owner. */
static void stats_deadline(char *buf, unsigned int len)
{
	char deadline[320];
	char holds[240];
	struct tgm_chan *chan = NULL;
	unsigned int n = 0;
	unsigned int turns = 0;
	unsigned int shown = 0;
	u64 latency = 0;

	tgm_deadline_stats(deadline, sizeof(deadline));
	tgm_hold_stats(holds, sizeof(holds));
	n = scnprintf(buf, len, "%s; %s; latency by channel:", deadline, holds);
	mutex_lock(&tgm_live_lock);
	list_for_each_entry(chan, &tgm_live_chans, live)
	{
		if (!chan->tracked)
			continue;
		tgm_hold_chan_stats(chan->chid, &latency, &turns);
		if (latency <= TGM_PREEMPT_MIN_NS && !turns)
			continue;
		if (turns) {
			n += scnprintf(buf + n, len - n, " %s ch%u %llu us (%u turns)", chan->owner,
				       chan->chid, latency / 1000, turns);
		} else {
			n += scnprintf(buf + n, len - n, " %s ch%u %llu us", chan->owner,
				       chan->chid, latency / 1000);
		}
		shown++;
	}
	mutex_unlock(&tgm_live_lock);
	if (!shown)
		scnprintf(buf + n, len - n, " all at the minimum");
}

/* Run by each submission: the first one after a window's end logs it. */
void tgm_stats_tick(void)
{
	u64 now = nvxnu_nanotime();
	u64 since = __atomic_load_n(&tgm_stats.since, __ATOMIC_RELAXED);
	u64 ms = 0;
	u64 sum = 0;
	u64 max = 0;
	u64 qs = 0;
	u64 qm = 0;
	u64 es = 0;
	u64 em = 0;
	u64 ps = 0;
	u64 pm = 0;
	u32 submits = 0;
	u32 fences = 0;
	u32 imports = 0;
	u32 timed = 0;
	u32 presents = 0;
	u32 idle = 0;
	u32 writer = 0;
	u32 present_flushes = 0;
	u32 allocs = 0;
	u32 mc_int = 0;
	u32 mc_err = 0;
	u32 mc_adr = 0;
	u32 copies = 0;
	u32 comp = 0;
	u32 behind = 0;
	u32 lnew = 0;
	u32 lfree = 0;
	u64 abytes = 0;
	u64 ans = 0;
	u64 pt = 0;
	u64 cal = 0;
	u64 rate = 0;
	u64 cs = 0;
	u64 cm = 0;
	u64 busy = 0;
	u64 cos = 0;
	u64 com = 0;
	u64 bs = 0;
	u64 bm = 0;
	u64 lns = 0;
	u64 lnm = 0;
	u64 lfs = 0;
	u64 lfm = 0;
	char line[1024];
	char clk[120];
	char scrubbed[200];
	char sched[120];
	char recov[128];

	__atomic_fetch_add(&tgm_stats.submits, 1, __ATOMIC_RELAXED);
	if (!since) {
		if (__atomic_compare_exchange_n(&tgm_stats.since, &since, now, false,
						__ATOMIC_RELAXED, __ATOMIC_RELAXED))
			tgm_stats.cal_ptimer = nvkm_timer_read(tgm_gpu.device->timer);
		return;
	}
	if (now - since < STATS_WINDOW_NS ||
	    !__atomic_compare_exchange_n(&tgm_stats.since, &since, now, false, __ATOMIC_RELAXED,
					 __ATOMIC_RELAXED))
		return;
	ms = (now - since) / 1000000;
	/* nanoseconds per PTIMER unit over this window, 16.16 fixed point; the window's stamps were taken within it */
	pt = nvkm_timer_read(tgm_gpu.device->timer);
	cal = tgm_stats.cal_ptimer;
	tgm_stats.cal_ptimer = pt;
	rate = cal && pt > cal ? ((now - since) << 16) / (pt - cal) : 1 << 16;
#define PT_NS(v) (((v) * rate) >> 16)
	submits = __atomic_exchange_n(&tgm_stats.submits, 0, __ATOMIC_RELAXED);
	fences = __atomic_exchange_n(&tgm_stats.fences, 0, __ATOMIC_RELAXED);
	imports = __atomic_exchange_n(&tgm_stats.imports, 0, __ATOMIC_RELAXED);
	sum = __atomic_exchange_n(&tgm_stats.lat_sum, 0, __ATOMIC_RELAXED);
	max = __atomic_exchange_n(&tgm_stats.lat_max, 0, __ATOMIC_RELAXED);
	timed = __atomic_exchange_n(&tgm_stats.timed, 0, __ATOMIC_RELAXED);
	tgm_present_stats(&presents, &ps, &pm);
	idle = __atomic_exchange_n(&tgm_stats.idle, 0, __ATOMIC_RELAXED);
	writer = __atomic_exchange_n(&tgm_stats.writer, 0, __ATOMIC_RELAXED);
	present_flushes = __atomic_exchange_n(&tgm_stats.present_flushes, 0, __ATOMIC_RELAXED);
	allocs = __atomic_exchange_n(&tgm_stats.allocs, 0, __ATOMIC_RELAXED);
	abytes = __atomic_exchange_n(&tgm_stats.alloc_bytes, 0, __ATOMIC_RELAXED);
	ans = __atomic_exchange_n(&tgm_stats.alloc_ns, 0, __ATOMIC_RELAXED);
	qs = __atomic_exchange_n(&tgm_stats.queue_sum, 0, __ATOMIC_RELAXED);
	qm = __atomic_exchange_n(&tgm_stats.queue_max, 0, __ATOMIC_RELAXED);
	es = __atomic_exchange_n(&tgm_stats.exec_sum, 0, __ATOMIC_RELAXED);
	em = __atomic_exchange_n(&tgm_stats.exec_max, 0, __ATOMIC_RELAXED);
	copies = __atomic_exchange_n(&tgm_stats.copies, 0, __ATOMIC_RELAXED);
	cs = __atomic_exchange_n(&tgm_stats.copy_sum, 0, __ATOMIC_RELAXED);
	cm = __atomic_exchange_n(&tgm_stats.copy_max, 0, __ATOMIC_RELAXED);
	comp = __atomic_exchange_n(&tgm_stats.comp, 0, __ATOMIC_RELAXED);
	cos = __atomic_exchange_n(&tgm_stats.comp_sum, 0, __ATOMIC_RELAXED);
	com = __atomic_exchange_n(&tgm_stats.comp_max, 0, __ATOMIC_RELAXED);
	behind = __atomic_exchange_n(&tgm_stats.behind, 0, __ATOMIC_RELAXED);
	bs = __atomic_exchange_n(&tgm_stats.behind_sum, 0, __ATOMIC_RELAXED);
	bm = __atomic_exchange_n(&tgm_stats.behind_max, 0, __ATOMIC_RELAXED);
	tgm_sched_stats(sched, sizeof(sched));
	tgm_recover_stats(recov, sizeof(recov));
	lnew = __atomic_exchange_n(&tgm_stats.lock_new, 0, __ATOMIC_RELAXED);
	lns = __atomic_exchange_n(&tgm_stats.lock_new_sum, 0, __ATOMIC_RELAXED);
	lnm = __atomic_exchange_n(&tgm_stats.lock_new_max, 0, __ATOMIC_RELAXED);
	lfree = __atomic_exchange_n(&tgm_stats.lock_free, 0, __ATOMIC_RELAXED);
	lfs = __atomic_exchange_n(&tgm_stats.lock_free_sum, 0, __ATOMIC_RELAXED);
	lfm = __atomic_exchange_n(&tgm_stats.lock_free_max, 0, __ATOMIC_RELAXED);
	tegra_gm20b_mc_errors(&mc_int, &mc_err, &mc_adr);
	if ((mc_err & 0xff) == 0x6c && switch_vic_mapping_describe) {
		char vic[128];
		if (!switch_vic_mapping_describe(mc_adr, vic, sizeof(vic)))
			nvxnu_log("tegra-gm20b: MC refused a VIC read at %08x: %s", mc_adr, vic);
	}
	/* the GPU's share of the window spent on stamped user submissions and copies */
	busy = PT_NS(es + cs) * 100 / (now - since);
	tgm_clk_measure(clk, sizeof(clk));
	tgm_scrub_stats(scrubbed, sizeof(scrubbed), ms);
	/* formatted here and logged as one string: os_log drops a message whose scalar arguments outgrow its 256-byte buffer
	 * (libkern/os/log.h OS_LOG_BUFFER_MAX_SIZE), which a string argument does not count against */
	scnprintf(line, sizeof(line),
		  "%llu ms: %u submits (%llu/s), fence latency mean %llu us max %llu us (%u), GPU "
		  "queue "
		  "mean %llu us max %llu us, exec mean %llu us max %llu us (%u stamped), busy "
		  "%llu%% (PTIMER x%llu.%03llu), "
		  "L2 %s, %u presents (%u writer-only, %u L2 flushes), "
		  "offer to done mean %llu us max "
		  "%llu us, %s; compositor GPU queue mean %llu us max %llu us (%u), with an app on "
		  "the GPU at its kick mean %llu us max %llu us (%u); %s; %s",
		  ms, submits, submits * 1000ULL / (ms ? ms : 1), fences ? sum / fences / 1000 : 0,
		  max / 1000, fences, timed ? PT_NS(qs) / timed / 1000 : 0, PT_NS(qm) / 1000,
		  timed ? PT_NS(es) / timed / 1000 : 0, PT_NS(em) / 1000, timed, busy, rate >> 16,
		  ((rate & 0xffff) * 1000) >> 16, l2_on() ? "cached" : "volatile", presents, writer,
		  present_flushes, presents ? ps / presents / 1000 : 0, pm / 1000, clk,
		  comp ? PT_NS(cos) / comp / 1000 : 0, PT_NS(com) / 1000, comp,
		  behind ? PT_NS(bs) / behind / 1000 : 0, PT_NS(bm) / 1000, behind, sched, recov);
	nvxnu_stats("tegra-gm20b: stats %s", line);
	stats_deadline(line, sizeof(line));
	nvxnu_stats("tegra-gm20b: deadline %s", line);
	tgm_acquire_probe_window(line, sizeof(line), now);
	if (line[0])
		nvxnu_stats("tegra-gm20b: acquire probe: %s", line);
	switch_gpu_compositor_sync_window(line, sizeof(line));
	if (line[0])
		nvxnu_stats("tegra-gm20b: compositor sync pushes in profiled frames: %s", line);
	/* the pool by holder, so no share of it goes unnamed */
	uint64_t pool_clients = 0;
	uint64_t pool_orphan_bytes = 0;
	uint64_t pool_orphans = 0;
	uint64_t pool_comp_bytes = 0;
	uint64_t pool_comps = 0;
	uint64_t pool_system_bytes = 0;
	uint64_t pool_systems = 0;
	tgm_pool_breakdown(&pool_clients, &pool_orphan_bytes, &pool_orphans, &pool_comp_bytes,
			   &pool_comps, &pool_system_bytes, &pool_systems);
	scnprintf(
		line, sizeof(line),
		"copy mean %llu us max %llu us (%u); "
		"%u offers idle; %u imports "
		"(%llu/s), %lld KiB live in "
		"%d, %u BoAllocs (%llu KiB, %llu us), pool %lu/%llu MiB (compositor %llu, clients "
		"%llu, orphans %llu in "
		"%llu; compressible %llu MiB in %llu; coherent from system memory %llu MiB in "
		"%llu), %s, MC "
		"%08x "
		"err "
		"%08x at %08x; gpu.lock held by channel setup mean %llu us max %llu us (%u), "
		"teardown mean %llu us max %llu us (%u)",
		copies ? PT_NS(cs) / copies / 1000 : 0, PT_NS(cm) / 1000, copies, idle, imports,
		imports * 1000ULL / (ms ? ms : 1),
		/* live_bytes is signed: an arithmetic shift keeps its sign. */
		/* NOLINTNEXTLINE(bugprone-signed-bitwise) */
		(long long)(__atomic_load_n(&tgm_stats.live_bytes, __ATOMIC_RELAXED) >> 10),
		__atomic_load_n(&tgm_stats.live, __ATOMIC_RELAXED), allocs, abytes >> 10,
		ans / 1000, nvxnu_pool_used_pages() >> (20u - PAGE_SHIFT), nvxnu_pool_size() >> 20,
		__atomic_load_n(&tgm_compositor_pool, __ATOMIC_RELAXED) >> 20, pool_clients >> 20,
		pool_orphan_bytes >> 20, pool_orphans, pool_comp_bytes >> 20, pool_comps,
		pool_system_bytes >> 20, pool_systems, scrubbed, mc_int, mc_err, mc_adr,
		lnew ? lns / lnew / 1000 : 0, lnm / 1000, lnew, lfree ? lfs / lfree / 1000 : 0,
		lfm / 1000, lfree);
	nvxnu_stats("tegra-gm20b: stats2 %s", line);
	{
		const u64 small = __atomic_exchange_n(&tgm_stats.map_small, 0, __ATOMIC_RELAXED);
		const u64 big = __atomic_exchange_n(&tgm_stats.map_big, 0, __ATOMIC_RELAXED);
		const u64 could = __atomic_exchange_n(&tgm_stats.map_could, 0, __ATOMIC_RELAXED);

		if (small || big)
			nvxnu_stats("tegra-gm20b: maps %llu KiB in 128 KiB PTEs, %llu KiB in 4 KiB "
				    "PTEs, %llu KiB of those 128 KiB PTEs could have mapped",
				    big >> 10, small >> 10, could >> 10);
	}
	switch_gpu_path_window(line, sizeof(line));
	nvxnu_stats("tegra-gm20b: paths %s", line);
	switch_gpu_job_window(line, sizeof(line));
	if (line[0])
		nvxnu_stats("tegra-gm20b: jobs %s", line);
	switch_gpu_event_stats_window(line, sizeof(line));
	if (line[0])
		nvxnu_stats("tegra-gm20b: events %s", line);
	stats_exec_owners(line, sizeof(line), rate);
	tgm_stats_mem(ms);
	tgm_stats_waits(ms);
	/* The IOSurface lock/unlock traps' cache maintenance this window (TegraGM20BCoherency.cpp): GPU-written imports
	 * invalidated at a CPU lock, CPU-written imports cleaned at an unlock. */
	{
		uint64_t coh[6];
		tgm_coherency_take_stats(coh);
		if (coh[0] || coh[2] || coh[4] || coh[5]) {
			nvxnu_log(
				"tegra-gm20b: coherency %llu lock invalidations (%llu KiB), "
				"%llu unlock cleans (%llu KiB), %llu locks waited for GPU writes, "
				"%llu AvoidSync locks refused\n",
				coh[0], coh[1] >> 10, coh[2], coh[3] >> 10, coh[4], coh[5]);
		}
	}
	/* IOSurface layouts decided this window (TegraGM20BTiled.cpp), the block-linear ones live, and the CPU views'
	 * scratch: what tiling costs in memory */
	{
		uint64_t t[9];
		tgm_tiled_take_stats(t);
		if (t[0] || t[1] || t[2] || t[3] || t[4] || t[5] || t[7]) {
			nvxnu_log("tegra-gm20b: layouts %llu block-linear, pitch %llu sampled "
				  "first, %llu shared, "
				  "%llu by shape, %llu failed tiling; %llu block-linear live (%llu "
				  "KiB), CPU views' "
				  "scratch %llu KiB (peak %llu KiB)\n",
				  t[0], t[1], t[2], t[3], t[4], t[5], t[6] >> 10, t[7] >> 10,
				  t[8] >> 10);
		}
		uint64_t r[5];
		tgm_tiled_take_relayouts(r);
		if (r[0] || r[1] || r[2] || r[3] || r[4]) {
			nvxnu_log("tegra-gm20b: relayouts %llu tiling moves skipped (nothing had "
				  "written "
				  "the surface), %llu untiles deferred (%llu made at a CPU lock, "
				  "%llu "
				  "never: the memory went), %llu moves made\n",
				  r[0], r[1], r[2], r[3], r[4]);
		}
	}
#undef PT_NS
}
#endif
