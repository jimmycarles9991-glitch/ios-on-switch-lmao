// SPDX-License-Identifier: MIT
// GPCPLL programming and the idle and performance policy that sets the GPU clock.
#include "gpu_internal.h"

/* GPCPLL above 76.8 MHz: GPU_MHZ, one of L4T's B01 GPU rates (76.8 MHz x n). Runs
 * between M1 and M2, with the GPU as quiet as at the end of M1's own clk init (PRI ring, timer and GPCPLL up, no
 * falcon running yet), so the relock below happens under the conditions M1 has already proven; ACR, PMU, GR (M2) and
 * the channel test (M3) then run at the new rate, as nvgpu runs them at the rate its PLL holds (217.6 MHz on a
 * first power-on, gm20b_init_clk_setup_sw() clk_gm20b.c:1230-1242; the last DVFS rate after a railgate).
 *
 * PLL: nouveau's legacy fixed-frequency path (speedo ID 0, see m1), gk20a_clk_calc() + gk20a_clk_prog() as
 * gm20b_clk_init() runs them: NDIV slide to NDIV_LO, GPU on bypass (38.4 MHz with M1's 1:1 divider), relock with lock
 * detect, NDIV slide up. For every L4T rate it picks M=1 (38.4 MHz update rate) and the same N/PL that B01's C1 PLL
 * limits allow (VCO 1300-2600 MHz, u 19.2-38.4 MHz: nvgpu gpc_pll_params_c1, clk_gm20b.c:71-85), so this mode needs no
 * C1 parameters.
 *
 * Voltage: L4T has only noise-aware (NAPLL) tables for B01 and WARNs without NA mode (tegra210-dvfs.c:821-929,2089);
 * platform.c's tegra_gm20b_dvfs_na_uv() reproduces them. A fixed-frequency PLL cannot back off under supply droop and
 * needs more: on T210, where L4T has both, the fixed table asks 6-19% more than the NA table at equal speedo
 * (FIXED_FREQ_CVB_TABLE vs NA_FREQ_CVB_TABLE, :682-746, evaluated at speedo 1400-1900). So a rate is taken only if its
 * worst-case NA voltage plus FIXED_PLL_MARGIN_PCT fits the rail ceiling, 800 mV (switchroot's cap,
 * nx-common.dtsi:548). The rail is held at that ceiling for every rate: it is also L4T's floor at any rate below 20 C
 * (gpu_scaling_cdev trip0, nx-common.dtsi:1365), and nothing here reads the GPU temperature. Voltage goes up (with
 * readback) before the clock; this hook never lowers it.
 * For GPU speedo 1673 the 20% margin admits up to 768 MHz (worst NA 665 mV); HOS also stops retail units at 768 MHz
 * (nx-common.dtsi:521-523). 13% is T210's fixed/NA ratio at 921.6 MHz for speedo 1700 (+10..13% across 768-921.6), the
 * only measured basis for less, and would admit 844.8 MHz (685 mV + 13% = 774 mV) but not 921.6 (710 mV + 13% > 800 mV). */
#define FIXED_PLL_MARGIN_PCT 20
#define GPU_MHZ 768
#define BOOT_KHZ 76800 /* gm20b_pstates[0], set by gm20b_clk_init() in M1 */

static int clk_prog_khz(struct nvkm_device *device, u32 khz)
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

/* One CLK trace line after a PLL change: GPCPLL state and, over the PRI ring, whether the ring survived (as M1 reads
 * it). Returns 1 if the GPU runs from a locked GPCPLL at `khz` with the 1:1 bypass divider and a live ring. */
static int clk_state(struct nvkm_device *device, const char *what, int ret, u32 khz)
{
	u32 cfg = nvkm_rd32(device, 0x137000);   /* GPCPLL_CFG: 0 enable, 17 lock */
	u32 coeff = nvkm_rd32(device, 0x137004); /* GPCPLL_COEFF: M 7:0, N 15:8, PL 21:16 */
	u32 vco = nvkm_rd32(device, 0x137100);   /* SEL_VCO: 0 = GPU on bypass */
	u32 out =
		nvkm_rd32(device, 0x137250); /* GPC2CLK_OUT: 5:0 bypass divider, 13:8 VCO divider */
	u32 ring = nvkm_rd32(device,
			     0x120050); /* PRI_RINGMASTER_START_RESULTS: 0 = connectivity pass */
	u32 bus = nvkm_rd32(device, 0x001100); /* BUS_INTR_0: 3 = PRI timeout */
	int now = nvkm_clk_read(device->clk, nv_clk_src_gpc);

	nvxnu_step("tegra-gm20b: CLK step: %s, ret %d: GPCPLL_CFG %08x COEFF %08x (M %u N %u PL "
		   "%u) SEL_VCO %x "
		   "GPC2CLK_OUT %08x = %d kHz; ring %08x BUS_INTR %08x\n",
		   what, ret, cfg, coeff, coeff & 0xff, (coeff >> 8) & 0xff, (coeff >> 16) & 0x3f,
		   vco, out, now, ring, bus);
	return !ret && (cfg & 0x20001) == 0x20001 && (vco & 1) && !(out & 0x3f3f) &&
	       now == (int)khz && !badf(ring) && (ring & 1) && !(bus & 0x8);
}

int tgm_clk_raise(char *line, unsigned int len)
{
	struct nvkm_device *device = tgm_gpu.device;
	int speedo = tegra_sku_info.gpu_speedo_value;
	int ceil_uv = tegra_gm20b_rail_max_uv();
	int uv = 0;
	int na_uv = 0;
	int need_uv = 0;
	int ret = 0;
	int back = 0;
	int i = 0;
	int fits = 1;
	const int margin = FIXED_PLL_MARGIN_PCT;
	u32 khz = 0;
	u32 best = 0;
	u32 k = 0;
	const long long mhz = GPU_MHZ;

	for (i = 0; (k = tegra_gm20b_dvfs_khz(i)); i++) {
		int v = tegra_gm20b_dvfs_na_uv(k, speedo);
		if (mhz > 0 && mhz < 10000 && mhz * 1000 > (long long)k - 1000 &&
		    mhz * 1000 < (long long)k + 1000)
			khz = k;
		fits = fits && v > 0 && DIV_ROUND_UP(v * (100 + margin), 100) <= ceil_uv;
		if (fits)
			best = k; /* L4T drops every rate above the first one that does not fit */
	}
	if (!khz) {
		nvxnu_log("tegra-gm20b: %lld MHz is not an L4T B01 GPU rate (76.8 MHz x "
			  "1..16, 1267.2): GPU stays at "
			  "%d kHz\n",
			  mhz, nvkm_clk_read(device->clk, nv_clk_src_gpc));
		return 0;
	}
	if (nvkm_clk_read(device->clk, nv_clk_src_gpc) == (int)khz)
		return 0; /* the boot rate, or a retried stage 2 */
	na_uv = tegra_gm20b_dvfs_na_uv(khz, speedo);
	if (na_uv < 0) {
		nvxnu_log("tegra-gm20b: %lld MHz refused: no B01 table voltage for GPU "
			  "speedo %d; GPU stays at %d kHz\n",
			  mhz, speedo, nvkm_clk_read(device->clk, nv_clk_src_gpc));
		return 0;
	}
	need_uv = DIV_ROUND_UP(na_uv * (100 + margin), 100);
	if (need_uv > ceil_uv) {
		nvxnu_log("tegra-gm20b: %lld MHz refused: %u kHz needs %d uV in "
			  "fixed-frequency mode (B01 NA table %d "
			  "uV + %d%%) for GPU speedo %d, above the %d uV rail ceiling; GPU stays "
			  "at %d kHz (highest rate that fits: "
			  "%u kHz)\n",
			  mhz, khz, need_uv, na_uv, margin, speedo, ceil_uv,
			  nvkm_clk_read(device->clk, nv_clk_src_gpc), best);
		return 0;
	}
	ret = tegra_gm20b_rail_get_uv(&uv);
	nvxnu_step("tegra-gm20b: CLK step: %lld MHz -> %u kHz, GPU speedo %d: B01 NA "
		   "table %d uV, +%d%% fixed PLL "
		   "%d uV, ceiling %d uV (highest rate that fits %u kHz); VDD_GPU %d uV (ret %d)\n",
		   mhz, khz, speedo, na_uv, margin, need_uv, ceil_uv, best, uv, ret);

	/* voltage first */
	if (!ret && uv < ceil_uv) {
		ret = tegra_gm20b_rail_set_uv(
			ceil_uv); /* MAX77812 write + readback, then the ramp delay */
		if (!ret)
			ret = tegra_gm20b_rail_get_uv(&uv);
		nvxnu_step("tegra-gm20b: CLK step: VDD_GPU raised to %d uV (ret %d) before the "
			   "clock\n",
			   uv, ret);
	}
	if (ret || uv < ceil_uv) {
		nvxnu_log("tegra-gm20b: %lld MHz: VDD_GPU %d uV (ret %d), %d uV needed "
			  "first; GPU stays at %d kHz\n",
			  mhz, uv, ret, ceil_uv, nvkm_clk_read(device->clk, nv_clk_src_gpc));
		return 0;
	}

	/* then the clock */
	nvxnu_step("tegra-gm20b: CLK step: GPCPLL_CFG %08x COEFF %08x = %d kHz -> %u kHz (NDIV "
		   "slide, bypass, relock, slide)\n",
		   nvkm_rd32(device, 0x137000), nvkm_rd32(device, 0x137004),
		   nvkm_clk_read(device->clk, nv_clk_src_gpc), khz);
	ret = clk_prog_khz(device, khz);
	if (clk_state(device, "raised", ret, khz)) {
		/* the one result line (nvxnu_step prints only when traced): the rate the PLL now holds, from
		 * its own registers; the stats line measures it every window (clk_measure) */
		u32 coeff = nvkm_rd32(device, 0x137004);
		nvxnu_log("tegra-gm20b: GPU clock %d kHz (GPCPLL M %u N %u PL %u, "
			  "%lld MHz), "
			  "VDD_GPU %d uV\n",
			  nvkm_clk_read(device->clk, nv_clk_src_gpc), coeff & 0xff,
			  (coeff >> 8) & 0xff, (coeff >> 16) & 0x3f, mhz, uv);
		return 0;
	}

	/* not at the target: one way back to the M1 rate, at the same voltage */
	back = clk_prog_khz(device, BOOT_KHZ);
	if (clk_state(device, "back to the boot rate", back, BOOT_KHZ)) {
		nvxnu_log("tegra-gm20b: %lld MHz: GPCPLL did not settle at %u kHz (ret "
			  "%d); back at %u kHz\n",
			  mhz, khz, ret, BOOT_KHZ);
		return 0;
	}
	scnprintf(line, len,
		  "M2 FAIL GPCPLL to %u kHz (ret %d) and back to %u kHz (ret %d): GPCPLL_CFG %08x "
		  "COEFF %08x "
		  "SEL_VCO %x",
		  khz, ret, BOOT_KHZ, back, nvkm_rd32(device, 0x137000),
		  nvkm_rd32(device, 0x137004), nvkm_rd32(device, 0x137100));
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
 * Before first use, gov_start runs one drop and raise at stage 3 (no client yet) and logs both times. Any failure
 * (the divider not reading back, the PLL not locked, the clock not at the rate) logs one line and leaves the GPU at
 * 768 MHz for the boot. */
#define IDLE_DROP_NS (500ULL * 1000000ULL)
#define GPCPLL_CFG_R 0x137000  /* 0 enable, 17 lock */
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
			       (nvkm_rd32(device, GPCPLL_CFG_R) & 0x20001) == 0x20001 &&
			       (nvkm_rd32(device, SEL_VCO_R) & 1) &&
			       !(nvkm_rd32(device, GPC2CLK_OUT_R) & 0x3f3f)
		       ? 0
		       : -EIO;
}

/* Under tgm_idle.clk: a divider change failed. One line, then 768 MHz for the boot (the divider, else nouveau's full
 * reprogram of the rate), the idle clock off. */
static void idle_fail(const char *what, u32 pl)
{
	struct nvkm_device *device = tgm_gpu.device;
	int back = 0;

	WRITE_ONCE(tgm_idle.on, 0);
	WRITE_ONCE(tgm_idle.clock_available, 0);
	back = pl_switch(tgm_idle.run_pl);
	if (back)
		back = clk_prog_khz(device, tgm_idle.run_khz);
	WRITE_ONCE(tgm_idle.low, 0);
	nvxnu_log("tegra-gm20b: idle clock off: %s to PL %u did not take (GPCPLL_CFG %08x COEFF "
		  "%08x SEL_VCO %x "
		  "GPC2CLK_OUT %08x); %d kHz now (%d), for this boot\n",
		  what, pl, nvkm_rd32(device, GPCPLL_CFG_R), nvkm_rd32(device, GPCPLL_COEFF_R),
		  nvkm_rd32(device, SEL_VCO_R), nvkm_rd32(device, GPC2CLK_OUT_R),
		  nvkm_clk_read(device->clk, nv_clk_src_gpc), back);
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
		if (pl_switch(tgm_idle.idle_pl)) {
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
/* drivers/SwitchPower switch_perf_hint (touch, missed frame, launch): busy now, and back up at once if low. */
void tegra_gm20b_perf_hint(unsigned int hint)
{
	if (!READ_ONCE(tgm_idle.on) || hint < 1 || hint > 3)
		return;
	__atomic_store_n(&tgm_idle.last_busy, nvxnu_nanotime(), __ATOMIC_SEQ_CST);
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
		nvxnu_mutex_unlock(tgm_idle.clk);
		return;
	}
#if DEVELOPMENT || DEBUG
	down = nvxnu_nanotime() - t0;
	at_idle = gpc_counted_khz();
	t0 = nvxnu_nanotime();
#endif
	if (pl_switch(tgm_idle.run_pl) || nvkm_clk_read(device->clk, nv_clk_src_gpc) != run) {
		idle_fail("test raise", tgm_idle.run_pl);
		nvxnu_mutex_unlock(tgm_idle.clk);
		return;
	}
#if DEVELOPMENT || DEBUG
	up = nvxnu_nanotime() - t0;
	at_run = gpc_counted_khz();
#endif
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
