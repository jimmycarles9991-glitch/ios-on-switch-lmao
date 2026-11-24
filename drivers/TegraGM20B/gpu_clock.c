// SPDX-License-Identifier: MIT
// GPCPLL programming and the idle and performance policy that sets the GPU clock.
#include "gpu_internal.h"
#include "TegraGM20BNapll.h"

#define FIXED_PLL_MARGIN_PCT 20
#define GPU_NAPLL_TARGET_KHZ 921600u
#define GPU_FIXED_TARGET_KHZ 768000u
#define BOOT_KHZ 76800

static struct tgm_napll_state tgm_napll;

static uint32_t napll_rd32(void *arg, uint32_t reg)
{
	return nvkm_rd32((struct nvkm_device *)arg, reg);
}

static void napll_wr32(void *arg, uint32_t reg, uint32_t val)
{
	nvkm_wr32((struct nvkm_device *)arg, reg, val);
}

static void napll_udelay(void *arg, uint32_t us)
{
	(void)arg;
	udelay(us);
}

static struct tgm_napll_io napll_io(struct nvkm_device *device)
{
	struct tgm_napll_io io = {device, napll_rd32, napll_wr32, napll_udelay};

	return io;
}

static int clk_prog_fixed_khz(struct nvkm_device *device, u32 khz)
{
	struct nvkm_clk *clk = device->clk;
	struct nvkm_cstate cstate = {.domain[nv_clk_src_gpc] = khz};
	int ret = clk->func->calc(clk, &cstate);

	if (!ret) {
		ret = clk->func->prog(clk);
		clk->func->tidy(clk);
	}
	return ret;
}

static int clk_prog_khz(struct nvkm_device *device, u32 khz)
{
	if (tgm_napll.active) {
		struct tgm_napll_io io = napll_io(device);

		if (tgm_idle.vco_coeff && tgm_idle.vco_khz && khz &&
		    !(tgm_idle.vco_khz % (2u * khz))) {
			u32 m = tgm_idle.vco_coeff & 0xffu;
			u32 n = (tgm_idle.vco_coeff >> 8) & 0xffu;
			u32 pl = tgm_idle.vco_khz / (2u * khz);

			if (m == 1u && pl >= tgm_napll.params.min_pl &&
			    pl <= tgm_napll.params.max_pl)
				return tgm_napll_reprogram_mnp(&io, &tgm_napll, m, n, pl);
		}
		return tgm_napll_reprogram(&io, &tgm_napll, khz);
	}
	return clk_prog_fixed_khz(device, khz);
}

void tgm_clk_suspend(struct nvkm_device *device)
{
	struct tgm_napll_io io = napll_io(device);

	tgm_napll_suspend(&io, &tgm_napll);
}

/* One CLK trace line after a PLL change: GPCPLL state and, over the PRI ring, whether the ring survived (as M1 reads
 * it). Returns 1 if the GPU runs from a locked GPCPLL at `khz` with the 1:1 bypass divider and a live ring. */
static int clk_state(struct nvkm_device *device, const char *what, int ret, u32 khz)
{
	u32 cfg = nvkm_rd32(device, TGM_GPCPLL_CFG_R);
	u32 coeff = nvkm_rd32(device, TGM_GPCPLL_COEFF_R);
	u32 dvfs1 = nvkm_rd32(device, TGM_GPCPLL_DVFS1_R);
	u32 vco = nvkm_rd32(device, TGM_SEL_VCO_R);
	u32 out = nvkm_rd32(device, TGM_GPC2CLK_OUT_R);
	u32 ring = nvkm_rd32(device, 0x120050);
	u32 bus = nvkm_rd32(device, 0x001100);
	int now = nvkm_clk_read(device->clk, nv_clk_src_gpc);

	nvxnu_step("tegra-gm20b: CLK step: %s, ret %d: GPCPLL_CFG %08x COEFF %08x DVFS1 %08x "
		   "(M %u N %u PL %u) SEL_VCO %x GPC2CLK_OUT %08x = %d kHz; ring %08x "
		   "BUS_INTR %08x\n",
		   what, ret, cfg, coeff, dvfs1, coeff & 0xff, (coeff >> 8) & 0xff,
		   (coeff >> 16) & 0x3f, vco, out, now, ring, bus);
	return !ret && tgm_napll_cfg_running(cfg, dvfs1) && (vco & 1) && !(out & 0x3f3f) &&
	       now == (int)khz && !badf(ring) && (ring & 1) && !(bus & 0x8);
}

int tgm_clk_raise(char *line, unsigned int len)
{
	struct nvkm_device *device = tgm_gpu.device;
	u32 chipset = (tgm_gpu.boot0 >> 20) & 0x1ffu;
	int speedo = tegra_sku_info.gpu_speedo_value;
	int ceil_uv = tegra_gm20b_rail_max_uv();
	int uv = 0;
	int ret = 0;
	int back = 0;
	u32 na_khz = 0;
	u32 na_best = 0;
	u32 fixed_khz = 0;
	u32 fixed_best = 0;
	u32 target_khz = 0;
	struct tgm_napll_params params = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
	int try_napll = 0;

	tgm_napll.active = 0;
	na_khz = tgm_napll_select_rate_khz(GPU_NAPLL_TARGET_KHZ, ceil_uv, 0, tegra_gm20b_dvfs_khz,
					   tegra_gm20b_dvfs_na_uv, speedo, &na_best);
	fixed_khz = tgm_napll_select_rate_khz(GPU_FIXED_TARGET_KHZ, ceil_uv, FIXED_PLL_MARGIN_PCT,
					      tegra_gm20b_dvfs_khz, tegra_gm20b_dvfs_na_uv, speedo,
					      &fixed_best);
	/* platform.c carries T210B01 16nm CVB and 800 mV ceiling; gate NAPLL to C1 until B1 has its own. */
	try_napll = chipset == TGM_CHIPSET_GM20B_C1 && na_khz > (u32)BOOT_KHZ &&
		    tgm_napll_params_for_chipset(chipset, &params) == 0;
	target_khz = try_napll ? na_khz : fixed_khz;

	if (target_khz <= (u32)BOOT_KHZ) {
		nvxnu_log("tegra-gm20b: no GPU rate above %u kHz fits %d uV ceiling (speedo %d): "
			  "GPU stays at %d kHz\n",
			  (u32)BOOT_KHZ, ceil_uv, speedo,
			  nvkm_clk_read(device->clk, nv_clk_src_gpc));
		return 0;
	}
	if (!try_napll && nvkm_clk_read(device->clk, nv_clk_src_gpc) == (int)target_khz)
		return 0;

	ret = tegra_gm20b_rail_get_uv(&uv);
	nvxnu_step("tegra-gm20b: CLK step: chip %03x speedo %d ceil %d uV (NA %u/%u kHz, "
		   "fixed +%d%% %u/%u kHz); VDD_GPU %d uV (ret %d)\n",
		   chipset, speedo, ceil_uv, na_khz, na_best, FIXED_PLL_MARGIN_PCT, fixed_khz,
		   fixed_best, uv, ret);

	/* Converge rail to ceil_uv before raising the clock; rail_set_uv clamps to GPU_UV_CEIL. */
	if (!ret && uv != ceil_uv) {
		ret = tegra_gm20b_rail_set_uv(ceil_uv);
		if (!ret)
			ret = tegra_gm20b_rail_get_uv(&uv);
		nvxnu_step("tegra-gm20b: CLK step: VDD_GPU set to %d uV (ret %d) before the "
			   "clock\n",
			   uv, ret);
	}
	if (ret || uv != ceil_uv) {
		nvxnu_log("tegra-gm20b: %u kHz: VDD_GPU %d uV (ret %d), %d uV required; GPU stays "
			  "at %d kHz\n",
			  target_khz, uv, ret, ceil_uv, nvkm_clk_read(device->clk, nv_clk_src_gpc));
		return 0;
	}

	if (try_napll) {
		struct tgm_napll_io io = napll_io(device);
		u32 calib0 = tegra_gm20b_fuse_read(TGM_FUSE_RESERVED_CALIB0_OFF);

		ret = tgm_napll_start(&io, chipset, calib0, uv, TGM_NAPLL_REF_KHZ, na_khz,
				      &tgm_napll);
		if (clk_state(device, "raised NAPLL", ret, na_khz)) {
			u32 coeff = nvkm_rd32(device, TGM_GPCPLL_COEFF_R);
			nvxnu_log("tegra-gm20b: GPU clock %d kHz (NAPLL %s M %u N %u PL %u, "
				  "coeff %u ext_cal %d), VDD_GPU %d uV\n",
				  nvkm_clk_read(device->clk, nv_clk_src_gpc),
				  tgm_napll.fused ? "fused" : "self-cal", coeff & 0xff,
				  (coeff >> 8) & 0xff, (coeff >> 16) & 0x3f,
				  tgm_napll.dvfs.dfs_coeff, tgm_napll.dvfs.dfs_ext_cal, uv);
			return 0;
		}
		tgm_napll_clear_dvfs(&io);
		tgm_napll.active = 0;
		nvxnu_log("tegra-gm20b: NAPLL %u kHz failed (ret %d); switching to fixed PLL %u "
			  "kHz\n",
			  na_khz, ret, fixed_khz);
	}

	if (fixed_khz > (u32)BOOT_KHZ) {
		ret = clk_prog_fixed_khz(device, fixed_khz);
		if (clk_state(device, "raised fixed", ret, fixed_khz)) {
			u32 coeff = nvkm_rd32(device, TGM_GPCPLL_COEFF_R);
			nvxnu_log("tegra-gm20b: GPU clock %d kHz (GPCPLL M %u N %u PL %u), "
				  "VDD_GPU %d uV\n",
				  nvkm_clk_read(device->clk, nv_clk_src_gpc), coeff & 0xff,
				  (coeff >> 8) & 0xff, (coeff >> 16) & 0x3f, uv);
			return 0;
		}
	}

	back = clk_prog_fixed_khz(device, (u32)BOOT_KHZ);
	if (clk_state(device, "back to the boot rate", back, (u32)BOOT_KHZ)) {
		nvxnu_log("tegra-gm20b: GPCPLL did not settle at %u kHz (ret %d); back at %u "
			  "kHz\n",
			  target_khz, ret, (u32)BOOT_KHZ);
		return 0;
	}
	scnprintf(line, len,
		  "M2 FAIL GPCPLL to %u kHz (ret %d) and back to %u kHz (ret %d): GPCPLL_CFG %08x "
		  "COEFF %08x SEL_VCO %x",
		  target_khz, ret, (u32)BOOT_KHZ, back, nvkm_rd32(device, TGM_GPCPLL_CFG_R),
		  nvkm_rd32(device, TGM_GPCPLL_COEFF_R), nvkm_rd32(device, TGM_SEL_VCO_R));
	return -EIO;
}

/* idle clock: the GPCPLL's output divider, so the GPU downclocks only where that costs no performance.
 * The GPU runs at GPU_MHZ (768 MHz, Horizon's docked maximum) whenever there is any GPU work, and drops
 * to 76.8 MHz (L4T's lowest B01 row, Horizon's own boost-mode GPU clock) only once it has been truly idle:
 *   * down: no submission kicked, nothing in flight and no display swap for IDLE_DROP_NS (500 ms), checked on the
 *     CPU governor's tick (SwitchPerfGovernor.cpp calls tegra_gm20b_idle_tick from XNU's perfcontrol deadline, so
 *     the GPU adds no timer of its own; without the CPU governor the GPU simply never drops). A swap arriving means
 *     Core Animation is animating, so the clock never drops during an animation, even one that submits little.
 *   * up: synchronously, before the submission's work is kicked (tgm_chan_exec, tgm_present_copy_dependencies, and chan_fence as
 *     a backstop), so no GPU work ever starts at the idle clock; also at once (a work item) on switch_perf_hint's
 *     touch, missed-frame and launch hints, so the clock is usually back before the frame's first submission.
 * The change is the output divider alone, VCO left running and locked (1536 MHz: M 1, N 40 from the 38.4 MHz
 * reference), PL 1 <-> 10. GM20B's GPCPLL changes PL without a glitch (nvgpu clk_gm20b.c: PLDIV_GLITCHLESS; nouveau
 * gm20b.c gm20b_pllg_program_mnp's pdiv_only path, whose sequence this is: VCO divider 1:2 around it, an interim PL
 * sharing a bit with both ends). No NDIV slide, no relock, no bypass, no VDD_GPU write (the rail stays at tgm_clk_raise's
 * 800 mV): a few register writes and two 2 us delays, timed on every raise and logged (raise mean/max).
 * Before first use, gov_start runs one drop and raise at stage 3 (no client yet) and logs both times. A failure (the
 * divider not reading back, the PLL not locked, the clock not at the rate) logs the PLL's registers, puts the run rate
 * back and leaves the idle clock in use for the next idle period (SwitchGpuClockControl.h idle_fail). */
#define IDLE_DROP_NS (500ULL * 1000000ULL)
#define GPC2CLK_OUT_R 0x137250 /* VCO divider 13:8: 0 = 1:1, 2 = 1:2 */
#define SEL_VCO_R 0x137100     /* 0 = GPU on bypass, 1 = on GPCPLL */
#define PLL_REF_KHZ 38400

extern uint64_t
native_display_last_swap_ns(void); /* NativeLegacyDisplay.cpp: validateSwap's last call, 0: none */
struct idle_state tgm_idle;

#include "SwitchGpuIdleDeadline.h"

/* GM20B's glitchless PL change (nouveau gm20b.c gm20b_pllg_program_mnp, pdiv_only): returns 0 when the GPCPLL is
 * locked, on the VCO, at PL `pl`. Under tgm_idle.clk. */
static int pl_switch(u32 pl)
{
	struct nvkm_device *device = tgm_gpu.device;
	u32 coeff = nvkm_rd32(device, GPCPLL_COEFF_R);
	u32 old = (coeff >> 16) & 0x3f;
	u32 back = 0;

	if (old != pl) {
		/* the VCO divider at 1:2 around the change, written twice (linear divider operation) */
		/* nouveau's nvkm_mask computes in int and declares two locals. */
		/* NOLINTNEXTLINE(bugprone-signed-bitwise,readability-isolate-declaration) */
		nvkm_mask(device, GPC2CLK_OUT_R, 0x3f00, 2 << 8);
		/* nouveau's nvkm_mask computes in int and declares two locals. */
		/* NOLINTNEXTLINE(bugprone-signed-bitwise,readability-isolate-declaration) */
		nvkm_mask(device, GPC2CLK_OUT_R, 0x3f00, 2 << 8);
		nvkm_rd32(device, GPC2CLK_OUT_R);
		udelay(2);
		if (!(old & pl)) { /* an interim PL that shares a set bit with both ends */
			u32 a = old | (1u << (unsigned int)(__builtin_ffs((int)pl) - 1));
			u32 b = pl | (1u << (unsigned int)(__builtin_ffs((int)old) - 1));
			nvkm_wr32(device, GPCPLL_COEFF_R,
				  (coeff & ~(0x3fu << 16)) | ((a < b ? a : b) << 16));
		}
		nvkm_wr32(device, GPCPLL_COEFF_R, (coeff & ~(0x3fu << 16)) | (pl << 16));
		udelay(2);
		/* nouveau's nvkm_mask computes in int and declares two locals. */
		/* NOLINTNEXTLINE(bugprone-signed-bitwise,readability-isolate-declaration) */
		nvkm_mask(device, GPC2CLK_OUT_R, 0x3f00, 0);
		/* nouveau's nvkm_mask computes in int and declares two locals. */
		/* NOLINTNEXTLINE(bugprone-signed-bitwise,readability-isolate-declaration) */
		nvkm_mask(device, GPC2CLK_OUT_R, 0x3f00, 0);
		nvkm_rd32(device, GPC2CLK_OUT_R);
	}
	back = nvkm_rd32(device, GPCPLL_COEFF_R);
	return ((back >> 16) & 0x3f) == pl &&
			       tgm_napll_cfg_running(nvkm_rd32(device, GPCPLL_CFG_R),
						     nvkm_rd32(device, TGM_GPCPLL_DVFS1_R)) &&
			       (nvkm_rd32(device, SEL_VCO_R) & 1) &&
			       !(nvkm_rd32(device, GPC2CLK_OUT_R) & 0x3f3f)
		       ? 0
		       : -EIO;
}

#include "SwitchGpuClockControl.h"

/* Up to the run rate if the idle divider is in force; returns the ns it took (0: nothing to do). Thread context. */
u64 tgm_idle_raise(void)
{
	u64 t0 = 0;
	u64 dt = 0;

	nvxnu_mutex_lock(tgm_idle.clk);
	if (READ_ONCE(tgm_idle.low)) {
		t0 = stats_on() ? nvxnu_nanotime() : 0;
		if (pl_switch(tgm_idle.run_pl)) {
			idle_fail("raise", tgm_idle.run_pl);
		} else {
			WRITE_ONCE(tgm_idle.low, 0);
			if (stats_on()) {
				dt = nvxnu_nanotime() - t0;
				tgm_idle.raises++;
				tgm_idle.raise_sum += dt;
				if (dt > tgm_idle.raise_max)
					tgm_idle.raise_max = dt;
				tgm_idle.low_ns += t0 - tgm_idle.low_since;
			}
		}
	}
	nvxnu_mutex_unlock(tgm_idle.clk);
	return dt;
}

static void idle_drop(void *arg)
{
	u64 now = 0;

	(void)arg;
	nvxnu_mutex_lock(tgm_idle.clk);
	now = nvxnu_nanotime();
	if (READ_ONCE(tgm_idle.on) && !READ_ONCE(tgm_idle.low) && !idle_in_flight() &&
	    now >= __atomic_load_n(&tgm_idle.service_until, __ATOMIC_SEQ_CST) &&
	    now - __atomic_load_n(&tgm_idle.last_busy, __ATOMIC_SEQ_CST) >= IDLE_DROP_NS &&
	    now - native_display_last_swap_ns() >= IDLE_DROP_NS) {
		if ((nvkm_rd32(tgm_gpu.device, GPCPLL_COEFF_R) & 0xffff) != tgm_idle.vco_coeff ||
		    pl_switch(tgm_idle.idle_pl)) {
			idle_fail("drop", tgm_idle.idle_pl);
		} else {
			WRITE_ONCE(tgm_idle.low, 1);
			if (stats_on()) {
				tgm_idle.drops++;
				tgm_idle.low_since = nvxnu_nanotime();
			}
		}
	}
	__atomic_store_n(&tgm_idle.dropping, 0, __ATOMIC_SEQ_CST);
	nvxnu_mutex_unlock(tgm_idle.clk);
	if (READ_ONCE(tgm_idle.low))
		tgm_rail_idle();
}

int tgm_idle_now(void)
{
	return !READ_ONCE(tgm_idle.on) || READ_ONCE(tgm_idle.low);
}

static void idle_hint_raise(void *arg)
{
	(void)arg;
	if (tgm_idle_raise()) {
		nvxnu_mutex_lock(tgm_idle.clk);
		tgm_idle.hinted++;
		nvxnu_mutex_unlock(tgm_idle.clk);
	}
}
#if TGM_STATS
/* Debug kernels: fn on a GPU held still, once the bring-up has reached M3: up and kept up under tgm_gpu.lock (no rail-down,
 * rail-up or system sleep), its GPCPLL divider fixed under tgm_idle.clk (taken inside tgm_gpu.lock, as sleep_idle_off takes it).
 * 0, or -EBUSY before M3. */
int tgm_gpu_held(void (*fn)(struct nvkm_device *device, void *arg), void *arg)
{
	tgm_gpu_lock();
	if (tgm_gpu.stage < 3) {
		mutex_unlock(&tgm_gpu.lock);
		return -EBUSY;
	}
	if (tgm_idle.clk)
		nvxnu_mutex_lock(tgm_idle.clk);
	fn(tgm_gpu.device, arg);
	if (tgm_idle.clk)
		nvxnu_mutex_unlock(tgm_idle.clk);
	mutex_unlock(&tgm_gpu.lock);
	return 0;
}

#endif
/* drivers/SwitchPower switch_perf_hint (touch, missed frame, launch): busy now, and back up at once if low. last_busy
 * is a time its readers only compare with their clock (idle_drop, SwitchGpuIdleDeadline.h); it publishes nothing else.
 * A drop that read the time just before this store is undone by the synchronous raise before the next kick. */
void tegra_gm20b_perf_hint(unsigned int hint)
{
	if (!READ_ONCE(tgm_idle.on) || hint < 1 || hint > 3)
		return;
	__atomic_store_n(&tgm_idle.last_busy, nvxnu_nanotime(), __ATOMIC_RELAXED);
	if (READ_ONCE(tgm_idle.low))
		nvxnu_work_schedule(tgm_idle.raise_w);
}

/* GPC 0's clock counter, as clk_measure reads it (NCGPCCLK cycles over 800 cycles of the 38.4 MHz reference, about
 * 21 us): started, waited for, read. In kHz of what it counts (about twice gpcclk if it counts gpc2clk). 0 if it could
 * not be read. tgm_idle_start only, before any client. */
#if DEVELOPMENT || DEBUG
static u32 gpc_counted_khz(void)
{
	struct nvkm_device *device = tgm_gpu.device;
	u32 cnt = 0;

	nvkm_wr32(device, TRIM_GPC0_NCGPCCLK_CFG, NCGPCCLK_CFG_RESET);
	nvkm_wr32(device, TRIM_GPC0_NCGPCCLK_CFG,
		  NCGPCCLK_CFG_ENABLE | NCGPCCLK_CFG_WRITE_EN | NCGPCCLK_CYCLES);
	udelay(30);
	cnt = nvkm_rd32(device, TRIM_GPC0_NCGPCCLK_CNT);
	return badf(cnt) ? 0
			 : (u32)((u64)(gk20a_clk(device->clk)->parent_rate / 1000) *
				 (cnt & 0xfffff) / NCGPCCLK_CYCLES);
}

#endif
/* Qualify the idle divider before exposing the clock policy to clients. */
void tgm_idle_start(void)
{
	struct nvkm_device *device = tgm_gpu.device;
	u32 coeff = nvkm_rd32(device, GPCPLL_COEFF_R);
	u32 m = coeff & 0xff;
	u32 n = (coeff >> 8) & 0xff;
	u32 pl = (coeff >> 16) & 0x3f;
	u32 vco = 0;
	u32 k = 0;
	u32 best = 0;
	u32 i = 0;
	int run = nvkm_clk_read(device->clk, nv_clk_src_gpc);
#if DEVELOPMENT || DEBUG
	u64 t0 = 0;
	u64 down = 0;
	u64 up = 0;
	u32 at_idle = 0;
	u32 at_run = 0;
#endif

	if (tgm_idle.clk || run <= BOOT_KHZ)
		return; /* tgm_clk_raise did not raise it: it already runs the lowest rate */
	vco = m ? PLL_REF_KHZ * n / m : 0;
	/* the lowest L4T B01 rate this VCO reaches by its output divider alone (gpcclk = VCO / 2 / PL) */
	for (i = 0; (k = tegra_gm20b_dvfs_khz(i)); i++) {
		if (k < (u32)run && vco && !(vco % (2 * k)) && vco / (2 * k) <= 31 &&
		    (!best || k < best))
			best = k;
	}
	if (!m || !pl || vco / (2 * pl) != (u32)run || !best) {
		nvxnu_log("tegra-gm20b: idle clock not used: GPCPLL M %u N %u PL %u (%u kHz VCO) "
			  "runs %d kHz, "
			  "no L4T rate below it by the divider alone; the GPU stays at %d kHz\n",
			  m, n, pl, vco, run, run);
		return;
	}
	tgm_idle.run_pl = pl;
	tgm_idle.run_khz = (u32)run;
	tgm_idle.vco_coeff = coeff & 0xffff;
	tgm_idle.vco_khz = vco;
	tgm_idle.maximum_khz = (u32)run;
	tgm_idle.requested_khz = 0;
	tgm_idle.idle_pl = vco / (2 * best);
	tgm_idle.idle_khz = best;
	tgm_idle.clk = nvxnu_mutex_alloc();
	tgm_idle.drop_w = nvxnu_work_alloc(idle_drop, NULL);
	tgm_idle.raise_w = nvxnu_work_alloc(idle_hint_raise, NULL);
	tgm_idle.settle_w = nvxnu_work_alloc(tgm_idle_settle, NULL);
	if (!tgm_idle.clk || !tgm_idle.drop_w || !tgm_idle.raise_w || !tgm_idle.settle_w) {
		nvxnu_log("tegra-gm20b: idle clock not used: no memory; the GPU stays at %d kHz\n",
			  run);
		return;
	}
	nvxnu_mutex_lock(tgm_idle.clk);
#if DEVELOPMENT || DEBUG
	t0 = nvxnu_nanotime();
#endif
	if (pl_switch(tgm_idle.idle_pl) ||
	    nvkm_clk_read(device->clk, nv_clk_src_gpc) != (int)best) {
		idle_fail("test drop", tgm_idle.idle_pl);
	} else {
#if DEVELOPMENT || DEBUG
		down = nvxnu_nanotime() - t0;
		at_idle = gpc_counted_khz();
		t0 = nvxnu_nanotime();
#endif
		if (pl_switch(tgm_idle.run_pl) || nvkm_clk_read(device->clk, nv_clk_src_gpc) != run)
			idle_fail("test raise", tgm_idle.run_pl);
#if DEVELOPMENT || DEBUG
		up = nvxnu_nanotime() - t0;
		at_run = gpc_counted_khz();
#endif
	}
	tgm_idle.log_start = nvxnu_nanotime();
	__atomic_store_n(&tgm_idle.last_busy, tgm_idle.log_start, __ATOMIC_SEQ_CST);
	WRITE_ONCE(tgm_idle.clock_available, 1);
	WRITE_ONCE(tgm_idle.on, 1);
	nvxnu_mutex_unlock(tgm_idle.clk);
#if DEVELOPMENT || DEBUG
	nvxnu_log("tegra-gm20b: idle clock: %d kHz while busy, %u kHz after %llu ms with no "
		  "submission and no "
		  "swap (GPCPLL M %u N %u: PL %u <-> %u, VCO %u kHz locked throughout, VDD_GPU "
		  "unchanged); "
		  "test drop %llu ns (GPC counter %u kHz), raise %llu ns (GPC counter %u kHz)\n",
		  run, best, IDLE_DROP_NS / 1000000, m, n, pl, tgm_idle.idle_pl, vco, down, at_idle,
		  up, at_run);
#endif
}

/* The last high completion, by both clocks (interrupt pass only) */
u64 tgm_high_end_ns, tgm_high_end_pt;
