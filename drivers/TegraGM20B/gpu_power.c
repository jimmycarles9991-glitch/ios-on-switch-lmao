// SPDX-License-Identifier: MIT
// System sleep and wake, and rail gating.
#include "gpu_internal.h"

/* system sleep (TegraGM20BSleep.h):
 * SC7 powers the SoC off with DRAM in self-refresh: the GPU's registers, falcons, L2 and PTIMER are lost; its memory is
 * kept (pool and nvkm instmem pages: instance blocks, RAMFC, page tables, runlists, GR context buffers, ACR's blob, rings,
 * fence pages). Every nvif object is in the one client m2() made (the user client's connections, the present and the
 * scrubber are its vmm, mem and channel objects), so nouveau's own suspend and resume (nouveau_do_suspend/_resume: the
 * channels idle, nvif_client_suspend, nvif_client_resume) take all of them. Around them:
 *  sleep  the gate shut (submissions and new channels wait for the wake, the present and the scrubber are refused), every
 *         channel's last fence (interrupt-driven, TGM_HANG_NS, else tgm_chan_hung), tgm_gpu.lock taken and held to the wake
 *         (every other nvif call waits), the idle clock off, nvif_client_suspend (each GR context unbound after a
 *         preempt that saves it, each channel blocked, preempted and off its runlist, events blocked), the L2's dirty
 *         lines written back (the GPU writes RAMFC and contexts through it: nvkm's gk20a instmem flushes it before every
 *         CPU look, instmem/gk20a.c), nvkm_device_fini(NVKM_SUSPEND) (what nvkm_udevice_fini would run if m2() held no
 *         device reference), interrupts off, the MC client drained, then nvkm_device_tegra_power_down()'s steps.
 *  wake   nvkm_device_tegra_power_up()'s steps, interrupts on, then the boot's order: M1's PRI ring, timer and GPCPLL,
 *         tgm_clk_raise() (the relock under M1's quiet conditions, before any falcon runs), tgm_m2_device_init() (ACR's HS load
 *         again, which checks the GPU still reports the WPR it was built for; the PMU; every engine with a user, GR's
 *         FECS/GPCCS through the PMU), nvif_client_resume (channels bound and back on their runlists, GR contexts
 *         rebound, events allowed), one fence on a kernel channel, the idle clock, tgm_gpu.lock, the gate. */
static struct {
	enum tgm_sleep_plan plan; /* the last sleep's, until its wake */
	int khz; /* the GPC clock before sleep: its run rate, also when the idle clock had it low */
	int idle_on;
} sc7;

static u64 us_since(u64 start)
{
	return (nvxnu_nanotime() - start) / 1000;
}

static void sleep_gate(void)
{
	u64 start = nvxnu_nanotime();
	u64 end = start + TGM_HANG_NS;
	u32 inside = 0;

	mutex_lock(&tgm_gate_lock);
	tgm_gate_close(&tgm_sc7_gate);
	inside = __atomic_load_n(&tgm_sc7_gate.inside, __ATOMIC_SEQ_CST);
	while (!tgm_gate_drained(&tgm_sc7_gate)) {
		if (!nvxnu_mutex_sleep(tgm_gate_lock.lck, &tgm_sc7_gate.inside, end) &&
		    !tgm_gate_drained(&tgm_sc7_gate)) {
			nvxnu_panic("tegra-gm20b: sleep: %u callers still in the GPU %llu ms after "
				    "its gate shut",
				    __atomic_load_n(&tgm_sc7_gate.inside, __ATOMIC_SEQ_CST),
				    (unsigned long long)(TGM_HANG_NS / 1000000));
		}
	}
	mutex_unlock(&tgm_gate_lock);
	tgm_journal("tegra-gm20b: sleep: submissions held (%u were in the driver): %llu us", inside,
		    us_since(start));
}

#define SLEEP_RECOVERIES 3

static void sleep_fences(void)
{
	struct tgm_chan *chan = NULL;
	u64 start = nvxnu_nanotime();
	u64 end = start + TGM_HANG_NS;
	u64 id = 0;
	u32 seqno = 0;
	u32 chans = 0;
	u32 waits = 0;
	u32 recoveries = 0;
	int busy = 0;

	for (;;) {
		u32 seq = nvxnu_wake_seq();

		busy = 0;
		chans = 0;
		mutex_lock(&tgm_live_lock);
		list_for_each_entry(chan, &tgm_live_chans, live)
		{
			chans++;
			if (busy || READ_ONCE(chan->error) ||
			    (s32)(tgm_chan_completed(chan) - chan->submitted) >= 0)
				continue;
			/* the gate is shut, so no work comes after this: what is stuck is killed, slow work gets more
			 * time, each up to SLEEP_RECOVERIES times */
			if (nvxnu_nanotime() >= end) {
				if (++recoveries > SLEEP_RECOVERIES ||
				    tgm_hang_recover(chan, "system sleep", chan->submitted,
						     nvxnu_nanotime() - start) == TGM_HANG_STUCK) {
					tgm_chan_hung(chan, "system sleep", chan->submitted,
						      nvxnu_nanotime() - start);
				}
				end = nvxnu_nanotime() + TGM_HANG_NS;
			}
			busy = 1;
			id = chan->id;
			seqno = chan->submitted;
			/* the gate is drained, so no owner's submission or wake entry is under way */
			if (switch_gpu_quiet_need(&chan->quiet, seqno))
				tgm_chan_wake_push(chan);
		}
		mutex_unlock(&tgm_live_lock);
		if (!busy)
			break;
		waits++;
		if (!nvxnu_idle(seq, end) && tgm_chan_announced_id(id, seqno))
			nvxnu_wake_due(seq, "fence", seqno);
	}
	tgm_journal("tegra-gm20b: sleep: %u channels idle (%u fence waits): %llu us", chans, waits,
		    us_since(start));
}

static void sleep_idle_off(void)
{
	sc7.khz = nvkm_clk_read(tgm_gpu.device->clk, nv_clk_src_gpc);
	sc7.idle_on = 0;
	if (!tgm_idle.clk)
		return;
	nvxnu_mutex_lock(tgm_idle.clk);
	sc7.idle_on = READ_ONCE(tgm_idle.on);
	WRITE_ONCE(tgm_idle.on, 0);
	if (READ_ONCE(tgm_idle.low)) {
		if (stats_on())
			tgm_idle.low_ns += nvxnu_nanotime() - tgm_idle.low_since;
		WRITE_ONCE(tgm_idle.low, 0);
		sc7.khz = (int)tgm_idle.run_khz;
	}
	nvxnu_mutex_unlock(tgm_idle.clk);
}

static void sleep_nvkm(void)
{
	struct nvkm_device *device = tgm_gpu.device;
	u64 start = TGM_STATS ? nvxnu_nanotime() : 0;
	int ret = nvif_client_suspend(&tgm_gpu.client, false);

	if (ret)
		nvxnu_panic("tegra-gm20b: sleep: nvkm object suspend failed: %d", ret);
	tgm_journal("tegra-gm20b: sleep: nvkm objects suspended (GR contexts saved, channels off "
		    "the runlist): %llu us",
		    us_since(start));
	start = TGM_STATS ? nvxnu_nanotime() : 0;
	tgm_stamp_last = tgm_stamp_monotonic(tgm_stamp_last, nvkm_timer_read(device->timer));
	/* nvgpu_mm_suspend cleans the comptag lines (FLUSH_L2_CLEAN_COMPTAGS) before its L2 flush: the backing store must
	 * hold every line the L2 loses with its power */
	if (device->ltc->num_tags) {
		nvkm_wr32(device, 0x07000c, 0x00000001);
		/* nouveau's nvkm_wait_msec assigns in its loop's condition and widens the timeout. */
		/* NOLINTBEGIN(bugprone-signed-bitwise) */
		/* NOLINTBEGIN(bugprone-assignment-in-if-condition) */
		/* NOLINTBEGIN(bugprone-implicit-widening-of-multiplication-result) */
		if (nvkm_wait_msec(device, 2000, 0x07000c, 0x00000003, 0x00000000) < 0) {
			nvxnu_panic("tegra-gm20b: sleep: comptag clean still busy after 2 s (%08x)",
				    nvkm_rd32(device, 0x07000c));
		}
		/* NOLINTEND(bugprone-implicit-widening-of-multiplication-result) */
		/* NOLINTEND(bugprone-assignment-in-if-condition) */
		/* NOLINTEND(bugprone-signed-bitwise) */
	}
	nvkm_ltc_flush(device->ltc);
	if (nvkm_rd32(device, 0x070010) &
	    0x3) { /* FLUSH_L2_FLUSH_DIRTY: 0 pending, 1 outstanding */
		nvxnu_panic("tegra-gm20b: sleep: L2 flush still busy after 2 s (%08x)",
			    nvkm_rd32(device, 0x070010));
	}
	ret = nvkm_device_fini(device, NVKM_SUSPEND);
	if (ret)
		nvxnu_panic("tegra-gm20b: sleep: nvkm device suspend failed: %d", ret);
	tgm_journal("tegra-gm20b: sleep: L2 written back, nvkm device suspended at %d kHz: %llu us",
		    sc7.khz, us_since(start));
}

static void sleep_power_down(void)
{
	struct nvkm_device_tegra *tdev = tgm_gpu.device->func->tegra(tgm_gpu.device);
	u64 start = TGM_STATS ? nvxnu_nanotime() : 0;
	int drained = 0;
	int ret = 0;

	nvxnu_irq_suspend();
	tgm_recover_suspend();
	drained = tegra_gm20b_mc_flush(1); /* nvgpu's railgate order: the client drained first */
	clk_disable_unprepare(tdev->clk_pwr);
	clk_disable_unprepare(tdev->clk_ref);
	clk_disable_unprepare(tdev->clk);
	udelay(10);
	ret = tdev->vdd ? regulator_disable(tdev->vdd) : 0;
	if (drained)
		nvxnu_log("tegra-gm20b: sleep: the MC's GPU client did not drain (%d)\n", drained);
	if (ret)
		nvxnu_log("tegra-gm20b: sleep: VDD_GPU may still be on (MAX77812 %d)\n", ret);
	tgm_journal("tegra-gm20b: sleep: interrupts off, MC client %s, clocks off, VDD_GPU %s: "
		    "%llu us",
		    drained ? "NOT drained" : "drained", ret ? "MAYBE STILL ON" : "off",
		    us_since(start));
}

/* nvkm_device_tegra_power_up() (engine/device/tegra.c, static there): what M1's nvkm_device_tegra_new() ran. */
static int power_up(struct nvkm_device_tegra *tdev)
{
	int ret = tdev->vdd ? regulator_enable(tdev->vdd) : 0;

	if (!ret)
		ret = clk_prepare_enable(tdev->clk);
	if (!ret)
		ret = clk_prepare_enable(tdev->clk_ref);
	if (!ret)
		ret = clk_prepare_enable(tdev->clk_pwr);
	if (ret)
		return ret;
	clk_set_rate(tdev->clk_pwr, 204000000);
	udelay(10);
	if (!tdev->pdev->dev.pm_domain) {
		reset_control_assert(tdev->rst);
		udelay(10);
		ret = tegra_pmc_powergate_remove_clamping(tdev->pmc, TEGRA_POWERGATE_3D);
		if (ret)
			return ret;
		udelay(10);
		reset_control_deassert(tdev->rst);
		udelay(10);
	}
	return 0;
}

static void wake_power_up(void)
{
	u64 start = TGM_STATS ? nvxnu_nanotime() : 0;
	int ret = power_up(tgm_gpu.device->func->tegra(tgm_gpu.device));
	int uv = 0;

	if (ret) {
		nvxnu_panic("tegra-gm20b: wake: GPU power-up failed: %d (VDD_GPU, clocks, clamp)",
			    ret);
	}
	nvxnu_irq_resume();
	if (TGM_STATS)
		tegra_gm20b_rail_get_uv(&uv);
	tgm_journal("tegra-gm20b: wake: VDD_GPU %d uV, clocks on, out of reset and clamp: %llu us",
		    uv, us_since(start));
}

/* After nvkm_timer_init, which restarts PTIMER from ktime: forward past the last GPU time before the power-down when the
 * GPU's clock had run ahead of ktime, so every time after the wake is after every one before it; then the power-up's clock
 * anchor, before any channel runs. */
static void stamp_reseed(struct nvkm_device *device)
{
	u64 floor = tgm_stamp_reseed(tgm_stamp_last, nvkm_timer_read(device->timer));

	if (floor) {
		device->timer->func->time(device->timer, floor);
		nvkm_timer_alarm_trigger(device->timer);
	}
	tgm_clock_anchor_publish(device);
}

/* M1 after nvkm_device_tegra_new() */
static void wake_m1(void)
{
	struct nvkm_device *device = tgm_gpu.device;
	u64 start = TGM_STATS ? nvxnu_nanotime() : 0;
	u32 boot0 = nvkm_rd32(device, 0x000000);
	u32 ring = 0;
	int ret = 0;

	if (boot0 != tgm_gpu.boot0) {
		nvxnu_panic("tegra-gm20b: wake: BOOT_0 reads %08x after the power-up, %08x before "
			    "sleep",
			    boot0, tgm_gpu.boot0);
	}
	ret = nvkm_subdev_init(device->privring);
	if (!ret)
		ret = nvkm_subdev_init(&device->timer->subdev);
	if (!ret)
		stamp_reseed(device);
	ring = nvkm_rd32(device,
			 0x120050); /* PRI_RINGMASTER_START_RESULTS, bit 0 = connectivity pass */
	if (!ret)
		ret = nvkm_subdev_init(&device->clk->subdev);
	if (!ret) {
		/* nouveau's nvkm_mask computes in int and declares two locals. */
		/* NOLINTNEXTLINE(bugprone-signed-bitwise,readability-isolate-declaration) */
		nvkm_mask(device, 0x137250, 0x0000003f, 0x00000000); /* M1's 1:1 bypass divider */
	}
	if (ret || badf(ring) || !(ring & 1)) {
		nvxnu_panic("tegra-gm20b: wake: PRI ring %08x, timer or GPCPLL init failed: %d",
			    ring, ret);
	}
	tgm_journal("tegra-gm20b: wake: BOOT_0 %08x, PRI ring %08x, timer, GPCPLL_CFG %08x at %d "
		    "kHz: %llu us",
		    boot0, ring, nvkm_rd32(device, 0x137000),
		    nvkm_clk_read(device->clk, nv_clk_src_gpc), us_since(start));
}

static void wake_clock(void)
{
	struct nvkm_device *device = tgm_gpu.device;
	u64 start = TGM_STATS ? nvxnu_nanotime() : 0;
	char line[200] = "";
	int ret = tgm_clk_raise(line, sizeof(line));
	int khz = 0;

	if (!ret && sc7.idle_on)
		ret = tgm_clock_ceiling_restore();
	khz = nvkm_clk_read(device->clk, nv_clk_src_gpc);
	if (ret || khz < sc7.khz) {
		nvxnu_panic("tegra-gm20b: wake: GPU clock %d kHz, %d kHz before sleep (GPCPLL_CFG "
			    "%08x COEFF %08x)%s%s",
			    khz, sc7.khz, nvkm_rd32(device, 0x137000), nvkm_rd32(device, 0x137004),
			    line[0] ? ": " : "", line);
	}
	tgm_journal("tegra-gm20b: wake: GPU clock %d kHz (GPCPLL COEFF %08x): %llu us", khz,
		    nvkm_rd32(device, 0x137004), us_since(start));
}

static void wake_device(void)
{
	struct nvkm_device *device = tgm_gpu.device;
	struct nvkm_acr *acr = device->acr;
	u64 start = TGM_STATS ? nvxnu_nanotime() : 0;
	u64 lo = 0;
	u64 hi = 0;
	const char *at = "";
	u32 mc[5];
	int ret = tgm_m2_device_init(device, &at);

	if (ret || !acr || !acr->done) {
		/* gm200_acr_wpr_check(): the WPR the GPU reports now; ACR built its image for acr->wpr_start/_end */
		nvkm_wr32(device, 0x100cd4, 2);
		lo = (u64)(nvkm_rd32(device, 0x100cd4) & 0xffffff00) << 8;
		nvkm_wr32(device, 0x100cd4, 3);
		hi = ((u64)(nvkm_rd32(device, 0x100cd4) & 0xffffff00) << 8) + 0x20000;
		tegra_gm20b_mc_gpu_wpr(mc);
		nvxnu_panic(
			"tegra-gm20b: wake: nvkm init failed at %s: %d, ACR %s; the GPU reports "
			"WPR %llx-%llx, ACR's image is for %llx-%llx; MC carveout 2 CFG0 %08x "
			"BOM %08x SIZE %u x 128 KiB CLIENT_ACCESS2 %08x CLIENT_ACCESS4 %08x",
			at, ret, acr && acr->done ? "loaded" : "NOT loaded", lo, hi,
			acr ? acr->wpr_start : 0, acr ? acr->wpr_end : 0, mc[0], mc[1], mc[2],
			mc[3], mc[4]);
	}
	tgm_journal("tegra-gm20b: wake: nvkm device up (ACR loaded, WPR %llx-%llx; PMU; GR "
		    "FECS/GPCCS): %llu us",
		    acr->wpr_start, acr->wpr_end, us_since(start));
}

static void wake_nvkm(void)
{
	struct tgm_chan *chan = NULL;
	u64 start = TGM_STATS ? nvxnu_nanotime() : 0;
	u32 chans = 0;
	u32 dead = 0;
	int ret = nvif_client_resume(&tgm_gpu.client);

	if (ret)
		nvxnu_panic("tegra-gm20b: wake: nvkm object resume failed: %d", ret);
	if (TGM_STATS) {
		mutex_lock(&tgm_live_lock);
		list_for_each_entry(chan, &tgm_live_chans, live)
		{
			chans++;
			dead += READ_ONCE(chan->error) != 0;
		}
		mutex_unlock(&tgm_live_lock);
	}

	tgm_journal("tegra-gm20b: wake: nvkm objects resumed (%u channels back on the runlist, %u "
		    "dead ones left off): %llu us",
		    chans - dead, dead, us_since(start));
}

/* One kernel fence through the host, PBDMA, semaphore release and nonstall interrupt before any user work. The scrubber's
 * channel when there is one: no caller waiting at the gate holds tgm_scrub.lock (tgm_present.lock is held around a channel
 * creation, which waits at the gate, while tgm_present.chan is still NULL). */
static void wake_fence(void)
{
	struct tgm_chan *chan = tgm_scrub.chan ? tgm_scrub.chan : tgm_present.chan;
	struct mutex *lock = tgm_scrub.chan ? &tgm_scrub.lock : &tgm_present.lock;
	u64 start = nvxnu_nanotime();
	u64 end = start + TGM_PRESENT_NS;
	u32 seqno = 0;
	int ret = 0;

	if (!chan || READ_ONCE(chan->error)) {
		tgm_journal("tegra-gm20b: wake: no kernel channel to test the GPU on");
		return;
	}
	mutex_lock(lock);
	ret = chan_push_wait(chan, CHAN_FENCE_DWORDS);
	if (ret) {
		nvxnu_panic("tegra-gm20b: wake: %s channel push buffer unavailable: %d",
			    chan->owner, ret);
	}
	seqno = chan_fence(chan);
	mutex_unlock(lock);
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if ((s32)(tgm_chan_completed(chan) - seqno) >= 0)
			break;
		if (nvxnu_nanotime() >= end)
			tgm_chan_hung(chan, "wake: kernel fence", seqno, nvxnu_nanotime() - start);
		if (!nvxnu_idle(seq, end) && (s32)(tgm_chan_completed(chan) - seqno) >= 0)
			nvxnu_wake_due(seq, "fence", seqno);
	}
	tgm_journal("tegra-gm20b: wake: %s channel fence %u done: %llu us", chan->owner, seqno,
		    us_since(start));
}

/* The idle clock back and the gate open, tgm_gpu.lock still held (system sleep's wake_open releases it first). */
static const char *wake_open_locked(void)
{
	const char *clock = "not in use";

	if (tgm_idle.clk && sc7.idle_on) {
		nvxnu_mutex_lock(tgm_idle.clk);
		if (((nvkm_rd32(tgm_gpu.device, GPCPLL_COEFF_R) >> 16) & 0x3f) == tgm_idle.run_pl) {
			tgm_idle.log_start = nvxnu_nanotime();
			__atomic_store_n(&tgm_idle.last_busy, tgm_idle.log_start, __ATOMIC_SEQ_CST);
			WRITE_ONCE(tgm_idle.on, 1);
			clock = "on";
		} else {
			clock = "OFF for this boot";
			nvxnu_log("tegra-gm20b: wake: GPCPLL COEFF %08x is not the idle clock's PL "
				  "%u: idle clock off, GPU at %d kHz\n",
				  nvkm_rd32(tgm_gpu.device, GPCPLL_COEFF_R), tgm_idle.run_pl,
				  sc7.khz);
		}
		nvxnu_mutex_unlock(tgm_idle.clk);
	}
	return clock;
}

static void gate_open(void)
{
	mutex_lock(&tgm_gate_lock);
	tgm_gate_open(&tgm_sc7_gate);
	nvxnu_wakeup(&tgm_sc7_gate.shut);
	mutex_unlock(&tgm_gate_lock);
	scrub_kick();
	tgm_present_redo_kick();
}

static void wake_open(void)
{
	u64 start = nvxnu_nanotime();
	const char *clock = wake_open_locked();

	mutex_unlock(&tgm_gpu.lock);
	gate_open();
	tgm_journal("tegra-gm20b: wake: submissions open, idle clock %s: %llu us", clock,
		    us_since(start));
}

void tgm_sleep(void)
{
	sc7.plan = tgm_sleep_plan(tgm_gpu.stage, tgm_gpu.device != NULL);
	switch (sc7.plan) {
	case TGM_SLEEP_NOTHING:
		return;
	case TGM_SLEEP_POWER:
		switch_sleep_mark(SWITCH_STEP_GPU_POWER_DOWN);
		sleep_power_down();
		switch_sleep_mark(SWITCH_STEP_GPU_DOWN);
		tgm_journal("tegra-gm20b: sleep: no nvkm client (bring-up stopped after stage %d): "
			    "powered down",
			    tgm_gpu.stage);
		return;
	case TGM_SLEEP_FULL:
		break;
	}
	switch_sleep_mark(SWITCH_STEP_GPU_GATE);
	sleep_gate();
	tgm_deadline_suspend();
	switch_sleep_mark(SWITCH_STEP_GPU_FENCES);
	sleep_fences();
	mutex_lock(&tgm_gpu.lock);
	switch_sleep_mark(SWITCH_STEP_GPU_IDLE_OFF);
	sleep_idle_off();
	switch_sleep_mark(SWITCH_STEP_GPU_NVKM);
	sleep_nvkm();
	switch_sleep_mark(SWITCH_STEP_GPU_POWER_DOWN);
	sleep_power_down();
	switch_sleep_mark(SWITCH_STEP_GPU_DOWN);
}

void tgm_wake(void)
{
	enum tgm_sleep_plan plan = sc7.plan;

	sc7.plan = TGM_SLEEP_NOTHING;
	switch (plan) {
	case TGM_SLEEP_NOTHING:
		return;
	case TGM_SLEEP_POWER:
		tgm_journal("tegra-gm20b: wake: GPU left off (its bring-up stopped after stage %d)",
			    tgm_gpu.stage);
		return;
	case TGM_SLEEP_FULL:
		break;
	}
	wake_power_up();
	wake_m1();
	wake_clock();
	wake_device();
	tgm_recover_arm(tgm_gpu.device);
	wake_nvkm();
	/* before the gate opens: a job queued from then on is the hang rule's again */
	tgm_promise_resume();
	wake_fence();
	tgm_clkgate_enable(tgm_gpu.device);
	wake_open();
	tgm_deadline_resume();
}

/* Rail-gating (TegraGM20BRail.h), from TegraGM20B.cpp's rail worker: system sleep's sleep steps, its durable trail
 * left alone, then tgm_gpu.lock released with the GPU marked down and the gate left shut. 0 (down), or -EBUSY for a GPU
 * without an nvkm client (it stays as it is). */
int tgm_rail_down(void)
{
	u64 start = TGM_STATS ? nvxnu_nanotime() : 0;

	if (__atomic_load_n(&tgm_gpu.railed, __ATOMIC_SEQ_CST))
		return 0;
	if (tgm_sleep_plan(tgm_gpu.stage, tgm_gpu.device != NULL) != TGM_SLEEP_FULL)
		return -EBUSY;
	sleep_gate();
	tgm_deadline_suspend();
	sleep_fences();
	mutex_lock(&tgm_gpu.lock);
	sleep_idle_off();
	sleep_nvkm();
	sleep_power_down();
	__atomic_store_n(&tgm_gpu.railed, 1, __ATOMIC_SEQ_CST);
	mutex_unlock(&tgm_gpu.lock);
	/* callers waiting at the gate now wake the GPU themselves (tgm_gate_enter) */
	mutex_lock(&tgm_gate_lock);
	nvxnu_wakeup(&tgm_sc7_gate.shut);
	mutex_unlock(&tgm_gate_lock);
	tgm_journal("tegra-gm20b: rail down in %llu us", us_since(start));
	return 0;
}

/* The caller holds tgm_gpu.lock and finds the GPU rail-gated: system sleep's wake steps under that lock, and the gate open.
 * Not wake_fence: its kernel channel's lock may be the caller's (tgm_present.lock around a channel creation), and the
 * caller's own work is the GPU's next test. `site` is the caller's return address, NULL for the display. */
void tgm_rail_up_locked(const void *site)
{
	u64 start = TGM_STATS ? nvxnu_nanotime() : 0;
	const char *clock = NULL;

	tgm_rail_moving();
	wake_power_up();
	wake_m1();
	wake_clock();
	wake_device();
	tgm_recover_arm(tgm_gpu.device);
	wake_nvkm();
	tgm_clkgate_enable(tgm_gpu.device);
	clock = wake_open_locked();
	tgm_promise_resume();
	/* The gate opens before railed clears: a closer acts only once it has seen the GPU up (tgm_rail_down, the worker's
	 * tgm_railed(), SwitchSuspend's tgm_rail_sc7_hold), so its close can never land before this open and be undone. */
	gate_open();
	__atomic_store_n(&tgm_gpu.railed, 0, __ATOMIC_SEQ_CST);
	tgm_deadline_resume();
	if (TGM_STATS) {
		if (site) {
			if (!tgm_rail_ups.demand++)
				tgm_rail_ups.first_site = site;
		} else {
			tgm_rail_ups.display++;
		}
	}

	/* the first site as an offset from tgm_stage: KASLR-free, resolved against the kernel's symbols */
	tgm_journal(
		"tegra-gm20b: rail up in %llu us, idle clock %s (rail: %u up (display %u, "
		"demand %u by "
		"tgm_stage%+ld); %llu KiB zeroed on the CPU while down)",
		us_since(start), clock, tgm_rail_ups.display + tgm_rail_ups.demand,
		tgm_rail_ups.display, tgm_rail_ups.demand,
		tgm_rail_ups.first_site
			? (long)((unsigned long)tgm_rail_ups.first_site - (unsigned long)tgm_stage)
			: 0L,
		(unsigned long long)(__atomic_load_n(&tgm_rail_ups.cpu_zeroed, __ATOMIC_RELAXED) >>
				     10));
	tgm_rail_moved(site != NULL);
}

/* The rail worker, for the display coming on: the GPU up if it is rail-gated, on this thread. */
void tgm_rail_up(void)
{
	mutex_lock(&tgm_gpu.lock);
	if (tgm_gpu.railed)
		tgm_rail_up_locked(NULL);
	mutex_unlock(&tgm_gpu.lock);
}

int tgm_railed(void)
{
	return __atomic_load_n(&tgm_gpu.railed, __ATOMIC_SEQ_CST);
}

/* System sleep with the GPU rail-gated, on SwitchSuspend's one thread: tgm_gpu.lock held from its suspend to its resume, as
 * tgm_sleep holds it, so no caller wakes the GPU in between; whoever needs it after the resume wakes it then. 1 when
 * held (the GPU was down), 0 when the caller must take system sleep's own path (tgm_sleep). */
int tgm_rail_sc7_hold(void)
{
	mutex_lock(&tgm_gpu.lock);
	if (tgm_gpu.railed)
		return 1;
	mutex_unlock(&tgm_gpu.lock);
	return 0;
}

void tgm_rail_sc7_release(void)
{
	mutex_unlock(&tgm_gpu.lock);
}
