// SPDX-License-Identifier: MIT
// GPU load from the PMU's idle counters (TegraGM20BLoad.h), debug kernels: the read the display takes at each frame end,
// the submissions' read when nothing else read for a second, and the stats line's figure for its window.
#include "gpu_internal.h"
#include "gpu_inline.h"
#include "TegraGM20BLoad.h"

#if TGM_STATS

/* The display's frame end, the submissions and the stats line all read, and each read extends the same 31-bit counters:
 * a read and its step are one under this lock, three register reads long, never a wait. */
static DEFINE_SPINLOCK(load_lock);
static struct tgm_load load;
static struct tgm_load load_reported; /* the stats line's last window: its one caller's own */
static int
	load_off; /* the program did not hold: no more reads (load_lock; read unlocked to skip the gate) */
static u32 load_mask_after; /* the busy mask read back when it did not hold */

static void load_arm(struct nvkm_device *device)
{
	nvkm_wr32(
		device, TGM_LOAD_CTRL(TGM_LOAD_BUSY),
		tgm_load_ctrl(nvkm_rd32(device, TGM_LOAD_CTRL(TGM_LOAD_BUSY)), TGM_LOAD_CTRL_BUSY));
	nvkm_wr32(device, TGM_LOAD_CTRL(TGM_LOAD_ALWAYS),
		  tgm_load_ctrl(nvkm_rd32(device, TGM_LOAD_CTRL(TGM_LOAD_ALWAYS)),
				TGM_LOAD_CTRL_ALWAYS));
	nvkm_wr32(device, TGM_LOAD_COUNT(TGM_LOAD_BUSY), TGM_LOAD_COUNT_RESET);
	nvkm_wr32(device, TGM_LOAD_COUNT(TGM_LOAD_ALWAYS), TGM_LOAD_COUNT_RESET);
	nvkm_wr32(device, TGM_LOAD_MASK(TGM_LOAD_BUSY), TGM_LOAD_GR_CE2);
}

/* Inside the sleep gate, load_lock held. -1 when the program just written did not hold. */
static int load_read(struct nvkm_device *device)
{
	const u64 start = nvxnu_nanotime();
	const int fresh = !tgm_load_armed(nvkm_rd32(device, TGM_LOAD_MASK(TGM_LOAD_BUSY)));
	u32 busy = 0;
	u32 total = 0;

	if (fresh) {
		load_arm(device);
		load_mask_after = nvkm_rd32(device, TGM_LOAD_MASK(TGM_LOAD_BUSY));
		if (!tgm_load_armed(load_mask_after)) {
			WRITE_ONCE(load_off, 1);
			return -1;
		}
	}
	busy = nvkm_rd32(device, TGM_LOAD_COUNT(TGM_LOAD_BUSY));
	total = nvkm_rd32(device, TGM_LOAD_COUNT(TGM_LOAD_ALWAYS));
	tgm_load_step(&load, nvxnu_nanotime(), busy, total, fresh);
	load.read_ns += nvxnu_nanotime() - start;
	return 0;
}

/* In the gate (1), or the GPU is down, not brought up yet, or the probe is off (0). */
static int load_enter(void)
{
	return READ_ONCE(tgm_gpu.stage) >= 3 && !READ_ONCE(load_off) && gate_try();
}

/* In the gate: one read, and the gate left. -1 when the program did not hold (one line, the probe off). */
static int load_take(uint64_t *busy, uint64_t *total)
{
	unsigned long flags = 0;
	int ret = 0;
	u32 mask = 0;

	spin_lock_irqsave(&load_lock, flags);
	ret = load_read(tgm_gpu.device);
	mask = load_mask_after;
	*busy = load.busy;
	*total = load.total;
	spin_unlock_irqrestore(&load_lock, flags);
	gate_leave();
	if (ret)
		nvxnu_log(
			"tegra-gm20b: PMU idle counter 1's mask reads %08x after %08x was written: "
			"the load probe is off\n",
			mask, TGM_LOAD_GR_CE2);
	return ret;
}

int tegra_gm20b_gpu_load(uint64_t *busy, uint64_t *total)
{
	uint64_t b = 0;
	uint64_t t = 0;

	if (!load_enter() || load_take(&b, &t))
		return -1;
	*busy = b;
	*total = t;
	return 0;
}

/* Each submission (gpu_stats_tick.c): a read when none came for TGM_LOAD_SAMPLE_NS, so the stats line counts with the
 * display's frame ends masked or its caller absent. The unlocked look at the last read only decides whether to read. */
void tgm_load_sample(uint64_t now_ns)
{
	uint64_t b = 0;
	uint64_t t = 0;

	if (!tgm_load_due(__atomic_load_n(&load.last_ns, __ATOMIC_RELAXED), now_ns) ||
	    !load_enter())
		return;
	(void)load_take(&b, &t);
}

/* The stats line's window (gpu_stats_tick.c, its one caller at a time): the busy share of the counted cycles, how much
 * of the window was counted, counter 2's rate and what the reads cost. */
void tgm_load_stats(char *buf, unsigned int len, uint64_t window_ns)
{
	struct tgm_load now;
	struct tgm_load_window w;
	unsigned long flags = 0;
	uint64_t b = 0;
	uint64_t t = 0;
	const int up = load_enter();

	if (up)
		(void)load_take(&b, &t);
	spin_lock_irqsave(&load_lock, flags);
	now = load;
	spin_unlock_irqrestore(&load_lock, flags);
	w = tgm_load_between(&load_reported, &now, window_ns);
	load_reported = now;
	if (w.busy_permille < 0) {
		scnprintf(buf, len, "PMU busy - (no cycles counted: %u reads, %u restarts%s)",
			  w.reads, w.restarts,
			  READ_ONCE(load_off) ? ", probe off"
			  : up                ? ""
					      : ", GPU down at the line");
		return;
	}
	scnprintf(buf, len,
		  "PMU busy %d.%d%% over %d.%d%% of the window "
		  "(counter at %llu MHz; %u reads, %u restarts, %llu ns each)",
		  w.busy_permille / 10, w.busy_permille % 10, w.counted_permille / 10,
		  w.counted_permille % 10, w.rate_khz / 1000, w.reads, w.restarts, w.read_ns_each);
}

#endif
