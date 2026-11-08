// SPDX-License-Identifier: MIT
/*
 * GM20B bring-up and the per-connection GPU model, on top of the vendored nouveau nvkm/nvif code.
 * Mirrors what nouveau's DRM driver does for the same jobs:
 *   bring-up      nouveau_platform.c (gm20b_platform_data) + nvkm_device_tegra_new() + nvkm_device_init(), the latter
 *                 as m2_device_init() (no fini pass, GPCPLL up before L2/FB/PMU, as nvgpu)
 *   address space nouveau_uvmm.c VM_INIT (RAW vmm, kernel range [1<<39, 1<<40)) and VM_BIND (raw get/map/unmap)
 *   memory        nvkm_mem_new_host() with a caller-owned DMA address array (nvif_mem_ram_v0.dma)
 *   channels      nouveau_chan.c nouveau_channel_ctor()/_init() + nvif/chan906f.c GPFIFO/pushbuf helpers
 *   submission    nouveau_exec.c (one GPFIFO entry per push) + nv84/gf100 fence semaphore release
 * Interrupts, no polled mode: from the end of M1 the GPU's stall/nonstall SPIs drive nvkm_intr from a work loop
 * (nvxnu_irq_start, shim/xnu_glue.cpp) and every wait sleeps in nvxnu_idle(); each fence also raises a nonstall
 * interrupt (NON_STALL_INTERRUPT after the semaphore release, as nouveau's nvc0_fence_emit32) that a per-channel
 * NON_STALL_INTR event turns into a wake. A fence found complete with no wake panics (nvxnu_wake_due).
 * GPU L2: user mappings are L2-cacheable instead of volatile, as nvgpu maps
 * the buffers its clients ask to be cacheable (gk20a/mm_gk20a.c __update_pte: VOL only for !cacheable); see l2_on().
 * GPU present: the display's frames are swizzled into its block-linear scanout by the copy
 * engine on a kernel channel that waits for the compositor's work on the GPU (see "GPU present" below).
 * GPU failures from the end of the self-test (M3) on: an error nvkm's interrupt handler reports (MMU fault, PBDMA, GR,
 * FECS, a context switch that times out, ...: nvxnu_error) on a client's channel, or work that stops completing with no
 * error at all (fence waits, sync_for_cpu, GPFIFO full, channel teardown, a late present), recovers the channel the GPU
 * is stuck on and kills only it (see "client fault isolation"); the compositor's channels included, as an app's. What
 * panics, with one line naming it, is a failure on the kernel's own channels and a GPU on which nothing progresses.
 */
#include <core/device.h>
#include <core/tegra.h>
#include <engine/device/priv.h> /* nvkm_device_fini: system sleep */
#include <engine/fifo.h>
#include <engine/fifo/runl.h> /* nvkm_runl_chan_get_chid, nvkm_runl_rc_cgrp: client fault isolation */
#include <engine/fifo/cgrp.h> /* nvkm_cgrp.rc: a channel nvkm itself put into recovery */
#include <engine/fifo/runq.h> /* nvkm_runq.id: the engine list's PBDMAs */
#include <core/gpuobj.h>      /* nvkm_wo32 on a channel's RAMFC: compositor timeslice */
#include <engine/gr.h>
#include <engine/gr/gf100.h> /* gf100_gr_zbc_color_get, _depth_get: tgm_zbc_add */
#include <subdev/acr.h>
#include <subdev/clk.h>
#include <subdev/clk/priv.h> /* nvkm_clk_func: clk_raise() runs calc/prog as gm20b_clk_init() does */
#include <subdev/clk/gk20a.h> /* gk20a_clk.parent_rate: clk_measure()'s reference */
#include <subdev/devinit.h>
#include <subdev/fb.h>
#include <subdev/ltc.h>
#include <subdev/mmu.h>
#include <subdev/timer.h>
#include <subdev/timer/priv.h> /* nvkm_timer_func.time: GPU time reseeded, never behind what it handed out */
#include <subdev/top.h>
#include <nvif/class.h>
#include <nvif/cl0080.h>
#include <nvif/cl9097.h>
#include <nvif/client.h>
#include <nvif/device.h>
#include <nvif/driver.h>
#include <nvif/event.h>
#include <nvif/fifo.h>
#include <nvif/if000a.h>
#include <nvif/if0020.h>
#include <nvif/if900d.h>
#include <nvif/ifb00d.h>
#include <nvif/mem.h>
#include <nvif/mmu.h>
#include <nvif/parent.h>
#include <nvif/vmm.h>
#include <nvif/chan.h>
#include <nvif/push906f.h>
#include <nvhw/class/cla0b5.h>
#include "TegraGM20BClkgate.h"
#include "TegraGM20BComp.h"
#include "TegraGM20BGpu.h"
#include "TegraGM20BFences.h"
#include "TegraGM20BHangDecode.h"
#include "TegraGM20BRail.h"
#include "TegraGM20BRecover.h"
#include "TegraGM20BSched.h"
#include "../../kits/SwitchIdle/switch_idle.h"
#include "TegraGM20BSchedPolicy.h"
#include "TegraGM20BSleepGate.h"
#include "TegraGM20BTlb.h"
#include "../SwitchSuspend/SwitchSleepSteps.h"
#include "../TegraDisplayFlip/SwitchFrameTimeline.h"
#include "../TegraDisplayFlip/TegraScanoutCopy.h"

/* mainline tegra210.dtsi gpu@57000000: BAR0, BAR1, SPI 157 ("stall") / 158 ("nonstall") */
static struct resource gpu_res[] = {
	{.start = 0x57000000, .end = 0x57ffffff, .name = "bar0", .flags = IORESOURCE_MEM},
	{.start = 0x58000000, .end = 0x58ffffff, .name = "bar1", .flags = IORESOURCE_MEM},
};
static struct platform_device gpu_pdev = {
	.name = "gpu",
	.id = 0,
	.dev = {.init_name = "57000000.gpu"},
	.num_resources = 2,
	.resource = gpu_res,
	.irq_stall = 157 + 32,
	.irq_nonstall = 158 + 32,
};
/* nouveau_platform.c gm20b_platform_data */
static const struct nvkm_device_tegra_func gm20b_platform_data = {
	.iommu_bit = 34,
	.require_vdd = true,
	.require_ref_clk = true,
};

static struct {
	int stage;
	struct nvkm_device *device;
	u32 boot0;
	struct nvif_client client;
	struct nvif_device dev;
	struct nvif_mmu mmu;
	int mem_type;
	struct mutex lock; /* serialises every nvif call (nouveau holds cli->mutex likewise) */
	int railed; /* rail-gated (TegraGM20BRail.h): powered down, under gpu.lock; the gate shut */
} gpu = {.lock = {NULL}};

/* Rail-gating's count of wakes, for its journal line: how many the display asked for, how many work at gpu.lock or the
 * gate did, where the first such caller came from, and the pool bytes its callers zeroed on the CPU while it was down. */
static struct {
	u32 display, demand;
	const void *first_site;
	u64 cpu_zeroed;
} rail_ups;

/* The last GPU time handed out (tgm_timestamp_ns) or read before a power-down (sleep_nvkm), under gpu.lock: the floor of
 * every GPU time after it (tgm_stamp_monotonic). */
static u64 stamp_last;

static void rail_up_locked(const void *site);

/* gpu.lock for an nvif call. A rail-gated GPU is woken here, by the caller that needs it, under the lock it owns: the
 * next holder finds the GPU up, and nothing reaches its registers while it is down. */
static noinline void gpu_lock(void)
{
	mutex_lock(&gpu.lock);
	if (unlikely(gpu.railed))
		rail_up_locked(__builtin_return_address(0));
}

int tgm_stage(void)
{
	return gpu.stage;
}

#if TGM_STATS
/* Debug kernels: clock gating off (nvgpu's disable values, ELCG run) or on again, for a frame-time A/B within one boot;
 * under gpu.lock, once the bring-up has reached M3. */
int tgm_clkgate_switch(int on)
{
	int ret = 0;

	gpu_lock();
	if (gpu.stage < 3) {
		ret = -EBUSY;
	} else if (on) {
		tgm_clkgate_enable(gpu.device);
	} else {
		tgm_clkgate_disable(gpu.device);
	}
	mutex_unlock(&gpu.lock);
	nvxnu_log("tegra-gm20b: clock gating %s (debug A/B)%s\n", on ? "on" : "off",
		  ret ? ": not yet, the bring-up has not reached M3" : "");
	return ret;
}
#endif

/* Fence waits by channel (nouveau_fence_chan.waiting): a fence's nonstall interrupt wakes the waiters of the channels
 * whose fence moved, not every thread in a GPU wait. nvkm runs every channel's nonstall handler on each nonstall
 * interrupt, so one global wake per handler woke each waiter once per live channel per fence. Channels share the words
 * by id; a waiter of another channel on the same word wakes, finds its fence unchanged and sleeps again. Static: a
 * waiter may still sleep on its channel's word while the channel is destroyed. */
#define TGM_WAITERS 128
static volatile u32 waiter_words[TGM_WAITERS];
/* stats: when each word was last woken (ns), and this window's wakes and fence waits (the "waits" line) */
static u64 waiter_woken_ns[TGM_WAITERS];

static struct {
	u32 wakes, slept, woken, other, deadline;
	u64 lat_sum, lat_max;
	u64 stall, nonstall; /* the interrupt passes at the window's start */
} wstat;

uint32_t tgm_waiter_seq(uint32_t waiter)
{
	return nvxnu_word_seq(&waiter_words[waiter % TGM_WAITERS]);
}

int tgm_waiter_idle(uint32_t waiter, uint32_t seq, uint64_t deadline_ns)
{
	return nvxnu_word_idle(&waiter_words[waiter % TGM_WAITERS], seq, deadline_ns);
}

void tgm_waiter_unwoken(uint32_t waiter, uint32_t seq, uint32_t seqno)
{
	nvxnu_word_wake_due(&waiter_words[waiter % TGM_WAITERS], seq, "fence", seqno);
}

static int badf(u32 v)
{
	return (v & 0xffff0000) == 0xbadf0000;
}

/* System sleep's gate (TegraGM20BSleepGate.h, "system sleep" below): what touches the GPU outside gpu.lock goes through it. */
static struct tgm_gate sc7_gate;
static DEFINE_MUTEX(gate_lock);

static void gate_wake_closer(void)
{
	mutex_lock(&gate_lock);
	nvxnu_wakeup(&sc7_gate.inside);
	mutex_unlock(&gate_lock);
}

/* In, waiting for the wake while the gate is shut. */
static void gate_enter(void)
{
	enum tgm_gate_entry entry = TGM_GATE_IN;

	while ((entry = tgm_gate_try_enter(&sc7_gate)) != TGM_GATE_IN) {
		if (READ_ONCE(gpu.railed)) {
			if (entry == TGM_GATE_SHUT_WAKE)
				gate_wake_closer();
			gpu_lock();
			mutex_unlock(&gpu.lock);
			continue;
		}
		mutex_lock(&gate_lock);
		if (entry == TGM_GATE_SHUT_WAKE)
			nvxnu_wakeup(&sc7_gate.inside);
		while (tgm_gate_is_shut(&sc7_gate) && !READ_ONCE(gpu.railed))
			nvxnu_mutex_sleep(gate_lock.lck, &sc7_gate.shut, ~0ULL);
		mutex_unlock(&gate_lock);
	}
}

/* In (1), or refused while the gate is shut (0). */
static int gate_try(void)
{
	enum tgm_gate_entry entry = tgm_gate_try_enter(&sc7_gate);

	if (entry == TGM_GATE_SHUT_WAKE)
		gate_wake_closer();
	return entry == TGM_GATE_IN;
}

static void gate_leave(void)
{
	if (tgm_gate_leave(&sc7_gate))
		gate_wake_closer();
}

static int m1(char *line, unsigned int len)
{
	struct nvkm_device *device = NULL;
	u32 boot0 = 0;
	u32 start = 0;
	u32 gpcs = 0;
	u32 fbps = 0;
	u32 cfg = 0;
	u32 coeff = 0;
	u32 rail = 0;
	int ret = 0;
	int uv = 0;

	/* Speedo ID 0 keeps nouveau's gm20b clk on the legacy fixed-frequency GPCPLL path (lock-detect, no fused
	 * calibration), which nvgpu also runs on B01's C1 PLL when NA mode is off (clk_gm20b.c:1211-1215,1250-1257).
	 * L4T uses 1 = NAPLL on B01 (speedo-tegra210.c:189), but nouveau's NAPLL path carries T210 B1 constants
	 * (nouveau gm20b.c:99-103, nvgpu clk_gm20b.c:56-69) where B01 needs C1 (clk_gm20b.c:71-85,
	 * platform_gk20a_tegra.c:815-817), so it is not used. */
	/* Bring-up trace: let the console's backlog drain first, so every step line below is on screen before its step. */
	if (nvxnu_trace_paced())
		nvxnu_usleep(2000000);
	nvxnu_step("tegra-gm20b: M1 step: start\n");
	ret = tegra_gm20b_platform_map();
	if (ret) {
		scnprintf(line, len, "M1 FAIL map CAR/PMC/FUSE/MC %d", ret);
		return ret;
	}
	/* T210B01 fuses the GPU speedo value in CPU_SPEEDO_2 (L4T speedo-tegra210.c:286-294) */
	tegra_sku_info.gpu_speedo_value = (int)tegra_gm20b_fuse_read(0x030);
	tegra_sku_info.gpu_speedo_id = 0;
	nvxnu_step("tegra-gm20b: M1 step: CAR/PMC/FUSE/MC mapped, GPU speedo %d; pool\n",
		   tegra_sku_info.gpu_speedo_value);
	ret = nvxnu_pool_setup();
	if (ret) {
		scnprintf(line, len, "M1 FAIL pool %d", ret);
		return ret;
	}
	/* power-up (shim/tegra_shim.c), then BOOT_0 and every subdev constructor */
	ret = nvkm_device_tegra_new(&gm20b_platform_data, &gpu_pdev, "", "error", &device);
	if (ret) {
		scnprintf(line, len, "M1 FAIL nvkm_device_tegra_new %d", ret);
		return ret;
	}
	gpu.device = device;
	nvxnu_step("tegra-gm20b: M1 step: device constructed, claim GPU interrupts\n");
	/* nvkm's handler is recorded (request_irq in nvkm_device_ctor) and nothing is armed yet: claim the GPU lines
	 * before the first nvkm init that may wait */
	nvxnu_irq_start();
	boot0 = nvkm_rd32(device, 0x000000);
	gpu.boot0 = boot0;
	tegra_gm20b_rail_get_uv(&uv);
	nvxnu_step("tegra-gm20b: M1 step: BOOT_0 %08x, rail %d uV; init PRI ring\n", boot0, uv);

	ret = nvkm_subdev_init(device->privring);
	if (!ret)
		ret = nvkm_subdev_init(&device->timer->subdev);
	start = nvkm_rd32(device,
			  0x120050); /* PRI_RINGMASTER_START_RESULTS, bit 0 = connectivity pass */
	fbps = nvkm_rd32(device, 0x120074); /* PRI_RINGMASTER_ENUM_FBP */
	gpcs = nvkm_rd32(device, 0x120078); /* PRI_RINGMASTER_ENUM_GPC */
	nvxnu_step("tegra-gm20b: M1 step: ring start %08x gpc %08x fbp %08x ret %d; init clk "
		   "(GPCPLL)\n",
		   start, gpcs, fbps, ret);
	if (!ret)
		ret = nvkm_subdev_init(&device->clk->subdev);
	if (!ret) {
		/* nouveau's clk init puts the bypass path back on the 1/31 boot divider (GPC2CLK_OUT_INIT_VAL BYPDIV31,
		 * clk/gk20a.h:91-99); nvgpu runs it 1:1 (gm20b_init_clk_setup_hw, clk_gm20b.c:1362-1371). The GPU is on the
		 * VCO here, so this changes nothing now; any later trip through bypass (PLL reprogramming) then stays at
		 * 38.4 MHz instead of running the whole GPU, PRI ring included, at 1.2 MHz. */
		/* nouveau's nvkm_mask computes in int and declares two locals. */
		/* NOLINTNEXTLINE(bugprone-signed-bitwise,readability-isolate-declaration) */
		nvkm_mask(device, 0x137250, 0x0000003f, 0x00000000);
		nvxnu_step("tegra-gm20b: M1 step: GPC2CLK_OUT %08x (bypass divider 1:1), SEL_VCO "
			   "%08x\n",
			   nvkm_rd32(device, 0x137250), nvkm_rd32(device, 0x137100));
	}
	cfg = nvkm_rd32(device, 0x137000);   /* GPCPLL_CFG: bit 0 enable, bit 17 lock */
	coeff = nvkm_rd32(device, 0x137004); /* GPCPLL_COEFF: M 7:0, N 15:8, PL 21:16 */
	rail = uv;
	const char *probe = "fail";

	if (badf(start)) {
		probe = "BADF";
	} else if (start & 1) {
		probe = "pass";
	}
	scnprintf(line, len,
		  "M1 %s boot0=%08x chip=%03x rail=%uuV ring=%s gpc=%u fbp=%u gpcpll cfg=%08x "
		  "coeff=%08x %s %dkHz ret=%d",
		  ret ? "FAIL" : "ok", boot0, (boot0 >> 20) & 0x1ff, rail, probe, gpcs & 0x1f,
		  fbps & 0x1f, cfg, coeff, (cfg & 0x20001) == 0x20001 ? "locked" : "UNLOCKED",
		  ret ? 0 : nvkm_clk_read(device->clk, nv_clk_src_gpc), ret);
	if (!ret && ((boot0 >> 20) & 0x1ff) != 0x12b && ((boot0 >> 20) & 0x1ff) != 0x12e)
		ret = -ENODEV;
	return ret;
}

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

static int clk_raise(char *line, unsigned int len)
{
	struct nvkm_device *device = gpu.device;
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

/* One M2 trace line: the host's PMC/bus registers (always answer) and, over the PRI ring, the ring master's interrupt
 * status and the GPCPLL output, so a step that leaves the ring or the GPU clock dead shows on the next line. */
static void m2_step(struct nvkm_device *device, const char *what, const char *name)
{
	u32 pmc = nvkm_rd32(device, 0x000200);  /* PMC_ENABLE */
	u32 elpg = nvkm_rd32(device, 0x00020c); /* PMC_ELPG_ENABLE: xbar 2, pfb 20, hub 29 */
	u32 bus = nvkm_rd32(device, 0x001100);  /* BUS_INTR_0: 1 squash, 2 fecserr, 3 PRI timeout */
	u32 ring = nvkm_rd32(
		device, 0x120058); /* PRI_RINGMASTER_INTR_STATUS0: 0 start fault, 1 disconnect */
	u32 pll = nvkm_rd32(device, 0x137000); /* GPCPLL_CFG: 0 enable, 1 IDDQ, 17 lock */
	u32 vco = nvkm_rd32(device, 0x137100); /* SEL_VCO: 0 = GPU on bypass, 1 = on GPCPLL */
	u32 out = nvkm_rd32(device, 0x137250); /* GPC2CLK_OUT: 5:0 bypass divider */

	nvxnu_step("tegra-gm20b: M2 step: %s%s: PMC_ENABLE %08x ELPG %08x BUS_INTR %08x ring %08x "
		   "GPCPLL %08x SEL_VCO %x "
		   "GPC2CLK_OUT %08x\n",
		   what, name, pmc, elpg, bus, ring, pll, vco, out);
}

/* nvkm_device_init() (engine/device/base.c) without its nvkm_device_fini() pass, one trace line per subdev init.
 * M1 left privring, timer and clk running, GPU on the GPCPLL. nvkm_device_init() first fini's every subdev, which drops
 * the GPU to bypass (gk20a_clk_fini: SEL_VCO bypass, PLL off, IDDQ) on the 1/31 divider nouveau's clk init re-armed,
 * then inits in core/layout.h order: privring (PRI ring reset through PMC_ENABLE bit 5 right after a posted write of
 * that divider, privring/gk20a.c:29-33), fb, ltc, mmu, bar, acr (HS ACR), pmu (LS firmware start), volt, and clk only
 * once the PMU firmware runs. That order timed out at the first L2 access (ltc zbc 0x17e338), then on the PMU's own
 * PRI reads around the point clk reprograms the PLL. nvgpu's gk20a_finalize_poweron()
 * (gk20a/gk20a.c:195-254) brings up bus, priv ring, clk (GPCPLL on, slowdown-on-boot divider removed) and only then
 * fifo, ltc and mm.
 * Without the fini, nvkm_subdev_init() passes over the three running subdevs and the rest come up in nvgpu's order on
 * the GPCPLL. The GPU left CAR reset in M1, so the fini pass's engine PMC resets have nothing to clear. */
static int m2_device_init(struct nvkm_device *device, const char **at)
{
	struct nvkm_subdev *subdev = NULL;
	int ret = 0;

	/* nvkm_device_preinit(); Tegra has no device preinit and GM20B no devinit (nvkm_devinit_post takes NULL) */
	nvkm_intr_unarm(device);
	list_for_each_entry(subdev, &device->subdev, head)
	{
		*at = subdev->name;
		ret = nvkm_subdev_preinit(subdev);
		if (ret)
			return ret;
	}
	*at = "preinit";
	ret = nvkm_devinit_post(device->devinit);
	if (!ret)
		ret = nvkm_top_parse(device);
	if (!ret)
		ret = nvkm_fb_mem_unlock(device->fb);
	if (ret)
		return ret;
	m2_step(device, "preinit done, privring/timer/clk still up from M1", "");

	nvkm_intr_rearm(device);
	list_for_each_entry(subdev, &device->subdev, head)
	{
		/* what nvkm_subdev_init() skips: running since M1, or an engine with no user yet (GR comes up by ref below) */
		if (subdev->use.enabled || !refcount_read(&subdev->use.refcount))
			continue;
		*at = subdev->name;
		if (subdev == &device->fb->subdev) {
			/* nvgpu gm20b_mc_fb_reset() (common/mc/mc_gm20b.c:340-351), run by nvgpu_init_mm_reset_enable_hw()
			 * (common/mm/mm.c:347-351) before any FB/MMU programming; nouveau never writes it */
			u32 elpg = nvkm_rd32(device, 0x00020c);
			nvkm_wr32(device, 0x00020c, elpg | 0x00000004 | 0x00100000 | 0x20000000);
			nvxnu_step("tegra-gm20b: M2 step: ELPG_ENABLE %08x -> %08x (xbar|pfb|hub, "
				   "nvgpu gm20b_mc_fb_reset)\n",
				   elpg, nvkm_rd32(device, 0x00020c));
		}
		m2_step(device, "init ", subdev->name);
		ret = nvkm_subdev_init(subdev);
		if (ret)
			return ret;
	}
	/* nvkm_acpi_init() and nvkm_therm_clkgate_enable() have nothing to do here: no ACPI, no therm subdev on GM20B */
	m2_step(device, "subdevs up, next GR (FECS/GPCCS booted by the PMU)", "");
	return 0;
}

/* nvif's object printer (NVIF_ERROR/NVIF_ERRON in nvif/printf.h dereference object->parent->func): nouveau_drm.c installs
 * one on its client before creating objects; without it the first nvif error message is a NULL dereference (a failed
 * channel event in M3 panics in nvif_event_ctor_). Errors go to the log, debug output nowhere. */
static void tgm_nvif_errorf(struct nvif_object *object, const char *fmt, ...)
{
	char buf[256];
	va_list ap; /* NOLINT(cppcoreguidelines-init-variables): va_start initializes it */

	(void)object;
	va_start(ap, fmt);
	nvxnu_vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	nvxnu_log("tegra-gm20b nvif: %s", buf);
}

static void tgm_nvif_debugf(struct nvif_object *object, const char *fmt, ...)
{
	(void)object;
	(void)fmt;
}

static const struct nvif_parent_func tgm_nvif_parent_func = {
	.debugf = tgm_nvif_debugf,
	.errorf = tgm_nvif_errorf,
};
static struct nvif_parent tgm_nvif_parent;

static int m2(char *line, unsigned int len)
{
	struct nvkm_device *device = gpu.device;
	struct nvkm_engine *gr = NULL;
	const char *at = "";
	u64 units = 0;
	int ret = m2_device_init(
		device,
		&at); /* fb, ltc, mmu, bar, ACR (HS on PMU), LS PMU, volt; privring/clk: M1 */

	if (ret) {
		scnprintf(line, len, "M2 FAIL nvkm init at %s: %d", at, ret);
		return ret;
	}
	/* The device reference nvkm_udevice_init() takes for the first nvif device (engine/device/user.c:277-295): held
	 * here, so the nvif device made below does not run nvkm_device_init() again (its fini pass would drop the GPU to
	 * bypass and reset GR under the running PMU) */
	mutex_lock(&device->mutex);
	device->refcount++;
	mutex_unlock(&device->mutex);

	gr = nvkm_engine_ref(
		&device->gr->engine); /* oneinit+init: FECS/GPCCS via ACR, golden context */
	ret = IS_ERR(gr) ? (int)PTR_ERR(gr) : 0;
	if (!ret)
		units = nvkm_gr_units(device->gr);
	scnprintf(
		line, len,
		"M2 %s wpr=[%llx-%llx] acr=%s fecs_mb0=%08x gpccs_cpuctl=%08x gpc=%u tpc=%u ret=%d",
		ret ? "FAIL" : "ok", device->acr ? device->acr->wpr_start : 0,
		device->acr ? device->acr->wpr_end : 0,
		device->acr && device->acr->done ? "done" : "no", nvkm_rd32(device, 0x409040),
		nvkm_rd32(device, 0x41a100), (u32)(units & 0xff), (u32)((units >> 8) & 0xff), ret);
	if (ret)
		return ret;
	/* GR is up: this line is the stage's real result, so it reaches the log even if the nvif setup below fails */
	nvxnu_step("tegra-gm20b: %s\n", line);

	mutex_init(&gpu.lock);
	at = "nvif_driver_init";
	ret = nvif_driver_init(NULL, "", "error", "tegra-gm20b", device->handle, &gpu.client);
	if (!ret) {
		/* every object made below inherits this printer (nvif/object.c: object->parent = parent->parent) */
		nvif_parent_ctor(&tgm_nvif_parent_func, &tgm_nvif_parent);
		gpu.client.object.parent = &tgm_nvif_parent;
		at = "nvif_device_ctor";
		ret = nvif_device_ctor(&gpu.client, "dev", &gpu.dev);
	}
	if (!ret) {
		/* the MMU class is a child of the device object, as nouveau_drm.c creates it (on the client: -22) */
		at = "nvif_mmu_ctor";
		ret = nvif_mmu_ctor(&gpu.dev.object, "mmu", NVIF_CLASS_MMU_GF100, &gpu.mmu);
	}
	if (!ret) {
		at = "nvif_mmu_type";
		gpu.mem_type = nvif_mmu_type(&gpu.mmu, NVIF_MEM_HOST | NVIF_MEM_KIND);
		if (gpu.mem_type < 0)
			gpu.mem_type = nvif_mmu_type(&gpu.mmu, NVIF_MEM_HOST);
		ret = gpu.mem_type < 0 ? gpu.mem_type : 0;
	}
	if (ret) {
		scnprintf(line, len, "M2 FAIL %s %d (GR came up: see the M2 ok line above)", at,
			  ret);
	}
	return ret;
}

void tgm_get_info(struct tgm_info *info)
{
	u64 units = gpu.stage >= 2 ? nvkm_gr_units(gpu.device->gr) : 0;
	memset(info, 0, sizeof(*info));
	if (!gpu.device)
		return;
	info->boot0 = gpu.boot0;
	info->chipset = gpu.device->chipset;
	info->gpc_count = units & 0xff;
	info->tpc_count = (units >> 8) & 0xffff;
	info->comptag_lines = gpu.device->ltc->num_tags;
}

/* Global, first fit, never removed, as nvgpu's ZBC_SET_TABLE and nvkm's FERMI_A_ZBC_* keep entries; GR and LTC init write
 * them all again after power-off. */
int tgm_zbc_add(int depth, uint32_t format, const uint32_t ds[4], const uint32_t l2[4])
{
	/* gf100_fermi_mthd_zbc_color's formats */
	static const u8 color_formats[] = {0x01, 0x02, 0x04, 0x08, 0x0c, 0x10, 0x14,
					   0x16, 0x18, 0x1c, 0x20, 0x24, 0x28, 0x2c,
					   0x30, 0x34, 0x38, 0x3c, 0x40};
	struct gf100_gr *gr = NULL;
	int ret = -EINVAL;
	unsigned int i = 0;

	if (gpu.stage < 2 || !gpu.device->ltc->num_tags)
		return -ENODEV;
	gr = gf100_gr(gpu.device->gr);
	gpu_lock();
	if (depth && format == FERMI_A_ZBC_DEPTH_V0_FMT_FP32)
		ret = gf100_gr_zbc_depth_get(gr, format, ds[0], l2[0]);
	for (i = 0; !depth && i < ARRAY_SIZE(color_formats); i++) {
		if (format == color_formats[i])
			ret = gf100_gr_zbc_color_get(gr, format, ds, l2);
	}
	mutex_unlock(&gpu.lock);
	return ret;
}

uint64_t tgm_timestamp_ns(void)
{
	u64 t = 0;
	if (gpu.stage < 2)
		return 0;
	mutex_lock(&gpu.lock);
	/* a rail-gated GPU's time is ktime, without a wake: PTIMER restarts from it (stamp_reseed) */
	t = gpu.railed ? (u64)ktime_to_ns(ktime_get()) : nvif_device_time(&gpu.dev);
	stamp_last = t = tgm_stamp_monotonic(stamp_last, t);
	mutex_unlock(&gpu.lock);
	return t;
}

/* Pool BOs are mapped into processes, and a process maps whole VM pages (four pool pages): each BO starts on one and is
 * padded to one, so its mapping never reaches another BO. */
static uint64_t pool_bo_bytes(uint64_t bytes)
{
	const uint64_t vm_page = nvxnu_vm_page_size();
	return (bytes + vm_page - 1) & ~(vm_page - 1);
}

void *tgm_pool_alloc(uint64_t bytes, uint64_t align, uint64_t *pa)
{
	unsigned long long h = 0;
	void *va = nvxnu_pool_alloc_clean(pool_bo_bytes(bytes),
					  max_t(u64, align, nvxnu_vm_page_size()), &h);
	*pa = va ? h : 0;
	return va;
}

void tgm_pool_free_zeroed(void *va, uint64_t bytes, uint64_t pa)
{
	bytes = pool_bo_bytes(bytes); /* the padding too: the owner's mapping covered it */
	memset(va, 0, bytes);
	/* the write-combining buffers drained before the pages are handed out again */
	__asm__ volatile("dsb sy" ::: "memory");
	nvxnu_pool_free_zeroed(pa, bytes);
}

static void scrub_kick(void);

/* Back dirty: the scrubber zeroes it on the GPU (see "pool scrub") unless an allocation takes it first and zeroes it. */
void tgm_pool_free(void *va, uint64_t bytes, uint64_t pa)
{
	dma_free_attrs(&gpu_pdev.dev, pool_bo_bytes(bytes), va, pa, 0);
	scrub_kick();
}

uint64_t tgm_pool_avail(void)
{
	return nvxnu_pool_size() - ((u64)nvxnu_pool_used_pages() << PAGE_SHIFT);
}

/* GPU L2 for user memory:
 * GM20B's L2 caches system memory unless a PTE is volatile; the GPU does not snoop the CPU and the CPU does not see the
 * L2. Volatile user mappings (the default, as nouveau maps all system memory) keep every texture fetch and ROP access
 * out of the L2. Here user mappings are cacheable and the L2 is kept coherent at submission boundaries,
 * which are Metal's visibility points:
 *  - CPU -> GPU: each submission starts with a host MEM_OP_D L2_SYSMEM_INVALIDATE in its own entry (NVK's host-write
 *    barrier on Maxwell B, nvk_cmd_buffer.c HOST_WFI_INVALIDATE_SYSMEM; the channel is idle there, after the previous
 *    submission's fence WFI), so what the CPU wrote since (WC pool BOs, cleaned cached BOs, imports, push buffers) is read
 *    from DRAM. The host finishes a MEM_OP before it processes the next method, and the submission's first push is
 *    fetched only after that (GP_ENTRY1 SYNC_WAIT, nvif's no_prefetch). The invalidate is ordered on the GPU: the
 *    submitting thread does not wait for it.
 *  - GPU -> CPU: the kernel's fence push writes the L2's dirty lines back (host WFI, FB_FLUSH, MEM_OP_D L2_FLUSH_DIRTY,
 *    FB_FLUSH; clb06f.h: gm20x moved MEM_OP_A/B to C/D) before it releases the seqno, so anyone who sees a fence complete
 *    (WaitFence, the client's mapped fence page, sync_for_cpu) finds the data in DRAM.
 *  - Pages leave the GPU only after their fences passed (BoFree's zombies, channel teardown), so no dirty line of a
 *    page the kernel gives back is written back later; nvgpu flushes on unmap for the same reason
 *    (common/mm/gmmu.c gk20a_gmmu_cache_maint_unmap). nvkm invalidates the L2 after its own page-table writes
 *    (instmem/gk20a.c release). Kernel mappings (rings, fence pages) stay volatile. */
static int l2_on(void)
{
	return 1;
}

struct tgm_vm {
	struct nvif_vmm vmm;
	struct list_head kmaps;
	u64 fences_va; /* every connection's fence page, read-only (tgm_chan_order's acquires), 0: not mapped */
	struct list_head
		imports; /* struct tgm_import: first pages of the host imports made in it (live_lock) */
	int imports_lost;         /* one could not be recorded: taken as importing every page */
	struct tgm_tlb_space tlb; /* GPU-ordered maps (TegraGM20BTlb.h) */
	const void *owner;        /* tgm_vm_claim: the connection whose channels alone address it */
};

struct tgm_import {
	struct list_head head;
	u64 pa;
	u32 refs;
};

struct tgm_mem {
	struct nvif_mem mem;
	u64 bytes;
	u8 page; /* the largest GMMU page it may be mapped with */
};

struct tgm_kmap {
	struct list_head head;
	struct nvif_vma vma;
};

static int vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, int read_only,
		   uint64_t *va);
static int fences_map(struct tgm_vm *vm);

/* The present channel's fence page (see "GPU present"): one pool page, made with the present's address space and kept
 * for good. */
static struct {
	void *va;
	u64 pa;
	struct tgm_mem *mem;
} pfence;

static DEFINE_MUTEX(pfence_lock);

static int pfence_get(struct tgm_vm *vm)
{
	int ret = 0;
	mutex_lock(&pfence_lock);
	if (!pfence.mem) {
		if (!pfence.va)
			pfence.va = tgm_pool_alloc(PAGE_SIZE, 0, &pfence.pa);
		ret = pfence.va ? tgm_mem_new(vm, &pfence.pa, 1, &pfence.mem) : -ENOMEM;
	}
	mutex_unlock(&pfence_lock);
	return ret;
}

int tgm_vm_new(struct tgm_vm **pvm)
{
	/* bigpage selects the big-page size, 16 (64 KiB) or 17 (128 KiB, nouveau's default); anything else is -EINVAL
	 * (vmmgm200.c gm200_vmm_new_; 0 fails M3). 4 KiB pages stay available, and the ABI binds only those. */
	struct gm200_vmm_v0 args = {.version = 0, .bigpage = 17};
	struct tgm_vm *vm = NULL;
	int ret = 0;

	if (gpu.stage < 2)
		return -ENODEV;
	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;
	INIT_LIST_HEAD(&vm->kmaps);
	INIT_LIST_HEAD(&vm->imports);
	tgm_tlb_space_init(&vm->tlb);
	gpu_lock();
	/* RAW: the client manages [0, 1<<39) with raw binds; nvkm allocates in [1<<39, 1<<40) */
	ret = nvif_vmm_ctor(&gpu.mmu, "uvmm", NVIF_CLASS_VMM_GM200, RAW, TGM_KERNEL_VA,
			    TGM_VA_LIMIT - TGM_KERNEL_VA, &args, sizeof(args), &vm->vmm);
	mutex_unlock(&gpu.lock);
	if (ret) {
		kfree(vm);
		return ret;
	}
	/* for the task order's acquires (tgm_chan_order); without the view, Order refuses and the client orders itself */
	if (fences_map(vm))
		vm->fences_va = 0;
	*pvm = vm;
	return 0;
}

/* gpu.lock held: the GPU L2's page-table lines, then every address space's TLB (gf100_vmm_invalidate with ALL_PDB),
 * synchronously. */
static void mmu_invalidate_all(void)
{
	struct nvkm_device *device = gpu.device;

	nvkm_ltc_invalidate(device->ltc);
	mutex_lock(&device->mmu->mutex);
	/* nouveau's nvkm_msec assigns in its polling loop's condition. */
	/* NOLINTNEXTLINE(bugprone-assignment-in-if-condition) */
	if (nvkm_msec(device, 2000, if (nvkm_rd32(device, 0x100c80) & 0x00ff0000) break;) < 0) {
		nvxnu_panic("tegra-gm20b: MMU invalidate: no free slot after 2 s (0x100c80 %08x)",
			    nvkm_rd32(device, 0x100c80));
	}
	nvkm_wr32(device, 0x100cbc, 0x80000000 | 0x00000003); /* PAGE_ALL | ALL_PDB */
	/* nouveau's nvkm_msec assigns in its polling loop's condition. */
	/* NOLINTNEXTLINE(bugprone-assignment-in-if-condition) */
	if (nvkm_msec(device, 2000, if (nvkm_rd32(device, 0x100c80) & 0x00008000) break;) < 0) {
		nvxnu_panic(
			"tegra-gm20b: MMU invalidate not acknowledged after 2 s (0x100c80 %08x)",
			nvkm_rd32(device, 0x100c80));
	}
	mutex_unlock(&device->mmu->mutex);
}

void tgm_vm_del(struct tgm_vm **pvm)
{
	struct tgm_vm *vm = *pvm;
	struct tgm_kmap *k = NULL;
	struct tgm_kmap *t = NULL;
	struct tgm_import *im = NULL;
	struct tgm_import *it = NULL;
	if (!vm)
		return;
	list_for_each_entry_safe(im, it, &vm->imports, head) /* no channel of it is left to look */
		kfree(im);
	gpu_lock();
	list_for_each_entry_safe(k, t, &vm->kmaps, head)
	{
		nvif_vmm_put(&vm->vmm, &k->vma);
		list_del(&k->head);
		kfree(k);
	}
	nvif_vmm_dtor(&vm->vmm);
	mmu_invalidate_all(); /* a later space may reuse this one's page directory page */
	mutex_unlock(&gpu.lock);
	kfree(vm);
	*pvm = NULL;
}

void tgm_vm_claim(struct tgm_vm *vm, const void *owner)
{
	vm->owner = owner;
}

static int stats_on(void);

static int mem_new(const uint64_t *pas, uint32_t npages, u8 page, struct tgm_mem **pmem,
		   uint64_t *lock_wait_ns)
{
	struct nvif_mem_ram_v0 args = {.version = 0, .dma = (dma_addr_t *)pas, .sgl = NULL};
	struct tgm_mem *mem = NULL;
	u64 asked = 0;
	int ret = 0;
	if (gpu.stage < 2)
		return -ENODEV;
	mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	if (!mem)
		return -ENOMEM;
	mem->bytes = (u64)npages << PAGE_SHIFT;
	mem->page = page;
	asked = stats_on() && lock_wait_ns ? nvxnu_nanotime() : 0;
	gpu_lock();
	if (asked)
		*lock_wait_ns = nvxnu_nanotime() - asked;
	ret = nvif_mem_ctor_type(&gpu.mmu, "bo", NVIF_CLASS_MEM_GF100, gpu.mem_type, page,
				 mem->bytes, &args, sizeof(args), &mem->mem);
	mutex_unlock(&gpu.lock);
	if (ret) {
		kfree(mem);
		return ret;
	}
	*pmem = mem;
	return 0;
}

int tgm_mem_new(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages, struct tgm_mem **pmem)
{
	(void)vm;
	return mem_new(pas, npages, PAGE_SHIFT, pmem, NULL);
}

int tgm_mem_new_waited(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages,
		       struct tgm_mem **pmem, uint64_t *lock_wait_ns)
{
	(void)vm;
	return mem_new(pas, npages, PAGE_SHIFT, pmem, lock_wait_ns);
}

/* Comptags belong to the physical pages, yet nvkm keeps them on the memory object (nvkm/subdev/mmu/uvmm.c): two objects
 * over the same pages would give them two sets of lines, and a GPU access through one would misread what the other
 * compressed. Only memory whose pages no other object names may compress, so one object per physical page holds its
 * lines and every map of it, in any space, shares them. Imports (an IOSurface's pages, wired once per task and
 * imported by every process that draws into it) never qualify. */
int tgm_mem_new_exclusive(struct tgm_vm *vm, const uint64_t *pas, uint32_t npages,
			  struct tgm_mem **pmem)
{
	const int comp = gpu.stage >= 2 && gpu.device->ltc->num_tags;
	(void)vm;
	return mem_new(pas, npages, comp ? tgm_comp_page_shift(pas, npages) : PAGE_SHIFT, pmem,
		       NULL);
}

void tgm_mem_del(struct tgm_mem **pmem)
{
	if (!*pmem)
		return;
	gpu_lock();
	nvif_mem_dtor(&(*pmem)->mem);
	mutex_unlock(&gpu.lock);
	kfree(*pmem);
	*pmem = NULL;
}

/* Sysmem is mapped volatile (GPU L2 does not keep its lines): CONTRACT.md §3(b) option one; cacheable with
 * l2_on(). A compressible kind maps with 128 KiB pages and the memory's comptags (TegraGM20BComp.h) or is refused: a map
 * demoted to its uncompressed kind, as nvgpu demotes one, would look compressed to its caller, who would go on clearing
 * and resolving it as compressed. */
static int vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		  uint64_t offset, uint8_t kind, int read_only, int vol, int gpu_ordered,
		  uint8_t *page_shift)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = !!vol, .ro = !!read_only, .priv = 0, .kind = kind};
	u8 shift = PAGE_SHIFT;
	int ret = 0;
	int got = 0;
	if (va < TGM_USER_VA_START || range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range ||
	    offset > mem->bytes || range > mem->bytes - offset ||
	    !nvif_mmu_kind_valid(&gpu.mmu, kind))
		return -EINVAL;
	if (gpu.mmu.kind[kind] != kind) {
		if (!tgm_comp_bind_ok(va, offset, range, mem->page))
			return -EINVAL;
		shift = TGM_COMP_SHIFT;
	}
	gpu_lock();
	if (gpu_ordered)
		nvxnu_gpu_ordered_begin();
	ret = nvif_vmm_raw_get(&vm->vmm, va, range, shift);
	got = !ret;
	if (got) {
		ret = nvif_vmm_raw_map(&vm->vmm, va, range, shift, &args, sizeof(args), &mem->mem,
				       offset);
	}
	if (gpu_ordered)
		nvxnu_gpu_ordered_end();
	if (gpu_ordered && !ret)
		tgm_tlb_mapped(&vm->tlb);
	if (ret) {
		/* removals are synchronous (TegraGM20BTlb.h): a failed get may have dropped page tables it made */
		if (got) {
			nvif_vmm_raw_put(&vm->vmm, va, range, shift);
		} else if (gpu_ordered) {
			mmu_invalidate_all();
		}
	}
	mutex_unlock(&gpu.lock);
	if (!ret && page_shift)
		*page_shift = shift;
	return ret;
}

int tgm_vm_map(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem, uint64_t offset,
	       uint8_t kind, int read_only)
{
	return vm_map(vm, va, range, mem, offset, kind, read_only, !l2_on(), 0, NULL);
}

int tgm_vm_map_ordered(struct tgm_vm *vm, uint64_t va, uint64_t range, struct tgm_mem *mem,
		       uint64_t offset, uint8_t kind, int read_only, uint8_t *page_shift)
{
	if (!vm->owner)
		return -EINVAL;
	return vm_map(vm, va, range, mem, offset, kind, read_only, !l2_on(), 1, page_shift);
}

int tgm_vm_unmap_pages(struct tgm_vm *vm, uint64_t va, uint64_t range, uint8_t page_shift)
{
	int ret = 0;
	if (va < TGM_USER_VA_START || range > TGM_KERNEL_VA || va > TGM_KERNEL_VA - range)
		return -EINVAL;
	gpu_lock();
	ret = nvif_vmm_raw_unmap(&vm->vmm, va, range, page_shift, false);
	nvif_vmm_raw_put(&vm->vmm, va, range, page_shift);
	mutex_unlock(&gpu.lock);
	return ret;
}

int tgm_vm_unmap(struct tgm_vm *vm, uint64_t va, uint64_t range)
{
	return tgm_vm_unmap_pages(vm, va, range, PAGE_SHIFT);
}

int tgm_vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, uint64_t *va)
{
	return vm_kmap(vm, mem, bytes, 0, va);
}

static int vm_kmap(struct tgm_vm *vm, struct tgm_mem *mem, uint64_t bytes, int read_only,
		   uint64_t *va)
{
	struct gf100_vmm_map_v0 args = {
		.version = 0, .vol = 1, .ro = !!read_only, .priv = 0, .kind = 0};
	struct tgm_kmap *k = kzalloc(sizeof(*k), GFP_KERNEL);
	int ret = 0;
	if (!k)
		return -ENOMEM;
	gpu_lock();
	ret = nvif_vmm_get(&vm->vmm, ADDR, false, PAGE_SHIFT, 0, bytes, &k->vma);
	if (!ret) {
		ret = nvif_vmm_map(&vm->vmm, k->vma.addr, bytes, &args, sizeof(args), &mem->mem, 0);
		if (ret)
			nvif_vmm_put(&vm->vmm, &k->vma);
	}
	if (!ret)
		list_add(&k->head, &vm->kmaps);
	mutex_unlock(&gpu.lock);
	if (ret) {
		kfree(k);
		return ret;
	}
	*va = k->vma.addr;
	return 0;
}

#define CHAN_PUSH_BYTES 0x10000  /* nouveau_channel_ctor: plength */
#define CHAN_GPFIFO_BYTES 0x2000 /* ilength: 1024 entries */
#define CHAN_SEMA_BYTES \
	0x2000 /* nvif's GET semaphore at +0; stats stamps at CHAN_STAMPS, push profile at CHAN_PROF */
#define CHAN_BYTES (CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES + CHAN_SEMA_BYTES)
#define CHAN_STAMPS (CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES + 0x800)
#define CHAN_PROF                              \
	(CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES + \
	 0x1000) /* one 16-byte stamp per push, TGM_PROF_MAX */
#define CHAN_QUEUED 8
#define CHAN_NSTAMPS \
	64 /* by seqno; one record = two 16-byte semaphore releases (payload, 0, PTIMER ns) */

/* What a 16-byte semaphore release writes (NV906F SEMAPHORED RELEASE_SIZE_16BYTE) */
struct tgm_stamp {
	u32 payload, pad;
	u64 ns;
};

struct tgm_stamps {
	struct tgm_stamp start,
		end; /* the GPU starts the submission (WFI: the one before finished) / finished it */
};

/* An acquire tgm_chan_order queued for a channel's next submission: another channel's fence word reaching seqno. */
struct tgm_order {
	u64 va;                  /* the source's fence word in the waiter's address space */
	const volatile u32 *cpu; /* and in the kernel's */
	u64 id;                  /* the source channel */
	u32 seqno;
	u32 chid; /* the source's hardware channel */
};

struct tgm_chan {
	struct tgm_vm *vm;
	struct tgm_tlb_chan tlb; /* the space's GPU-ordered maps this channel has invalidated for */
	struct nvif_object user;
	struct nvif_object obj[5];
	struct nvif_event kill;
	struct nvif_event nonstall;
	struct nvif_chan chan;
	void *ring; /* push | gpfifo | sema, pool memory */
	u64 ring_pa, ring_va;
	u64 *ring_pas;
	struct tgm_mem *ring_mem;
	struct nvif_vma ring_vma;
	volatile u32 *fence_cpu;
	u64 fence_gpu;
	u32 submitted;
	u32 error;
	u32 chid;
	int nobj;
	int kernel; /* a kernel channel, no client waits on it and the present does not wait for it: CHAN_PRESENT, CHAN_SCRUB */
	int critical; /* the compositor's or the kernel's: the high timeslice */
	int high; /* the compositor's or the present's: high on the runlist (TegraGM20BSched.h) */
	int isolating; /* client fault isolation: its error or hang was taken as its own, the kill is under way */
	u64 isolated_ns;
	struct list_head live; /* live_chans, for tegra_gm20b_sync_for_cpu */
	u64 id;
	char owner[17];  /* process that created it */
	char fault[200]; /* the nvkm report it was isolated for */
	u64 fault_va;    /* error 1: the VA its MMU fault report names */
	u64 submit_ns;   /* when the last fence was kicked */
	u64 busy_ns;     /* hang_recover: when it last went from idle to work pending */
	u64 progress_ns; /* and when its fence last moved, to progress_done */
	/* the hold's time on it (tgm_hold_held_ns) at the later of those two */
	u64 progress_held;
	u32 progress_done;
	u32 stall_done; /* tgm_chan_check_progress: completed seqno first seen stalled, and when */
	u64 stall_ns, stall_held;
	int app;     /* a user channel: the hold may take it from its first submission */
	int tracked; /* the hold may take it (tgm_hold_track) */
	u32 timeslice_due; /* RAMFC timeslice for a level change, written once the host holds the channel no more */

	/* high channels, by seqno: each submission's kick, the app channel it was queued behind and when that one was asked
	 * to leave the GPU (PTIMER), whether an app held the GPU at its kick, whether acquires came before its start stamp */
	struct {
		u64 kick_ns, asked_pt;
		s16 behind;
		u8 app, waits;
	} queued[CHAN_QUEUED];

	u32 gov_done;   /* idle clock: last seqno counted retired (gov_fenced) */
	u32 woken_done; /* the completed seqno its waiters were last woken for (chan_wake_waiters) */
	void (*fenced)(void *arg); /* tgm_chan_on_fence */
	void *fenced_arg;
	u32 stat_done; /* stats: last seqno whose latency was counted, submit times by seqno */
	u64 stat_sub_ns[CHAN_NSTAMPS];
	u64 stat_sub_ptimer[CHAN_NSTAMPS];
	u8 stat_kick[CHAN_NSTAMPS]; /* stats: what each high kick found (tgm_sched_kick) */
	u64 stat_exec,
		stat_exec_max; /* stats: this window's stamped exec time (PTIMER units), for its owner */
	u32 stat_exec_n;
	/* push profile: the profiled submission in flight (0: none), its pushes' classes, and this window's GPU time by
	 * class (PTIMER units) and the pushes counted, for its owner */
	u32 prof_seqno, prof_count, prof_subs;
	u8 prof_cls[TGM_PROF_MAX];
	u16 prof_draws[TGM_PROF_MAX], prof_sdraws[TGM_PROF_MAX];
	u64 prof_ns[TGM_PC_COUNT];
	u32 prof_n[TGM_PC_COUNT];
	u64 prof_dn[TGM_PC_COUNT],
		prof_sdn[TGM_PC_COUNT]; /* draws, and draws shaded per sample, by class */
	/* tgm_chan_order: its connection's task (tgm_chan_set_group), the acquires queued for its next submission
	 * (live_lock), and the last one kicked (a hang's line names it while it has not passed) */
	const void *group;
	u32 norder;
	struct tgm_order order[TGM_ORDER_MAX];
	struct tgm_order waited;
	struct tgm_hang_log hang;
};

#define CHAN_PRESENT 1 /* the GPU present's channel */
#define CHAN_SCRUB 2   /* the pool scrubber's */

/* Every user channel, so a CPU reader outside the GPU client can wait for all GPU work (tegra_gm20b_sync_for_cpu). */
/* An empty list head points at itself. */
/* NOLINTNEXTLINE(misc-static-initialization-cycle) */
static LIST_HEAD(live_chans);
static DEFINE_MUTEX(live_lock);
static u64 live_ids;

/* Host imports per address space, by first physical page: who can write a surface the GPU present copies (only an
 * address space that maps its pages; an IOSurface is imported from its page-aligned base, see "GPU present"). */
void tgm_vm_import_note(struct tgm_vm *vm, uint64_t pa, int add)
{
	struct tgm_import *im = NULL;

	mutex_lock(&live_lock);
	list_for_each_entry(im, &vm->imports, head)
	{
		if (im->pa == pa)
			break;
	}
	if (&im->head == &vm->imports) {
		im = add ? kzalloc(sizeof(*im), GFP_KERNEL) : NULL;
		if (im) {
			im->pa = pa;
			list_add(&im->head, &vm->imports);
		} else if (add) {
			vm->imports_lost = 1;
		}
	}
	if (im && add) {
		im->refs++;
	} else if (im && !--im->refs) {
		list_del(&im->head);
		kfree(im);
	}
	mutex_unlock(&live_lock);
}

static int vm_imports(struct tgm_vm *vm, u64 pa) /* live_lock held */
{
	struct tgm_import *im = NULL;
	if (vm->imports_lost)
		return 1;
	list_for_each_entry(im, &vm->imports, head)
	{
		if (im->pa == pa)
			return 1;
	}
	return 0;
}

uint32_t tgm_chan_submitted(struct tgm_chan *chan)
{
	return chan->submitted;
}

uint32_t tgm_chan_completed(struct tgm_chan *chan)
{
	return READ_ONCE(*chan->fence_cpu);
}

uint32_t tgm_chan_error(struct tgm_chan *chan)
{
	return chan->error;
}

uint64_t tgm_chan_fault_va(struct tgm_chan *chan)
{
	return READ_ONCE(chan->fault_va);
}

uint32_t tgm_chan_waiter(struct tgm_chan *chan)
{
	return (uint32_t)(chan->id % TGM_WAITERS);
}

static void chan_wake_all_waiters(struct tgm_chan *chan)
{
	u32 waiter = tgm_chan_waiter(chan);

	if (stats_on()) {
		__atomic_store_n(&waiter_woken_ns[waiter], nvxnu_nanotime(), __ATOMIC_RELAXED);
		__atomic_fetch_add(&wstat.wakes, 1, __ATOMIC_RELAXED);
	}
	nvxnu_word_wake(&waiter_words[waiter]);
}

/* One line of what the GPU is doing about a channel: owner, hardware channel id, error, completed/submitted fence and
 * how long ago the last one was kicked; GPFIFO GET/PUT as the host sees them (USERD 0x88/0x8c) and the PUT the kernel
 * wrote; PGRAPH_STATUS (0x400700, bit 0 busy), PFIFO engine 0 (GR) status (0x2640); PBDMA 0: status (0x3080, bits 13-15
 * busy, gk104_runq_idle), the channel it serves (0x40120) and the method/data it is on (0x400c0/0x400c4: subchannel
 * 18:16, method 13:2, as gf100_runq_intr decodes them; a NV906F semaphore method there means a semaphore acquire that is
 * not satisfied); PMC_INTR_0; and the memory controller's last refused access (MC_INTSTATUS, MC_ERR_STATUS, MC_ERR_ADR). */
static void chan_describe(struct tgm_chan *chan, char *buf, unsigned int len)
{
	struct nvkm_device *device = gpu.device;
	volatile u32 *userd = chan->user.map.ptr;
	const u32 eng = nvkm_rd32(device, 0x002640);
	u32 mc_int = 0;
	u32 mc_err = 0;
	u32 mc_adr = 0;
	char engine[48];
	char note[112];

	tgm_hang_engine(eng, engine, sizeof(engine));
	tgm_hang_note(&chan->hang, tgm_chan_completed(chan) + 1, chan->submitted,
		      chan->chan.gpfifo.map.ptr, chan->chan.gpfifo.max, chan->ring_va,
		      CHAN_PUSH_BYTES, chan->vm->owner, note, sizeof(note));
	tegra_gm20b_mc_errors(&mc_int, &mc_err, &mc_adr);
	scnprintf(
		buf, len,
		"%s ch %u err %u fence %u/%u kicked %llu ms ago%s, gp %u/%u (put %u), PGRAPH %08x "
		"ENG0 %08x (%s), "
		"PBDMA0 %08x ch %u mthd %08x data %08x, PMC %08x, MC %08x err %08x at %08x",
		chan->owner[0] ? chan->owner : "?", chan->chid, chan->error,
		tgm_chan_completed(chan), chan->submitted,
		chan->submit_ns ? (nvxnu_nanotime() - chan->submit_ns) / 1000000 : 0ULL, note,
		userd ? userd[0x88 / 4] : 0, userd ? userd[0x8c / 4] : 0, chan->chan.gpfifo.cur,
		nvkm_rd32(device, 0x400700), eng, engine, nvkm_rd32(device, 0x003080),
		nvkm_rd32(device, 0x040120) & 0xfff, nvkm_rd32(device, 0x0400c0),
		nvkm_rd32(device, 0x0400c4), nvkm_rd32(device, 0x000100), mc_int, mc_err, mc_adr);
}

void tgm_chan_hung(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
{
	char state[TGM_HANG_LINE];
	char order[64] = "";
	const struct tgm_order *w = &chan->waited;
	chan_describe(chan, state, sizeof(state));
	/* still waiting for another channel of its task (tgm_chan_order) */
	if (w->cpu && (s32)(READ_ONCE(*w->cpu) - w->seqno) < 0) {
		scnprintf(order, sizeof(order), ", order wait on chan %llu fence %u (at %u)", w->id,
			  w->seqno, READ_ONCE(*w->cpu));
	}
	nvxnu_panic("tegra-gm20b: %s: fence %u still pending after %llu ms and no GPU error "
		    "reported: %s%s",
		    who, seqno, waited_ns / 1000000, state, order);
}

#define TGM_HANG_NS (2 * NSEC_PER_SEC)

/* The time the hold kept the channel off the GPU is not the channel's: it never counts toward a stall. */
void tgm_chan_check_progress(struct tgm_chan *chan, const char *who, uint32_t seqno)
{
	u32 done = tgm_chan_completed(chan);
	u64 now = nvxnu_nanotime();
	u64 since = READ_ONCE(chan->stall_ns);
	u64 held = tgm_hold_held_ns(chan->chid, now);
	u64 stalled = 0;

	if ((s32)(done - chan->submitted) >= 0) {
		WRITE_ONCE(chan->stall_ns, 0);
	} else if (!since || READ_ONCE(chan->stall_done) != done) {
		WRITE_ONCE(chan->stall_done, done);
		WRITE_ONCE(chan->stall_held, held);
		WRITE_ONCE(chan->stall_ns, now);
	} else {
		stalled = tgm_unheld_ns(now, since, held, READ_ONCE(chan->stall_held));
		if (stalled >= TGM_HANG_NS) {
			tgm_chan_stalled(chan, who, seqno, stalled);
			WRITE_ONCE(chan->stall_held, held);
			WRITE_ONCE(chan->stall_ns, now);
		}
	}
}

/* client fault isolation:
 * With every process on the GPU, one app's bad shader or stale address must not take the system down. nvgpu's policy is
 * taken (gk20a_fifo_recover, gk20a_gr_isr: the faulting channel gets an error and only it is torn down): after an
 * interrupt pass in which nvkm reported errors (xnu_glue.cpp gpuInterrupt), errors_contained() takes the channels the
 * reports name ("on channel N [" for MMU faults, "ch N" for PBDMA errors and GR exceptions) and those nvkm put into
 * recovery in that pass without naming them. A context-switch timeout (a shader that never ends, a switch that never
 * drains) is a tick recover.c waits out until the engine's channel is stuck (tgm_ctxsw_stuck), then recovers as nvkm does
 * (gf100_fifo_intr_ctxsw_timeout: the engine faulted off it). If each is a client channel
 * (the compositor's too: Core Animation makes its resources again after an error, as on iOS), the errors are that
 * client's: nvkm's recovery (nvkm_runl_rc_cgrp: runlist blocked and preempted, the channel disabled and removed, an
 * engine still on it reset; nvkm starts it itself for MMU faults, errors_contained() for the rest, which nvkm only
 * reports) kills each one, chan_killed() marks it dead and releases its fence word so no acquire or CPU wait stays on
 * it, and the GPU goes on. A hang with no error report (a wait that ran out) is recovered the same way by
 * hang_recover(), from what the GPU itself is stuck on. Everything else panics: an error on a kernel channel, a report
 * naming no channel or an unknown one (one naming none is taken as fallout while a recovery is under way), a kill not
 * done TGM_ISOLATE_NS after its report, and a hang on a kernel channel or with nothing progressing (tgm_chan_hung). */
#define TGM_ISOLATE_NS (500 * NSEC_PER_MSEC)

int tgm_all_clients(void)
{
	return 1; /* every process that asks gets a device */
}

static struct {
	void *deadline; /* iso_deadline */
	u64 until;      /* end of the recovery window, 0: none (live_lock) */
} iso;

/* CONTRACT.md error codes: 1 MMU fault, 2 GR exception, 3 host error, 4 timeout (a hang, hang_recover) */
#define HANG_REPORT "hang: "

static u32 fault_code(const char *report)
{
	if (!strncmp(report, HANG_REPORT, strlen(HANG_REPORT)) || strstr(report, "CTXSW_TIMEOUT"))
		return 4;
	if (strstr(report, "fault "))
		return 1;
	if (strstr(report, "TRAP") || strstr(report, "ILLEGAL_") || strstr(report, "DATA_ERROR"))
		return 2;
	return 3;
}

static void fence_kill_record(volatile u32 *fence_cpu, u32 submitted, u32 error)
{
	volatile struct TegraGM20BAbiFence *fence = (volatile struct TegraGM20BAbiFence *)fence_cpu;

	WRITE_ONCE(fence->killed_first, READ_ONCE(fence->completed) + 1);
	WRITE_ONCE(fence->killed_last, submitted);
	__asm__ volatile("dsb sy" ::: "memory");
	WRITE_ONCE(fence->error, error);
	__asm__ volatile("dsb sy" ::: "memory");
}

void tgm_fence_kill_clear(volatile uint32_t *fence_cpu)
{
	volatile struct TegraGM20BAbiFence *fence = (volatile struct TegraGM20BAbiFence *)fence_cpu;

	WRITE_ONCE(fence->error, 0);
	WRITE_ONCE(fence->killed_first, 0);
	WRITE_ONCE(fence->killed_last, 0);
	__asm__ volatile("dsb sy" ::: "memory");
}

/* nvkm killed the channel (nvkm_chan_error, from its recovery work). Before M3, M3 reads chan->error. From M3 on: an
 * isolated client channel is dead (see above); any other panics, with the report that started it if the interrupt pass
 * did not panic first. */
static int chan_killed(struct nvif_event *event, void *repv, u32 repc)
{
	struct tgm_chan *chan = container_of(event, struct tgm_chan, kill);
	const char *report = chan->fault[0] ? chan->fault : nvxnu_gpu_errors();
	char state[TGM_HANG_LINE];

	(void)repv;
	(void)repc;
	if (gpu.stage >= 3 && (chan->kernel || !tgm_all_clients())) {
		chan_describe(chan, state, sizeof(state));
		nvxnu_panic("tegra-gm20b: channel killed by nvkm recovery after \"%s\": %s",
			    nvxnu_gpu_errors(), state);
	}
	if (gpu.stage < 3) {
		if (!chan->error)
			chan->error = 3;
		return NVIF_EVENT_DROP;
	}
	/* the runlist dropped it, so nothing writes its fence any more: release what it will never complete, after the
	 * record of what that was (TegraGM20BAbiFence), so a client that sees the fence pass sees the record too */
	if (fault_code(report) == 1)
		WRITE_ONCE(chan->fault_va, tgm_fault_va(report, (int)chan->chid));
	WRITE_ONCE(chan->error, fault_code(report));
	fence_kill_record(chan->fence_cpu, chan->submitted, chan->error);
	WRITE_ONCE(*chan->fence_cpu, chan->submitted);
	__asm__ volatile("dsb sy" ::: "memory");
	nvxnu_log("tegra-gm20b: GPU error in %s (channel %u): channel killed, GPU running: %s\n",
		  chan->owner, chan->chid, report);
	nvxnu_wake();
	chan_wake_all_waiters(chan);
	if (READ_ONCE(chan->fenced))
		chan->fenced(chan->fenced_arg);
	return NVIF_EVENT_DROP;
}

/* The next channel id a report names after p ("channel N [" or "ch N"), -2 when there is none; -1 is nvkm's "unknown". */
static const char *next_chid(const char *p, int *chid)
{
	static const char *const tags[] = {"channel ", "ch "};
	const char *best = NULL;
	const char *at = NULL;
	unsigned int i = 0;
	unsigned int len = 0;

	for (i = 0; i < ARRAY_SIZE(tags); i++) {
		for (at = p; (at = strstr(at, tags[i])); at++) {
			if (at == p || at[-1] == ' ' || at[-1] == '|')
				break;
		}
		if (at && (!best || at < best)) {
			best = at;
			len = strlen(tags[i]);
		}
	}
	if (!best)
		return NULL;
	best += len;
	*chid = -2;
	if (*best == '-' || (*best >= '0' && *best <= '9'))
		*chid = (int)simple_strtol(best, (char **)&best, 10);
	return best;
}

/* 1 if nvkm marked the channel's group for recovery (nvkm_runl_rc_cgrp). */
static int chan_marked(u32 chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	unsigned long flags = 0;
	int marked = 0;

	nvkm_runl_foreach(runl, gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (!nchan)
			continue;
		marked = atomic_read(&nchan->cgrp->rc) != NVKM_CGRP_RC_NONE;
		nvkm_chan_put(&nchan, flags);
		break;
	}
	return marked;
}

/* 1 if every report of the pass is a context-switch timeout tick: recover.c decides on those (tgm_ctxsw_stuck). */
static int only_ctxsw_ticks(const char *errors)
{
	const char *p = errors;
	const char *bar = NULL;
	const char *tick = NULL;

	for (;;) {
		bar = strstr(p, " | ");
		tick = strstr(p, "CTXSW_TIMEOUT");
		if (!tick || (bar && tick > bar))
			return 0;
		if (!bar)
			return 1;
		p = bar + 3;
	}
}

static void chan_recover(u32 chid)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	unsigned long flags = 0;

	nvkm_runl_foreach(runl, gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (nchan) {
			nvkm_runl_rc_cgrp(nchan->cgrp); /* a no-op if nvkm already started it */
			nvkm_chan_put(&nchan, flags);
			return;
		}
	}
}

/* live_lock held */
static void chan_isolate(struct tgm_chan *chan, const char *report, u64 now)
{
	if (chan->isolating || chan->error)
		return;
	chan->isolating = 1;
	chan->isolated_ns = now;
	tgm_hold_drop(chan->chid);
	strscpy(chan->fault, report, sizeof(chan->fault));
	chan_recover(chan->chid);
	iso.until = now + TGM_ISOLATE_NS;
	nvxnu_work_schedule_after(iso.deadline, TGM_ISOLATE_NS);
}

/* From the GPU interrupt work loop after a pass with error reports (nvxnu_errors_fatal's filter): 1 if they stay with
 * client channels, whose kills are then under way; 0 to panic. */
static int errors_contained(const char *errors)
{
	struct tgm_chan *named[8];
	struct tgm_chan *chan = NULL;
	struct tgm_chan *found = NULL;
	const char *p = errors;
	unsigned int n = 0;
	unsigned int i = 0;
	int contained = 1;
	int chid = 0;
	u64 now = nvxnu_nanotime();

	mutex_lock(&live_lock);
	while (contained) {
		p = next_chid(p, &chid);
		if (!p)
			break;
		if (chid == -2)
			continue; /* "ch" with no number: not an attribution */
		found = NULL;
		list_for_each_entry(chan, &live_chans, live)
		{
			if ((int)chan->chid == chid) {
				found = chan;
				break;
			}
		}
		if (!found || found->kernel)
			contained = 0;
		for (i = 0; contained && i < n && named[i] != found; i++)
			;
		if (contained && i == n && n < ARRAY_SIZE(named))
			named[n++] = found;
	}
	/* channels nvkm put into recovery itself, not yet dead or isolated */
	list_for_each_entry(chan, &live_chans, live)
	{
		if (!contained || chan->error || chan->isolating || !chan_marked(chan->chid))
			continue;
		for (i = 0; i < n && named[i] != chan; i++)
			;
		if (chan->kernel) {
			contained = 0;
		} else if (i == n && n < ARRAY_SIZE(named)) {
			named[n++] = chan;
		}
	}
	if (!n && (!iso.until || now >= iso.until) && !only_ctxsw_ticks(errors))
		contained = 0;
	for (i = 0; contained && i < n; i++)
		chan_isolate(named[i], errors, now);
	mutex_unlock(&live_lock);
	return contained;
}

static void iso_deadline(void *arg)
{
	struct tgm_chan *chan = NULL;
	char state[TGM_HANG_LINE];
	u64 now = nvxnu_nanotime();

	(void)arg;
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		if (chan->isolating && !READ_ONCE(chan->error) &&
		    now - chan->isolated_ns >= TGM_ISOLATE_NS) {
			chan_describe(chan, state, sizeof(state));
			nvxnu_panic("tegra-gm20b: GPU error in %s not recovered %llu ms after it "
				    "(\"%s\"): %s",
				    chan->owner,
				    (unsigned long long)(TGM_ISOLATE_NS / NSEC_PER_MSEC),
				    chan->fault, state);
		}
	}
	if (iso.until && now >= iso.until)
		iso.until = 0;
	mutex_unlock(&live_lock);
}

static u64 chan_progress_ns(struct tgm_chan *chan)
{
	return max(READ_ONCE(chan->progress_ns), READ_ONCE(chan->busy_ns));
}

/* The hold's time since the channel's last progress is left out, as tgm_chan_check_progress leaves it out. */
static int chan_stuck(struct tgm_chan *chan, u64 now)
{
	u64 since = chan_progress_ns(chan);
	u64 unheld = tgm_unheld_ns(now, since, tgm_hold_held_ns(chan->chid, now),
				   READ_ONCE(chan->progress_held));

	return tgm_stuck(READ_ONCE(chan->error) != 0, tgm_chan_completed(chan),
			 READ_ONCE(chan->submitted), since + unheld, since);
}

/* live_lock held; by id, else (id 0) by hardware channel */
static struct tgm_chan *live_by(u64 id, int chid)
{
	struct tgm_chan *chan = NULL;

	list_for_each_entry(chan, &live_chans, live)
	{
		if (id ? chan->id == id : (int)chan->chid == chid)
			return chan;
	}
	return NULL;
}

/* A wait on `late` (the present's or the scrubber's channel when its own work is late) ran out with no GPU error
 * reported. The channel to blame (TegraGM20BRecover.h tgm_hang_blame) is the first stuck one of what the engines and
 * PBDMAs are on (a context switch that does not finish blames the context nvkm's own recovery would), the channel of its
 * task `late` still waits on (tgm_chan_order), and `late`. A client's is killed as a faulting client's is, and the waits
 * it held up go on. live_lock held. */
static enum tgm_hang hang_recover(struct tgm_chan *late, const char *who, u32 seqno, u64 waited_ns)
{
	struct tgm_hang_candidate c[6] = {};
	struct tgm_chan *chans[6] = {};
	struct tgm_chan *chan = NULL;
	struct tgm_chan *blame = NULL;
	char report[sizeof(late->fault)];
	char state[TGM_HANG_LINE];
	enum tgm_hang verdict = TGM_HANG_STUCK;
	u64 now = nvxnu_nanotime();
	int suspects[4];
	int n = 0;
	int i = 0;
	int b = 0;
	int progress = 0;

	if (gpu.stage < 3 || !iso.deadline)
		return TGM_HANG_STUCK;
	n = tgm_recover_suspects(gpu.device, suspects, ARRAY_SIZE(suspects));
	for (i = 0; i < n; i++)
		chans[i] = live_by(0, suspects[i]);
	if (late->waited.cpu && (s32)(READ_ONCE(*late->waited.cpu) - late->waited.seqno) < 0)
		chans[n] = live_by(late->waited.id, 0);
	chans[++n] = late;
	for (i = 0; i <= n; i++) {
		c[i].live = chans[i] != NULL;
		c[i].stuck = chans[i] && chan_stuck(chans[i], now);
		c[i].kernel = chans[i] && chans[i]->kernel;
	}
	list_for_each_entry(chan, &live_chans, live)
	{
		if (now - chan_progress_ns(chan) < TGM_STUCK_NS)
			progress = 1;
	}
	verdict = tgm_hang_blame(c, n + 1, READ_ONCE(late->error) != 0, late->kernel, progress, &b);
	if (b < 0 || chans[b]->isolating)
		return verdict;
	blame = chans[b];
	chan_describe(blame, state, sizeof(state));
	scnprintf(report, sizeof(report), HANG_REPORT "%s waited %llu ms for %s fence %u; %s", who,
		  waited_ns / 1000000, late->owner, seqno, state);
	nvxnu_log(
		"tegra-gm20b: GPU hang, recovering %s (channel %u): %s waited %llu ms for %s fence "
		"%u; %s\n",
		blame->owner, blame->chid, who, waited_ns / 1000000, late->owner, seqno, state);
	chan_isolate(blame, report, now);
	tgm_recover_fault(gpu.device, blame->chid);
	return verdict;
}

int tgm_ctxsw_stuck(int chid)
{
	struct tgm_chan *chan = NULL;
	char report[sizeof(chan->fault)];
	char state[TGM_HANG_LINE];
	u64 now = nvxnu_nanotime();
	int stuck = 1;

	mutex_lock(&live_lock);
	chan = live_by(0, chid);
	if (chan && !chan_stuck(chan, now)) {
		stuck = 0;
	} else if (chan && !chan->isolating) {
		chan_describe(chan, state, sizeof(state));
		if (chan->kernel) {
			nvxnu_panic("tegra-gm20b: context switch timed out on %s's channel: %s",
				    chan->owner, state);
		}
		scnprintf(report, sizeof(report), "CTXSW_TIMEOUT: %s", state);
		nvxnu_log("tegra-gm20b: context switch timed out, recovering %s (channel %u): %s\n",
			  chan->owner, chan->chid, state);
		chan_isolate(chan, report, now);
	}
	mutex_unlock(&live_lock);
	return stuck;
}

void tgm_chan_stalled(struct tgm_chan *chan, const char *who, uint32_t seqno, uint64_t waited_ns)
{
	enum tgm_hang verdict = TGM_HANG_STUCK;

	mutex_lock(&live_lock);
	verdict = hang_recover(chan, who, seqno, waited_ns);
	mutex_unlock(&live_lock);
	if (verdict == TGM_HANG_STUCK)
		tgm_chan_hung(chan, who, seqno, waited_ns);
}

/* stats:
 * GPU-side times come from 16-byte semaphore releases, which write PTIMER. On this Tegra PTIMER is not in nanoseconds:
 * its source is 19.2 MHz while its 32 ns step assumes 31.25 MHz (nvgpu gm20b_tegra_platform.ptimer_src_freq,
 * os/linux/platform_gk20a_tegra.c; PTIMER_REF_FREQ_HZ, include/nvgpu/ptimer.h), so it runs at 0.6144 x real time and nvgpu
 * scales every PTIMER timeout by it; nouveau's gk20a timer programs no rate (subdev/timer/gk20a.c has no init). The
 * user fence latency (CPU clock) measures 1.63-1.65 x the stamped queue + exec in every window. Each window's
 * stamps are converted to nanoseconds with PTIMER's rate measured over that window against the CPU clock, which the
 * line prints ("PTIMER xR"). */
#define STATS_WINDOW_NS (5 * NSEC_PER_SEC)

static struct {
	u64 since;      /* start of the current window, 0 before the first submission */
	u64 cal_ptimer; /* PTIMER at `since`: its rate over the window */
	u32 submits, fences, imports, timed;
	u32 idle;   /* presents that found no user work pending */
	u32 writer; /* presents that waited for their surface's importers only */
	u32 allocs; /* BoAllocs */
	u64 alloc_bytes, alloc_ns;
	u64 lat_sum, lat_max;                         /* user submissions only */
	u64 queue_sum, queue_max, exec_sum, exec_max; /* GPU-side, from the stamps (PTIMER units) */
	u32 copies,
		gaps; /* presents stamped: the copy's own GPU time; the GPU's gap from the end of the last user
				   * work the copy waited for to the copy's start (scheduling and semaphore acquire latency) */
	u64 copy_sum, copy_max, gap_sum, gap_max; /* PTIMER units */
	/* the compositor's submissions: the GPU queue (kick to start stamp) of all of them, and of those whose kick found an
	 * app on the GPU (tgm_sched_kicked: preempted, mid-switch or deferred), PTIMER units */
	u32 comp, behind;
	u64 comp_sum, comp_max, behind_sum, behind_max;
	/* gpu.lock held by a channel's setup (chan_new) and teardown (chan_free): every VmBind, BoAlloc, BoImport and BoFree
	 * of every process, the compositor's too, waits for it; nanoseconds, the sum counts them */
	u64 lock_new_sum, lock_new_max, lock_free_sum, lock_free_max;
	u32 lock_new, lock_free;
	s64 live_bytes; /* host imports */
	s32 live;
} stats;

static int stats_on(void)
{
	return TGM_STATS;
}

/* The atomics below write through sum and max. */
/* NOLINTNEXTLINE(readability-non-const-parameter) */
static void stats_add(u64 *sum, u64 *max, u64 v)
{
	u64 m = __atomic_load_n(max, __ATOMIC_RELAXED);
	__atomic_fetch_add(sum, v, __ATOMIC_RELAXED);
	while (v > m &&
	       !__atomic_compare_exchange_n(max, &m, v, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
		;
}

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
static u64 compositor_pool; /* the compositor's pool BOs (tgm_stats_compositor_pool) */

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
		__atomic_fetch_add(&stats.imports, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stats.live, bytes > 0 ? 1 : -1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stats.live_bytes, bytes, __ATOMIC_RELAXED);
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
		__atomic_fetch_add(&compositor_pool, (u64)bytes, __ATOMIC_RELAXED);
}

/* The frame timeline (SwitchFrameTimeline.h): since the display last took them, the compositor's submissions and every
 * other process's count, and every process's new-surface work (BoImport, VmBind, BoAlloc, BoFree, the imports' cache
 * cleans). Callers on several connections at once: each field on its own atomics. */
static struct {
	u64 first, last, exec_ns;
	u32 submits, others;
	u32 imports, binds, allocs, frees;
	u64 import_ns, bind_ns, alloc_ns, clean_ns, clean_bytes;
} frame_gpu;

void tgm_stats_bo_alloc(uint64_t bytes, int kind, uint64_t ns, const uint64_t phase_ns[TGM_PHASES],
			const char *owner)
{
	int i = 0;
	if (!stats_on())
		return;
	__atomic_fetch_add(&frame_gpu.allocs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&frame_gpu.alloc_ns, ns, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stats.allocs, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stats.alloc_bytes, bytes, __ATOMIC_RELAXED);
	__atomic_fetch_add(&stats.alloc_ns, ns, __ATOMIC_RELAXED);
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
	__atomic_fetch_add(&frame_gpu.imports, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&frame_gpu.import_ns, ns, __ATOMIC_RELAXED);
	__atomic_fetch_add(&frame_gpu.clean_ns, phase_ns[3], __ATOMIC_RELAXED);
	__atomic_fetch_add(&frame_gpu.clean_bytes, bytes, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_bytes, bytes, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_ro, !!read_only, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.import_skipped, !!skipped, __ATOMIC_RELAXED);
	stats_add(&mstat.import_ns, &mstat.import_max, ns);
	for (i = 0; i < TGM_PHASES; i++)
		__atomic_fetch_add(&mstat.import_phase[i], phase_ns[i], __ATOMIC_RELAXED);
}

#if TGM_STATS
/* The "imports" line's histograms: quarter-octave buckets of microseconds (each about 19% wide; below 4 us one per
 * microsecond), so a percentile reads as the upper bound of the bucket it falls in. */
#define COST_BUCKETS 100
#define SLOW_IMPORT_NS (4 * NSEC_PER_MSEC)
#define SLOW_IMPORT_LINES 8 /* a window's "slow import" lines at most */

extern uint64_t thread_get_runtime_self(void); /* DEVELOPMENT kernels export every global */
extern void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);

static struct {
	u32 total[COST_BUCKETS], wire[COST_BUCKETS], bind[COST_BUCKETS];
	u64 n, bytes, total_max, wire_ns, wire_max, wire_cpu_ns, zero_fills, pages_ns, mem_ns,
		lock_ns, lock_max, coh_ns, clean_ns, slow, slow_logged;
} icost;

static u32 cost_bucket(u64 ns)
{
	const u64 us = ns / 1000;
	u32 octave = 0;
	if (us < 4)
		return (u32)us;
	octave = 63 - __builtin_clzll(us);
	return min_t(u32, octave * 4 + ((u32)(us >> (octave - 2)) & 3), COST_BUCKETS - 1);
}

static u64 cost_bucket_top_us(u32 b)
{
	return b < 4 ? b : ((u64)(4 + (b & 3) + 1) << (b / 4 - 2)) - 1;
}

/* The upper bound (us) of the bucket holding the `per_mille`th part of the window's `n` samples, taken from `hist`. */
static u64 cost_percentile(const u32 *hist, u64 n, u32 per_mille)
{
	const u64 want = (n * per_mille + 999) / 1000;
	u64 seen = 0;
	u32 b = 0;
	for (b = 0; b < COST_BUCKETS; b++) {
		seen += hist[b];
		if (seen >= want && seen)
			return cost_bucket_top_us(b);
	}
	return 0;
}

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
	__atomic_fetch_add(&icost.total[cost_bucket(c->total_ns)], 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&icost.wire[cost_bucket(c->wire_ns)], 1, __ATOMIC_RELAXED);
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

/* The window's "imports" line (stats_mem): its histograms and sums taken, as mstat's are. */
static void stats_imports(u64 ms, u64 map_n)
{
	u32 total[COST_BUCKETS];
	u32 wire[COST_BUCKETS];
	u32 bind[COST_BUCKETS];
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
	for (b = 0; b < COST_BUCKETS; b++) {
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
		    n, ms, bytes >> 10, cost_percentile(total, n, 500),
		    cost_percentile(total, n, 990), total_max / 1000, cost_percentile(wire, n, 500),
		    cost_percentile(wire, n, 990), wire_max / 1000,
		    wire_ns ? wire_cpu * 100 / wire_ns : 0, zero_fills, n ? pages / n / 1000 : 0,
		    n ? mem / n / 1000 : 0, n ? lock / n / 1000 : 0, lock_max / 1000,
		    n ? coh / n / 1000 : 0, n ? clean / n / 1000 : 0, slow, map_n,
		    cost_percentile(bind, map_n, 500), cost_percentile(bind, map_n, 990));
}
#endif

void tgm_stats_bind(int map, uint64_t pages, uint64_t ns)
{
	if (!stats_on())
		return;
	__atomic_fetch_add(&frame_gpu.binds, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&frame_gpu.bind_ns, ns, __ATOMIC_RELAXED);
	if (map) {
#if TGM_STATS
		__atomic_fetch_add(&icost.bind[cost_bucket(ns)], 1, __ATOMIC_RELAXED);
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
	__atomic_fetch_add(&frame_gpu.frees, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.free_n, 1, __ATOMIC_RELAXED);
	stats_add(&mstat.free_ns, &mstat.free_max, ns);
}

void tgm_stats_wait(uint32_t waiter, int woken, int done)
{
	u64 lat = 0;

	if (!stats_on())
		return;
	__atomic_fetch_add(&wstat.slept, 1, __ATOMIC_RELAXED);
	if (!woken) {
		__atomic_fetch_add(&wstat.deadline, 1, __ATOMIC_RELAXED);
		return;
	}
	__atomic_fetch_add(&wstat.woken, 1, __ATOMIC_RELAXED);
	if (!done) {
		__atomic_fetch_add(&wstat.other, 1, __ATOMIC_RELAXED);
		return;
	}
	lat = nvxnu_nanotime() -
	      __atomic_load_n(&waiter_woken_ns[waiter % TGM_WAITERS], __ATOMIC_RELAXED);
	__atomic_fetch_add(&wstat.lat_sum, lat, __ATOMIC_RELAXED);
	if (lat > __atomic_load_n(&wstat.lat_max, __ATOMIC_RELAXED))
		__atomic_store_n(&wstat.lat_max, lat, __ATOMIC_RELAXED);
}

/* The "waits" line: GPU interrupt passes by line, channel wakes, and how the fence waits that slept woke. */
static void stats_waits(u64 ms)
{
	u64 stall = 0;
	u64 nonstall = 0;
	u64 ds = 0;
	u64 dn = 0;
	u64 sum = 0;
	u64 max = 0;
	u32 wakes = 0;
	u32 slept = 0;
	u32 woken = 0;
	u32 other = 0;
	u32 deadline = 0;
	u32 per = ms ? (u32)ms : 1;

	nvxnu_irq_passes(&stall, &nonstall);
	ds = stall - wstat.stall;
	dn = nonstall - wstat.nonstall;
	wstat.stall = stall;
	wstat.nonstall = nonstall;
	wakes = __atomic_exchange_n(&wstat.wakes, 0, __ATOMIC_RELAXED);
	slept = __atomic_exchange_n(&wstat.slept, 0, __ATOMIC_RELAXED);
	woken = __atomic_exchange_n(&wstat.woken, 0, __ATOMIC_RELAXED);
	other = __atomic_exchange_n(&wstat.other, 0, __ATOMIC_RELAXED);
	deadline = __atomic_exchange_n(&wstat.deadline, 0, __ATOMIC_RELAXED);
	sum = __atomic_exchange_n(&wstat.lat_sum, 0, __ATOMIC_RELAXED);
	max = __atomic_exchange_n(&wstat.lat_max, 0, __ATOMIC_RELAXED);
	nvxnu_stats("tegra-gm20b: waits %llu ms: interrupt passes %llu/s stall, %llu/s nonstall; "
		    "channel wakes "
		    "%llu/s; fence waits slept %llu/s: %u woken (%u for another channel's fence, "
		    "wake to waiter mean %llu us max %llu us), %u at their deadline",
		    ms, ds * 1000 / per, dn * 1000 / per, (u64)wakes * 1000 / per,
		    (u64)slept * 1000 / per, woken, other,
		    woken > other ? sum / (woken - other) / 1000 : 0, max / 1000, deadline);
}

void tgm_stats_prefault(uint64_t bytes)
{
	if (!stats_on())
		return;
	__atomic_fetch_add(&mstat.prefault_n, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&mstat.prefault_bytes, bytes, __ATOMIC_RELAXED);
}

/* The window's "mem" line: BoAllocs (cached: mean/max and each phase's total; pool; how many were 64 KiB or less),
 * BoFrees, prefaulted views, and the system's page faults over the window (every vm_fault, and those that zero-filled a
 * page: XNU's own counters; with the kernel's user-fault timing, their mean and longest); and its "mem2" line: host
 * imports (mean/max, phase totals, read-only ones, cleans the registry skipped, re-imports of a surface released < 2 s
 * before) and VmBind maps (pages, mean/max) and unmaps. Each only when something happened. */
static void stats_mem(u64 ms)
{
	u64 v[sizeof(mstat) / sizeof(u64)];
	struct nvxnu_faults f;
	static struct nvxnu_faults last;
	static char line
		[800]; /* one caller at a time: the submission that won the window (stats_tick) */
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

static volatile struct tgm_stamps *chan_stamps(struct tgm_chan *chan, u32 seqno)
{
	return (volatile struct tgm_stamps *)((u8 *)chan->ring + CHAN_STAMPS) +
	       seqno % CHAN_NSTAMPS;
}

static u64 chan_stamps_va(struct tgm_chan *chan, u32 seqno)
{
	return chan->ring_va + CHAN_STAMPS +
	       (u64)(seqno % CHAN_NSTAMPS) * sizeof(struct tgm_stamps);
}

/* Submit-to-fence latency (CPU clock), seen from the fence's nonstall interrupt (every channel's handler runs on each
 * one), and from the GPU's stamps (PTIMER): queue = the GPU started the submission - the kernel kicked it, exec = the
 * GPU finished it (L2 flush included) - started it. The present channel's stamps time its copy instead
 * (present_gap: and the gap before it). */
static u64 present_gap(u32 seqno, u64 start);

/* push profile (stats):
 * What each push of a submission costs the GPU, sampled: one submission in 16 of each connection (the user client's
 * choice, with each push's class from the method stream it read, struct tgm_prof) stamps the GPU's time after each of its
 * pushes, a 16-byte semaphore release after wait-for-idle (push_stamp, as the submission's own start and end stamps) into
 * its own slot of the channel's CHAN_PROF page, each kicked as an entry of its own after the push's. So each push's time
 * is the GPU's from the end of the push before (the start stamp for the first) until its own work has drained, and the
 * fence push (WFI, L2 flush) takes the rest up to the end stamp; a push not stamped (TGM_PC_SKIP: a method may continue
 * into the next push, or the method stream was not read) has its time counted with the next one stamped. The stamps' wait
 * for idle keeps pushes from overlapping in a profiled submission, so those run slightly longer than others: one in 16
 * per connection, no more than one in flight per channel, and none with more than TGM_PROF_MAX pushes. Each window, the
 * "gpuprof" line gives every process's time by class (stats_exec_owners). */
static void prof_collect(struct tgm_chan *chan, u32 seqno, u64 start, u64 end)
{
	volatile struct tgm_stamp *slot =
		(volatile struct tgm_stamp *)((u8 *)chan->ring + CHAN_PROF);
	u64 prev = start;
	u64 t = 0;
	u32 i = 0;
	u32 n = min_t(u32, READ_ONCE(chan->prof_count), TGM_PROF_MAX);
	u32 draws = 0;
	u32 sdraws = 0;

	for (i = 0; i < n; i++) {
		u8 c = chan->prof_cls[i];
		draws +=
			chan->prof_draws
				[i]; /* a push not stamped: its draws ran in the next stamped one's time */
		sdraws += chan->prof_sdraws[i];
		if (c == TGM_PC_SKIP)
			continue;
		if (c >= TGM_PC_COUNT || slot[i].payload != seqno)
			return; /* not this submission's stamp: drop the rest */
		t = slot[i].ns;
		if (t < prev)
			return;
		__atomic_fetch_add(&chan->prof_ns[c], t - prev, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_n[c], 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_dn[c], draws, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_sdn[c], sdraws, __ATOMIC_RELAXED);
		draws = sdraws = 0;
		prev = t;
	}
	if (end >= prev) {
		__atomic_fetch_add(&chan->prof_ns[TGM_PC_FENCE], end - prev, __ATOMIC_RELAXED);
		__atomic_fetch_add(&chan->prof_n[TGM_PC_FENCE], 1, __ATOMIC_RELAXED);
	}
	__atomic_fetch_add(&chan->prof_subs, 1, __ATOMIC_RELAXED);
}

static const char *prof_name(unsigned int c, char *buf, unsigned int len)
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

static void stats_fences(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan);
	u32 s = chan->stat_done;
	u32 i = 0;
	u64 now = nvxnu_nanotime();
	u64 start = 0;
	u64 end = 0;
	u64 gap = 0;
	volatile struct tgm_stamps *st = NULL;

	if ((s32)(done - s) > CHAN_NSTAMPS)
		s = done - CHAN_NSTAMPS;
	for (; (s32)(done - s) > 0; s++) {
		i = (s + 1) % CHAN_NSTAMPS;
		if (!chan->kernel) {
			__atomic_fetch_add(&stats.fences, 1, __ATOMIC_RELAXED);
			stats_add(&stats.lat_sum, &stats.lat_max,
				  now - READ_ONCE(chan->stat_sub_ns[i]));
		}
		st = chan_stamps(chan, s + 1);
		if (!chan->kernel && s + 1 == READ_ONCE(chan->prof_seqno)) {
			if (st->start.payload == s + 1 && st->end.payload == s + 1)
				prof_collect(chan, s + 1, st->start.ns, st->end.ns);
			WRITE_ONCE(chan->prof_seqno, 0);
		}
		if (st->start.payload != s + 1 || st->end.payload != s + 1)
			continue; /* overwritten by a later submission, or not stamped */
		start = st->start.ns;
		end = st->end.ns;
		if (chan->kernel) {
			__atomic_fetch_add(&stats.copies, 1, __ATOMIC_RELAXED);
			stats_add(&stats.copy_sum, &stats.copy_max, end - start);
			gap = present_gap(s + 1, start);
			if (gap != ~0ULL) {
				__atomic_fetch_add(&stats.gaps, 1, __ATOMIC_RELAXED);
				stats_add(&stats.gap_sum, &stats.gap_max, gap);
			}
			continue;
		}
		__atomic_fetch_add(&stats.timed, 1, __ATOMIC_RELAXED);
		stats_add(&stats.queue_sum, &stats.queue_max,
			  start - READ_ONCE(chan->stat_sub_ptimer[i]));
		if (chan->high) {
			__atomic_fetch_add(&stats.comp, 1, __ATOMIC_RELAXED);
			stats_add(&stats.comp_sum, &stats.comp_max,
				  start - READ_ONCE(chan->stat_sub_ptimer[i]));
			if (READ_ONCE(chan->stat_kick[i]) != TGM_KICK_CLEAR) {
				__atomic_fetch_add(&stats.behind, 1, __ATOMIC_RELAXED);
				stats_add(&stats.behind_sum, &stats.behind_max,
					  start - READ_ONCE(chan->stat_sub_ptimer[i]));
			}
		}
		stats_add(&stats.exec_sum, &stats.exec_max, end - start);
		__atomic_fetch_add(&chan->stat_exec_n, 1, __ATOMIC_RELAXED);
		stats_add(&chan->stat_exec, &chan->stat_exec_max, end - start);
	}
	chan->stat_done = done;
}

static void present_stats(u32 *presents, u64 *sum, u64 *max);
static void scrub_stats(char *buf, unsigned int len, u64 window_ms);

/* The stats line's GPU clock, every window: the rate the GPCPLL is programmed for (nvkm_clk_read: its M/N/PL registers),
 * whether the GPU runs from it (SEL_VCO bit 0, GPC2CLK_OUT's bypass divider 0 = 1:1), and the rate GPC 0's clock
 * counter measured (nvgpu gm20b_clk_get_gpcclk_clock_counter: NCGPCCLK cycles counted over 800 cycles of the PLL's
 * 38.4 MHz reference, hw_trim_gm20b.h trim_gpc_clk_cntr_ncgpcclk_cfg/cnt). A count takes 21 us; each window reads the
 * count the previous window started and starts the next, so nothing waits for it. A measured rate below the programmed
 * one would show a slowdown or gating the PLL's registers do not: with it the line prints THERM's idle slowdown factor
 * (CLK_SLOWDOWN_0 bits 21:16, 0 = off; gm20b_clk_init clears it) and GR's engine clock gating mode (THERM_GATE_CTRL(0)
 * bits 1:0: 0 run, 1 auto, 2 stop). Called by one submission at a time (the window's winner, stats_tick). */
#define TRIM_GPC0_NCGPCCLK_CFG 0x134124
#define TRIM_GPC0_NCGPCCLK_CNT 0x134128
#define NCGPCCLK_CFG_RESET 0x01000000u
#define NCGPCCLK_CFG_ENABLE 0x00100000u
#define NCGPCCLK_CFG_WRITE_EN 0x00010000u
#define NCGPCCLK_CYCLES 800u

static void clk_measure(char *buf, unsigned int len)
{
	static int started;
	struct nvkm_device *device = gpu.device;
	u32 cnt = nvkm_rd32(device, TRIM_GPC0_NCGPCCLK_CNT);
	u32 ref_khz = gk20a_clk(device->clk)->parent_rate / 1000;
	u32 vco = nvkm_rd32(device, 0x137100);
	u32 out = nvkm_rd32(device, 0x137250);
	u32 slow = nvkm_rd32(device, 0x020160);
	u32 gate = nvkm_rd32(device, 0x020200);
	int pll = nvkm_clk_read(device->clk, nv_clk_src_gpc);
	char meas[40] = "-";

	if (started && !badf(cnt)) {
		scnprintf(meas, sizeof(meas), "%llu kHz",
			  (unsigned long long)ref_khz * (cnt & 0xfffff) / NCGPCCLK_CYCLES);
	} else if (started) {
		scnprintf(meas, sizeof(meas), "unread (%08x)", cnt);
	}
	nvkm_wr32(device, TRIM_GPC0_NCGPCCLK_CFG, NCGPCCLK_CFG_RESET);
	nvkm_wr32(device, TRIM_GPC0_NCGPCCLK_CFG,
		  NCGPCCLK_CFG_ENABLE | NCGPCCLK_CFG_WRITE_EN | NCGPCCLK_CYCLES);
	started = 1;
	const char *divider = "";

	if (!(vco & 1)) {
		divider = " BYPASSED";
	} else if (out & 0x3f00) {
		divider = " DIVIDED";
	}
	scnprintf(buf, len, "GPU clock %d kHz%s, measured %s (slowdown %u, ELCG %u)", pll, divider,
		  meas, (slow >> 16) & 0x3f, gate & 3);
}

/* The window's "exec" line: each process's stamped GPU time per submission (start to end stamp, the L2 flush included:
 * what its work held the GPU for, other channels' time slices inside it included), summed over its channels and taken from
 * them; the window's first EXEC_OWNERS - 1 processes by name, the rest as "(others)". `rate` converts PTIMER units to ns
 * (16.16, stats_tick). Called from the submission that won the window, never with live_lock held. */
#define EXEC_OWNERS 5

static void stats_exec_owners(char *line, unsigned int len, u64 rate)
{
	/* static: one caller at a time (the window's winner, stats_tick) */
	static struct {
		char name[17];
		u64 sum, max;
		u32 n, subs;
		u64 prof_ns[TGM_PC_COUNT];
		u32 prof_n[TGM_PC_COUNT];
		u64 prof_dn[TGM_PC_COUNT], prof_sdn[TGM_PC_COUNT];
	} o[EXEC_OWNERS];
	struct tgm_chan *chan = NULL;
	unsigned int used = 0;
	unsigned int i = 0;
	unsigned int c = 0;
	unsigned int at = 0;
	char name[32];

	memset(o, 0, sizeof(o));
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		u32 n = __atomic_exchange_n(&chan->stat_exec_n, 0, __ATOMIC_RELAXED);
		u64 sum = __atomic_exchange_n(&chan->stat_exec, 0, __ATOMIC_RELAXED);
		u64 max = __atomic_exchange_n(&chan->stat_exec_max, 0, __ATOMIC_RELAXED);
		u32 subs = __atomic_exchange_n(&chan->prof_subs, 0, __ATOMIC_RELAXED);
		if ((!n && !subs) || chan->kernel)
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
	mutex_unlock(&live_lock);
	line[0] = 0;
	for (i = 0; i < EXEC_OWNERS; i++) {
		if (!o[i].n)
			continue;
		at += scnprintf(line + at, len - at, "%s%s %u x mean %llu us max %llu us",
				at ? "; " : "", o[i].name, o[i].n,
				((o[i].sum / o[i].n) * rate >> 16) / 1000,
				(o[i].max * rate >> 16) / 1000);
	}
	if (at)
		nvxnu_stats("tegra-gm20b: exec by process: %s", line);
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
				shown ? ";" : "", prof_name(best, name, sizeof(name)),
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
	}
}

/* The compositor's deadline (deadline.c, hold.c), and each app channel's preemption latency and the one-period turns it
 * got, those above the minimum or with a turn, by owner. */
static void stats_deadline(char *buf, unsigned int len)
{
	char deadline[240];
	char holds[240];
	struct tgm_chan *chan = NULL;
	unsigned int n = 0;
	unsigned int turns = 0;
	unsigned int shown = 0;
	u64 latency = 0;

	tgm_deadline_stats(deadline, sizeof(deadline));
	tgm_hold_stats(holds, sizeof(holds));
	n = scnprintf(buf, len, "%s; %s; latency by channel:", deadline, holds);
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
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
	mutex_unlock(&live_lock);
	if (!shown)
		scnprintf(buf + n, len - n, " all at the minimum");
}

/* Run by each submission: the first one after a window's end logs it. */
static void stats_tick(void)
{
	u64 now = nvxnu_nanotime();
	u64 since = __atomic_load_n(&stats.since, __ATOMIC_RELAXED);
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
	u32 allocs = 0;
	u32 mc_int = 0;
	u32 mc_err = 0;
	u32 mc_adr = 0;
	u32 copies = 0;
	u32 gaps = 0;
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
	u64 gs = 0;
	u64 gm = 0;
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
	char scrubbed[120];
	char sched[120];
	char recov[80];

	__atomic_fetch_add(&stats.submits, 1, __ATOMIC_RELAXED);
	if (!since) {
		if (__atomic_compare_exchange_n(&stats.since, &since, now, false, __ATOMIC_RELAXED,
						__ATOMIC_RELAXED))
			stats.cal_ptimer = nvkm_timer_read(gpu.device->timer);
		return;
	}
	if (now - since < STATS_WINDOW_NS ||
	    !__atomic_compare_exchange_n(&stats.since, &since, now, false, __ATOMIC_RELAXED,
					 __ATOMIC_RELAXED))
		return;
	ms = (now - since) / 1000000;
	/* nanoseconds per PTIMER unit over this window, 16.16 fixed point; the window's stamps were taken within it */
	pt = nvkm_timer_read(gpu.device->timer);
	cal = stats.cal_ptimer;
	stats.cal_ptimer = pt;
	rate = cal && pt > cal ? ((now - since) << 16) / (pt - cal) : 1 << 16;
#define PT_NS(v) (((v) * rate) >> 16)
	submits = __atomic_exchange_n(&stats.submits, 0, __ATOMIC_RELAXED);
	fences = __atomic_exchange_n(&stats.fences, 0, __ATOMIC_RELAXED);
	imports = __atomic_exchange_n(&stats.imports, 0, __ATOMIC_RELAXED);
	sum = __atomic_exchange_n(&stats.lat_sum, 0, __ATOMIC_RELAXED);
	max = __atomic_exchange_n(&stats.lat_max, 0, __ATOMIC_RELAXED);
	timed = __atomic_exchange_n(&stats.timed, 0, __ATOMIC_RELAXED);
	present_stats(&presents, &ps, &pm);
	idle = __atomic_exchange_n(&stats.idle, 0, __ATOMIC_RELAXED);
	writer = __atomic_exchange_n(&stats.writer, 0, __ATOMIC_RELAXED);
	allocs = __atomic_exchange_n(&stats.allocs, 0, __ATOMIC_RELAXED);
	abytes = __atomic_exchange_n(&stats.alloc_bytes, 0, __ATOMIC_RELAXED);
	ans = __atomic_exchange_n(&stats.alloc_ns, 0, __ATOMIC_RELAXED);
	qs = __atomic_exchange_n(&stats.queue_sum, 0, __ATOMIC_RELAXED);
	qm = __atomic_exchange_n(&stats.queue_max, 0, __ATOMIC_RELAXED);
	es = __atomic_exchange_n(&stats.exec_sum, 0, __ATOMIC_RELAXED);
	em = __atomic_exchange_n(&stats.exec_max, 0, __ATOMIC_RELAXED);
	copies = __atomic_exchange_n(&stats.copies, 0, __ATOMIC_RELAXED);
	cs = __atomic_exchange_n(&stats.copy_sum, 0, __ATOMIC_RELAXED);
	cm = __atomic_exchange_n(&stats.copy_max, 0, __ATOMIC_RELAXED);
	gaps = __atomic_exchange_n(&stats.gaps, 0, __ATOMIC_RELAXED);
	gs = __atomic_exchange_n(&stats.gap_sum, 0, __ATOMIC_RELAXED);
	gm = __atomic_exchange_n(&stats.gap_max, 0, __ATOMIC_RELAXED);
	comp = __atomic_exchange_n(&stats.comp, 0, __ATOMIC_RELAXED);
	cos = __atomic_exchange_n(&stats.comp_sum, 0, __ATOMIC_RELAXED);
	com = __atomic_exchange_n(&stats.comp_max, 0, __ATOMIC_RELAXED);
	behind = __atomic_exchange_n(&stats.behind, 0, __ATOMIC_RELAXED);
	bs = __atomic_exchange_n(&stats.behind_sum, 0, __ATOMIC_RELAXED);
	bm = __atomic_exchange_n(&stats.behind_max, 0, __ATOMIC_RELAXED);
	tgm_sched_stats(sched, sizeof(sched));
	tgm_recover_stats(recov, sizeof(recov));
	lnew = __atomic_exchange_n(&stats.lock_new, 0, __ATOMIC_RELAXED);
	lns = __atomic_exchange_n(&stats.lock_new_sum, 0, __ATOMIC_RELAXED);
	lnm = __atomic_exchange_n(&stats.lock_new_max, 0, __ATOMIC_RELAXED);
	lfree = __atomic_exchange_n(&stats.lock_free, 0, __ATOMIC_RELAXED);
	lfs = __atomic_exchange_n(&stats.lock_free_sum, 0, __ATOMIC_RELAXED);
	lfm = __atomic_exchange_n(&stats.lock_free_max, 0, __ATOMIC_RELAXED);
	tegra_gm20b_mc_errors(&mc_int, &mc_err, &mc_adr);
	/* the GPU's share of the window spent on stamped user submissions and copies */
	busy = PT_NS(es + cs) * 100 / (now - since);
	clk_measure(clk, sizeof(clk));
	scrub_stats(scrubbed, sizeof(scrubbed), ms);
	/* formatted here and logged as one string: os_log drops a message whose scalar arguments outgrow its 256-byte buffer
	 * (libkern/os/log.h OS_LOG_BUFFER_MAX_SIZE), which a string argument does not count against */
	scnprintf(line, sizeof(line),
		  "%llu ms: %u submits (%llu/s), fence latency mean %llu us max %llu us (%u), GPU "
		  "queue "
		  "mean %llu us max %llu us, exec mean %llu us max %llu us (%u stamped), busy "
		  "%llu%% (PTIMER x%llu.%03llu), "
		  "L2 %s, %u presents (%u writer-only), "
		  "offer to done mean %llu us max "
		  "%llu us, %s; compositor GPU queue mean %llu us max %llu us (%u), with an app on "
		  "the GPU at its kick mean %llu us max %llu us (%u); %s; %s",
		  ms, submits, submits * 1000ULL / (ms ? ms : 1), fences ? sum / fences / 1000 : 0,
		  max / 1000, fences, timed ? PT_NS(qs) / timed / 1000 : 0, PT_NS(qm) / 1000,
		  timed ? PT_NS(es) / timed / 1000 : 0, PT_NS(em) / 1000, timed, busy, rate >> 16,
		  ((rate & 0xffff) * 1000) >> 16, l2_on() ? "cached" : "volatile", presents, writer,
		  presents ? ps / presents / 1000 : 0, pm / 1000, clk,
		  comp ? PT_NS(cos) / comp / 1000 : 0, PT_NS(com) / 1000, comp,
		  behind ? PT_NS(bs) / behind / 1000 : 0, PT_NS(bm) / 1000, behind, sched, recov);
	nvxnu_stats("tegra-gm20b: stats %s", line);
	stats_deadline(line, sizeof(line));
	nvxnu_stats("tegra-gm20b: deadline %s", line);
	/* the pool by holder, so no share of it goes unnamed */
	uint64_t pool_clients = 0;
	uint64_t pool_orphan_bytes = 0;
	uint64_t pool_orphans = 0;
	uint64_t pool_system_bytes = 0;
	uint64_t pool_systems = 0;
	tgm_pool_breakdown(&pool_clients, &pool_orphan_bytes, &pool_orphans, &pool_system_bytes,
			   &pool_systems);
	scnprintf(
		line, sizeof(line),
		"copy mean %llu us max %llu us (%u), after user work mean %llu us max %llu us "
		"(%u); "
		"%u offers idle; %u imports "
		"(%llu/s), %lld KiB live in "
		"%d, %u BoAllocs (%llu KiB, %llu us), pool %lu/%llu MiB (compositor %llu, clients "
		"%llu, orphans %llu in "
		"%llu; coherent from system memory %llu MiB in %llu), %s, MC "
		"%08x "
		"err "
		"%08x at %08x; gpu.lock held by channel setup mean %llu us max %llu us (%u), "
		"teardown mean %llu us max %llu us (%u)",
		copies ? PT_NS(cs) / copies / 1000 : 0, PT_NS(cm) / 1000, copies,
		gaps ? PT_NS(gs) / gaps / 1000 : 0, PT_NS(gm) / 1000, gaps, idle, imports,
		imports * 1000ULL / (ms ? ms : 1),
		/* live_bytes is signed: an arithmetic shift keeps its sign. */
		/* NOLINTNEXTLINE(bugprone-signed-bitwise) */
		(long long)(__atomic_load_n(&stats.live_bytes, __ATOMIC_RELAXED) >> 10),
		__atomic_load_n(&stats.live, __ATOMIC_RELAXED), allocs, abytes >> 10, ans / 1000,
		nvxnu_pool_used_pages() >> (20u - PAGE_SHIFT), nvxnu_pool_size() >> 20,
		__atomic_load_n(&compositor_pool, __ATOMIC_RELAXED) >> 20, pool_clients >> 20,
		pool_orphan_bytes >> 20, pool_orphans, pool_system_bytes >> 20, pool_systems,
		scrubbed, mc_int, mc_err, mc_adr, lnew ? lns / lnew / 1000 : 0, lnm / 1000, lnew,
		lfree ? lfs / lfree / 1000 : 0, lfm / 1000, lfree);
	nvxnu_stats("tegra-gm20b: stats2 %s", line);
	stats_exec_owners(line, sizeof(line), rate);
	stats_mem(ms);
	stats_waits(ms);
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
	}
#undef PT_NS
}

/* idle clock: the GPCPLL's output divider, so the GPU downclocks only where that costs no performance.
 * The GPU runs at GPU_MHZ (768 MHz, Horizon's docked maximum) whenever there is any GPU work, and drops
 * to 76.8 MHz (L4T's lowest B01 row, Horizon's own boost-mode GPU clock) only once it has been truly idle:
 *   * down: no submission kicked, nothing in flight and no display swap for IDLE_DROP_NS (500 ms), checked on the
 *     CPU governor's tick (SwitchPerfGovernor.cpp calls tegra_gm20b_idle_tick from XNU's perfcontrol deadline, so
 *     the GPU adds no timer of its own; without the CPU governor the GPU simply never drops). A swap arriving means
 *     Core Animation is animating, so the clock never drops during an animation, even one that submits little.
 *   * up: synchronously, before the submission's work is kicked (tgm_chan_exec, tgm_present_copy, and chan_fence as
 *     a backstop), so no GPU work ever starts at the idle clock; also at once (a work item) on switch_perf_hint's
 *     touch, missed-frame and launch hints, so the clock is usually back before the frame's first submission.
 * The change is the output divider alone, VCO left running and locked (1536 MHz: M 1, N 40 from the 38.4 MHz
 * reference), PL 1 <-> 10. GM20B's GPCPLL changes PL without a glitch (nvgpu clk_gm20b.c: PLDIV_GLITCHLESS; nouveau
 * gm20b.c gm20b_pllg_program_mnp's pdiv_only path, whose sequence this is: VCO divider 1:2 around it, an interim PL
 * sharing a bit with both ends). No NDIV slide, no relock, no bypass, no VDD_GPU write (the rail stays at clk_raise's
 * 800 mV): a few register writes and two 2 us delays, timed on every raise and logged (raise mean/max).
 * Before first use, gov_start runs one drop and raise at stage 3 (no client yet) and logs both times. Any failure
 * (the divider not reading back, the PLL not locked, the clock not at the rate) logs one line and leaves the GPU at
 * 768 MHz for the boot. */
#define IDLE_DROP_NS (500ULL * 1000000ULL)
#define IDLE_LOG_NS (10ULL * 1000000000ULL)
#define GPCPLL_CFG_R 0x137000   /* 0 enable, 17 lock */
#define GPCPLL_COEFF_R 0x137004 /* M 7:0, N 15:8, PL 21:16 */
#define GPC2CLK_OUT_R 0x137250  /* VCO divider 13:8: 0 = 1:1, 2 = 1:2 */
#define SEL_VCO_R 0x137100      /* 0 = GPU on bypass, 1 = on GPCPLL */
#define PLL_REF_KHZ 38400

extern uint64_t
native_display_last_swap_ns(void); /* NativeLegacyDisplay.cpp: validateSwap's last call, 0: none */

static struct {
	int on;       /* the idle clock is in use (atomic) */
	int low;      /* the GPCPLL runs the idle divider (atomic; changed under clk) */
	int dropping; /* a drop is queued or running (atomic) */
	void *clk;    /* mutex: every divider change */
	void *drop_w, *raise_w;
	u32 run_pl, idle_pl, run_khz, idle_khz;
	u64 last_busy; /* atomic: the last kick or hint (ns) */
	u64 kicked,
		retired; /* atomic: submissions kicked / seen complete, every channel, since boot */
	/* the periodic line (under clk) */
	u64 log_start, low_since, low_ns, raise_sum, raise_max;
	u32 drops, raises, hinted;
} idle;

static u64 idle_in_flight(void)
{
	return __atomic_load_n(&idle.kicked, __ATOMIC_SEQ_CST) -
	       __atomic_load_n(&idle.retired, __ATOMIC_SEQ_CST);
}

/* GM20B's glitchless PL change (nouveau gm20b.c gm20b_pllg_program_mnp, pdiv_only): returns 0 when the GPCPLL is
 * locked, on the VCO, at PL `pl`. Under idle.clk. */
static int pl_switch(u32 pl)
{
	struct nvkm_device *device = gpu.device;
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

/* Under idle.clk: a divider change failed. One line, then 768 MHz for the boot (the divider, else nouveau's full
 * reprogram of the rate), the idle clock off. */
static void idle_fail(const char *what, u32 pl)
{
	struct nvkm_device *device = gpu.device;
	int back = 0;

	WRITE_ONCE(idle.on, 0);
	back = pl_switch(idle.run_pl);
	if (back)
		back = clk_prog_khz(device, idle.run_khz);
	WRITE_ONCE(idle.low, 0);
	nvxnu_log("tegra-gm20b: idle clock off: %s to PL %u did not take (GPCPLL_CFG %08x COEFF "
		  "%08x SEL_VCO %x "
		  "GPC2CLK_OUT %08x); %d kHz now (%d), for this boot\n",
		  what, pl, nvkm_rd32(device, GPCPLL_CFG_R), nvkm_rd32(device, GPCPLL_COEFF_R),
		  nvkm_rd32(device, SEL_VCO_R), nvkm_rd32(device, GPC2CLK_OUT_R),
		  nvkm_clk_read(device->clk, nv_clk_src_gpc), back);
}

/* Up to the run rate if the idle divider is in force; returns the ns it took (0: nothing to do). Thread context. */
static u64 idle_raise(void)
{
	u64 t0 = 0;
	u64 dt = 0;

	nvxnu_mutex_lock(idle.clk);
	if (READ_ONCE(idle.low)) {
		t0 = nvxnu_nanotime();
		if (pl_switch(idle.run_pl)) {
			idle_fail("raise", idle.run_pl);
		} else {
			dt = nvxnu_nanotime() - t0;
			WRITE_ONCE(idle.low, 0);
			idle.raises++;
			idle.raise_sum += dt;
			if (dt > idle.raise_max)
				idle.raise_max = dt;
			idle.low_ns += t0 - idle.low_since;
		}
	}
	nvxnu_mutex_unlock(idle.clk);
	return dt;
}

/* Before a submission's work is kicked (the submitter's thread): busy now, and the run rate first. The order (busy
 * stamp, then the drop flags) pairs with gov_drop's (flag, then the stamp): one of the two always sees the other. */
static void idle_wake(void)
{
	if (!READ_ONCE(idle.on))
		return;
	__atomic_store_n(&idle.last_busy, nvxnu_nanotime(), __ATOMIC_SEQ_CST);
	if (__atomic_load_n(&idle.low, __ATOMIC_SEQ_CST) ||
	    __atomic_load_n(&idle.dropping, __ATOMIC_SEQ_CST))
		(void)idle_raise();
}

static void idle_drop(void *arg)
{
	u64 now = 0;

	(void)arg;
	nvxnu_mutex_lock(idle.clk);
	now = nvxnu_nanotime();
	if (READ_ONCE(idle.on) && !READ_ONCE(idle.low) && !idle_in_flight() &&
	    now - __atomic_load_n(&idle.last_busy, __ATOMIC_SEQ_CST) >= IDLE_DROP_NS &&
	    now - native_display_last_swap_ns() >= IDLE_DROP_NS) {
		if (pl_switch(idle.idle_pl)) {
			idle_fail("drop", idle.idle_pl);
		} else {
			WRITE_ONCE(idle.low, 1);
			idle.drops++;
			idle.low_since = nvxnu_nanotime();
		}
	}
	__atomic_store_n(&idle.dropping, 0, __ATOMIC_SEQ_CST);
	nvxnu_mutex_unlock(idle.clk);
	if (READ_ONCE(idle.low))
		tgm_rail_idle();
}

int tgm_idle_now(void)
{
	return !READ_ONCE(idle.on) || READ_ONCE(idle.low);
}

static void idle_hint_raise(void *arg)
{
	(void)arg;
	if (idle_raise()) {
		nvxnu_mutex_lock(idle.clk);
		idle.hinted++;
		nvxnu_mutex_unlock(idle.clk);
	}
}

/* drivers/SwitchPower SwitchPerfGovernor.cpp, on the CPU governor's tick (XNU's perfcontrol deadline: interrupts
 * masked, scheduler context): a few loads, and a work item queued once the GPU has been idle long enough. */
void tegra_gm20b_idle_tick(void)
{
	u64 now = 0;
	int expected = 0;

	if (!READ_ONCE(idle.on) || READ_ONCE(idle.low) || READ_ONCE(idle.dropping) ||
	    idle_in_flight())
		return;
	now = nvxnu_nanotime();
	if (now - __atomic_load_n(&idle.last_busy, __ATOMIC_SEQ_CST) < IDLE_DROP_NS ||
	    now - native_display_last_swap_ns() < IDLE_DROP_NS)
		return;
	if (__atomic_compare_exchange_n(&idle.dropping, &expected, 1, false, __ATOMIC_SEQ_CST,
					__ATOMIC_SEQ_CST))
		nvxnu_work_schedule(idle.drop_w);
}

/* drivers/SwitchPower switch_perf_hint (touch, missed frame, launch): busy now, and back up at once if low. */
void tegra_gm20b_perf_hint(unsigned int hint)
{
	if (!READ_ONCE(idle.on) || hint < 1 || hint > 3)
		return;
	__atomic_store_n(&idle.last_busy, nvxnu_nanotime(), __ATOMIC_SEQ_CST);
	if (READ_ONCE(idle.low))
		nvxnu_work_schedule(idle.raise_w);
}

/* chan_fence, after the kick; the nonstall pass (gov_fenced) and channel teardown retire. Counted from boot. */
static void idle_kicked(void)
{
	__atomic_fetch_add(&idle.kicked, 1, __ATOMIC_SEQ_CST);
}

static void idle_fenced(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan);
	u32 n = done - chan->gov_done;

	if ((s32)n <= 0)
		return;
	chan->gov_done = done;
	__atomic_fetch_add(&idle.retired, n, __ATOMIC_SEQ_CST);
}

/* The periodic line (log only), from a kick at most every IDLE_LOG_NS. */
static void idle_stats(void)
{
	u64 now = 0;
	u64 span = 0;
	u64 low = 0;
	u64 sum = 0;
	u64 max = 0;
	u32 drops = 0;
	u32 raises = 0;
	u32 hinted = 0;

	if (!READ_ONCE(idle.on) || nvxnu_nanotime() - READ_ONCE(idle.log_start) < IDLE_LOG_NS ||
	    !nvxnu_mutex_trylock(idle.clk))
		return;
	now = nvxnu_nanotime();
	if (now - idle.log_start < IDLE_LOG_NS) {
		nvxnu_mutex_unlock(idle.clk);
		return;
	}
	span = now - idle.log_start;
	low = idle.low_ns + (idle.low ? now - idle.low_since : 0);
	if (idle.low)
		idle.low_since = now;
	sum = idle.raise_sum;
	max = idle.raise_max;
	drops = idle.drops;
	raises = idle.raises;
	hinted = idle.hinted;
	idle.low_ns = idle.raise_sum = idle.raise_max = 0;
	idle.drops = idle.raises = idle.hinted = 0;
	idle.log_start = now;
	nvxnu_mutex_unlock(idle.clk);
	nvxnu_stats("tegra-gm20b: idle clock %llu ms: %llu.%llu%% at %u kHz, %u drops, %u raises "
		    "(%u by a hint), "
		    "raise mean %llu ns max %llu ns; %llu in flight; now %d kHz",
		    span / 1000000, low * 100 / span, low * 1000 / span % 10, idle.idle_khz, drops,
		    raises, hinted, raises ? sum / raises : 0, max, idle_in_flight(),
		    nvkm_clk_read(gpu.device->clk, nv_clk_src_gpc));
}

/* GPC 0's clock counter, as clk_measure reads it (NCGPCCLK cycles over 800 cycles of the 38.4 MHz reference, about
 * 21 us): started, waited for, read. In kHz of what it counts (about twice gpcclk if it counts gpc2clk). 0 if it could
 * not be read. idle_start only, before any client. */
static u32 gpc_counted_khz(void)
{
	struct nvkm_device *device = gpu.device;
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

/* After stage 3 (no client yet), at the rate clk_raise left: the divider pair, then one drop and raise as a test, each
 * timed and each rate read back from GPC 0's clock counter. */
static void idle_start(void)
{
	struct nvkm_device *device = gpu.device;
	u32 coeff = nvkm_rd32(device, GPCPLL_COEFF_R);
	u32 m = coeff & 0xff;
	u32 n = (coeff >> 8) & 0xff;
	u32 pl = (coeff >> 16) & 0x3f;
	u32 vco = 0;
	u32 k = 0;
	u32 best = 0;
	u32 i = 0;
	int run = nvkm_clk_read(device->clk, nv_clk_src_gpc);
	u64 t0 = 0;
	u64 down = 0;
	u64 up = 0;
	u32 at_idle = 0;
	u32 at_run = 0;

	if (idle.clk || run <= BOOT_KHZ)
		return; /* clk_raise did not raise it: it already runs the lowest rate */
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
	idle.run_pl = pl;
	idle.run_khz = (u32)run;
	idle.idle_pl = vco / (2 * best);
	idle.idle_khz = best;
	idle.clk = nvxnu_mutex_alloc();
	idle.drop_w = nvxnu_work_alloc(idle_drop, NULL);
	idle.raise_w = nvxnu_work_alloc(idle_hint_raise, NULL);
	if (!idle.clk || !idle.drop_w || !idle.raise_w) {
		nvxnu_log("tegra-gm20b: idle clock not used: no memory; the GPU stays at %d kHz\n",
			  run);
		return;
	}
	nvxnu_mutex_lock(idle.clk);
	t0 = nvxnu_nanotime();
	if (pl_switch(idle.idle_pl) || nvkm_clk_read(device->clk, nv_clk_src_gpc) != (int)best) {
		idle_fail("test drop", idle.idle_pl);
		nvxnu_mutex_unlock(idle.clk);
		return;
	}
	down = nvxnu_nanotime() - t0;
	at_idle = gpc_counted_khz();
	t0 = nvxnu_nanotime();
	if (pl_switch(idle.run_pl) || nvkm_clk_read(device->clk, nv_clk_src_gpc) != run) {
		idle_fail("test raise", idle.run_pl);
		nvxnu_mutex_unlock(idle.clk);
		return;
	}
	up = nvxnu_nanotime() - t0;
	at_run = gpc_counted_khz();
	idle.log_start = nvxnu_nanotime();
	__atomic_store_n(&idle.last_busy, idle.log_start, __ATOMIC_SEQ_CST);
	WRITE_ONCE(idle.on, 1);
	nvxnu_mutex_unlock(idle.clk);
	nvxnu_log("tegra-gm20b: idle clock: %d kHz while busy, %u kHz after %llu ms with no "
		  "submission and no "
		  "swap (GPCPLL M %u N %u: PL %u <-> %u, VCO %u kHz locked throughout, VDD_GPU "
		  "unchanged); "
		  "test drop %llu ns (GPC counter %u kHz), raise %llu ns (GPC counter %u kHz)\n",
		  run, best, IDLE_DROP_NS / 1000000, m, n, pl, idle.idle_pl, vco, down, at_idle, up,
		  at_run);
}

/* The last high completion, by both clocks (interrupt pass only) */
static u64 high_end_ns, high_end_pt;

/* A high channel's fence moved from `from` to `done` (interrupt pass): the GPU time of what completed, for the
 * compositor's time per frame (deadline.c): from its start stamp to its end stamp (debug kernels) or now, or for the
 * present (no stamp outside debug kernels) from its kick, then biased by any app on the GPU at its kicks. And each submission's start stamp after an app channel
 * it was queued behind: the time that app took to leave the GPU (its preemption latency, hold.c). A submission with
 * acquires before its stamp started late for its own reasons. */
static void high_fenced(struct tgm_chan *chan, u32 from, u32 done)
{
	volatile struct tgm_stamps *st = chan_stamps(chan, from + 1);
	volatile struct tgm_stamps *last = chan_stamps(chan, done);
	u64 now = nvxnu_nanotime();
	u64 now_pt = nvkm_timer_read(gpu.device->timer);
	u64 sample = 0;
	u64 end_pt = 0;
	int fit = (s32)(done - from) <= CHAN_QUEUED;
	int biased = !fit;
	u32 s = 0;

	tgm_deadline_clock(now, now_pt);
	end_pt = stats_on() && last->end.payload == done ? last->end.ns : now_pt;
	if (!chan->kernel && fit && st->start.payload == from + 1)
		sample = tgm_deadline_pt_ns(tgm_gpu_span(end_pt, st->start.ns, high_end_pt));
	if (!sample) {
		for (s = from + 1; !biased && (s32)(done - s) >= 0; s++)
			biased = READ_ONCE(chan->queued[s % CHAN_QUEUED].app);
		sample =
			tgm_gpu_span(now, READ_ONCE(chan->queued[(from + 1) % CHAN_QUEUED].kick_ns),
				     high_end_ns);
	}
	high_end_ns = now;
	high_end_pt = end_pt;
	tgm_deadline_progress(sample, biased);
	if (chan->kernel || !fit)
		return;
	for (s = from + 1; (s32)(done - s) >= 0; s++) {
		int behind = READ_ONCE(chan->queued[s % CHAN_QUEUED].behind);

		st = chan_stamps(chan, s);
		if (behind >= 0 && !READ_ONCE(chan->queued[s % CHAN_QUEUED].waits) &&
		    st->start.payload == s) {
			tgm_hold_sampled(behind, READ_ONCE(chan->queued[s % CHAN_QUEUED].asked_pt),
					 st->start.ns);
		}
	}
}

/* nouveau_fence_wait_uevent_handler(): a fence's NON_STALL_INTERRUPT arrived; wake the fence waiters. */
static void present_fenced(struct tgm_chan *chan);
static void scrub_fenced(struct tgm_chan *chan);

/* The interrupt pass's own nvxnu_wake() at its end wakes the kernel's waits (channel teardown, sync_for_cpu, system
 * sleep's drain); a channel's fence waiters sleep on its word and wake only when its fence moved. */
static void chan_wake_waiters(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan);

	if (done == chan->woken_done)
		return;
	chan->woken_done = done;
	if (SWITCH_FRAME_TIMELINE)
		nvxnu_kdebug(SWITCH_KDBG_GPU_DONE, chan->chid, done, chan->critical);
	chan_wake_all_waiters(chan);
}

static int chan_nonstall(struct nvif_event *event, void *repv, u32 repc)
{
	struct tgm_chan *chan = container_of(event, struct tgm_chan, nonstall);
	u32 done = tgm_chan_completed(chan);
	u64 now = 0;
	(void)repv;
	(void)repc;
	if (done != chan->progress_done) {
		if (__atomic_load_n(&chan->high, __ATOMIC_ACQUIRE))
			high_fenced(chan, chan->progress_done, done);
		chan->progress_done = done;
		now = nvxnu_nanotime();
		WRITE_ONCE(chan->progress_held, tgm_hold_held_ns(chan->chid, now));
		WRITE_ONCE(chan->progress_ns, now);
	}
	idle_fenced(chan);
	if (chan->kernel == CHAN_SCRUB) {
		scrub_fenced(chan);
		/* nobody sleeps on its fence but system sleep's drain and the wake's test fence */
		if (tgm_gate_is_shut(&sc7_gate))
			nvxnu_wake();
		return NVIF_EVENT_KEEP;
	}
	if (stats_on())
		stats_fences(chan);
	if (chan->kernel) {
		present_fenced(chan);
	} else {
		chan_wake_waiters(chan);
	}
	if (READ_ONCE(chan->fenced))
		chan->fenced(chan->fenced_arg);
	return NVIF_EVENT_KEEP;
}

void tgm_chan_on_fence(struct tgm_chan *chan, void (*fenced)(void *arg), void *arg)
{
	chan->fenced_arg = arg;
	__atomic_store_n(&chan->fenced, fenced, __ATOMIC_RELEASE);
}

static void chan_free(struct tgm_chan *chan)
{
	int i = 0;
	if (chan->high || chan->tracked)
		tgm_sched_forget(gpu.device, chan->chid);
	nvif_event_dtor(&chan->nonstall);
	nvif_event_dtor(&chan->kill);
	for (i = chan->nobj - 1; i >= 0; i--)
		nvif_object_dtor(&chan->obj[i]);
	nvif_object_unmap(&chan->user);
	nvif_object_dtor(&chan->user);
	/* the channel is gone: unmap its ring (TLB invalidated) before the ring's pages go back to the pool */
	nvif_vmm_put(&chan->vm->vmm, &chan->ring_vma);
	if (chan->ring_mem) {
		struct tgm_mem *m = chan->ring_mem;
		nvif_mem_dtor(&m->mem);
		kfree(m);
	}
	if (chan->ring)
		dma_free_attrs(&gpu_pdev.dev, CHAN_BYTES, chan->ring, chan->ring_pa, 0);
	kfree(chan->ring_pas);
	kfree(chan);
}

/* Compositor priority: nvkm's runlist has no priorities ("TODO: prio, interleaving",
 * fifo/nv50.c nv50_runl_update) and gives every channel RAMFC's default timeslice, 128 << 3 (fifo/gk104.c
 * gk104_chan_ramfc_write, RAMFC 0xf8 = PFIFO 0x2310: enable 28, scale 15:12, timeout 7:0), as nvgpu's default
 * (gk20a/fifo_gk20a.c gk20a_fifo_default_timeslice_us). nvgpu gives high-priority work 4x its low timeslice
 * (os/linux/driver_common.c nvgpu_init_timeslice: 5200 vs 1300 us); critical channels get scale 5, 4x, written before
 * their first submission (the host reads RAMFC when it first loads the channel). */
#define RAMFC_TIMESLICE 0xf8
#define RAMFC_TIMESLICE_DEFAULT 0x10003080
#define RAMFC_TIMESLICE_HIGH 0x10005080

static int compositor_timeslice_on(void)
{
	return 1;
}

static void chan_timeslice(u32 chid, u32 value)
{
	struct nvkm_runl *runl = NULL;
	struct nvkm_chan *nchan = NULL;
	unsigned long flags = 0;

	nvkm_runl_foreach(runl, gpu.device->fifo)
	{
		nchan = nvkm_runl_chan_get_chid(runl, chid, &flags);
		if (nchan) {
			nvkm_kmap(nchan->inst);
			nvkm_wo32(nchan->inst, RAMFC_TIMESLICE, value);
			nvkm_done(nchan->inst);
			nvkm_chan_put(&nchan, flags);
			return;
		}
	}
}

/* gpu.lock held */
static void chan_prioritize(struct tgm_chan *chan)
{
	chan->high = 1;
	tgm_sched_level(gpu.device, chan->chid, TGM_LEVEL_HIGH);
}

void tgm_chan_set_level(struct tgm_chan *chan, unsigned int level)
{
	int high = level == TGM_LEVEL_HIGH;
	u64 now = nvxnu_nanotime();
	u32 i = 0;

	if (chan->kernel)
		return;
	if (high == chan->high) {
		tgm_sched_level(gpu.device, chan->chid, level);
		return;
	}
	if (high) {
		/* work kicked before the change found no preempt: none of it is timed as the compositor's */
		for (i = 0; i < CHAN_QUEUED; i++) {
			WRITE_ONCE(chan->queued[i].kick_ns, now);
			WRITE_ONCE(chan->queued[i].behind, -1);
			WRITE_ONCE(chan->queued[i].app, 1);
			WRITE_ONCE(chan->queued[i].waits, 1);
		}
		WRITE_ONCE(chan->tracked, 0);
		chan->critical = 1;
		/* chan_nonstall reads queued[] once it sees high */
		__atomic_store_n(&chan->high, 1, __ATOMIC_RELEASE);
		tgm_sched_level(gpu.device, chan->chid, TGM_LEVEL_HIGH);
	} else {
		WRITE_ONCE(chan->high, 0);
		chan->critical = 0;
		tgm_sched_level(gpu.device, chan->chid, level);
	}
	if (compositor_timeslice_on())
		chan->timeslice_due = high ? RAMFC_TIMESLICE_HIGH : RAMFC_TIMESLICE_DEFAULT;
}

/* chan_exec, before a submission is kicked: the host saves a channel's RAMFC when it leaves it, so a level change's
 * timeslice is written while the channel is idle and on neither GR nor the PBDMA. */
static void chan_timeslice_due(struct tgm_chan *chan)
{
	u32 value = chan->timeslice_due;

	if (!value || (s32)(tgm_chan_completed(chan) - chan->submitted) < 0 ||
	    tgm_sched_loaded(gpu.device, chan->chid))
		return;
	chan_timeslice(chan->chid, value);
	chan->timeslice_due = 0;
}

static int chan_push_wait(struct tgm_chan *chan, u32 push_nr);

static int chan_new(struct tgm_vm *vm, const void *vm_owner, volatile uint32_t *fence_cpu,
		    uint64_t fence_gpu_va, const char *owner, int critical, struct tgm_chan **pchan)
{
	static const u32 classes[5] = {MAXWELL_B, MAXWELL_COMPUTE_B, KEPLER_INLINE_TO_MEMORY_B,
				       FERMI_TWOD_A, MAXWELL_DMA_COPY_A};

	union {
		struct nvif_chan_v0 chan;
		u8 raw[sizeof(struct nvif_chan_v0) + 32];
	} args = {};

	union {
		struct nvif_event_v0 ev;
		u8 raw[sizeof(struct nvif_event_v0) + sizeof(struct nvif_chan_event_v0)];
	} kargs = {};
	struct nvif_chan_event_v0 *host = (struct nvif_chan_event_v0 *)kargs.ev.data;
	struct gf100_vmm_map_v0 margs = {.version = 0, .vol = 1};
	struct nvif_push *push = NULL;
	struct tgm_chan *chan = NULL;
	u64 runm = 0;
	u64 locked = 0;
	u32 i = 0;
	u32 npages = (u32)CHAN_BYTES >> PAGE_SHIFT;
	int ret = 0;

	if (gpu.stage < 2)
		return -ENODEV;
	/* BoFree's zombies wait for the claimant's channels only: a space is addressed by its claimant's alone */
	if (vm->owner != vm_owner)
		return -EINVAL;
	chan = kzalloc(sizeof(*chan), GFP_KERNEL);
	if (!chan)
		return -ENOMEM;
	chan->vm = vm;
	for (i = 0; i < CHAN_QUEUED; i++)
		chan->queued[i].behind = -1;
	tgm_tlb_chan_init(&chan->tlb);
	chan->fence_cpu = fence_cpu;
	chan->fence_gpu = fence_gpu_va;
	chan->critical = critical;
	strscpy(chan->owner, owner, sizeof(chan->owner));
	/* a fence word only moves forward: a channel made in a slot continues its predecessor's sequence, so a semaphore
	 * acquire on the old seqno (the GPU present) is never left waiting for a word that went back to 0 */
	chan->submitted = READ_ONCE(*fence_cpu);
	chan->stat_done = chan->submitted;
	chan->woken_done = chan->submitted;
	chan->gov_done = chan->submitted;
	chan->progress_done = chan->submitted;
	chan->ring = dma_alloc_attrs(&gpu_pdev.dev, CHAN_BYTES, &chan->ring_pa, GFP_KERNEL,
				     DMA_ATTR_WRITE_COMBINE);
	chan->ring_pas = kcalloc(npages, sizeof(u64), GFP_KERNEL);
	chan->ring_mem = kzalloc(sizeof(*chan->ring_mem), GFP_KERNEL);
	if (!chan->ring || !chan->ring_pas || !chan->ring_mem) {
		chan_free(chan);
		return -ENOMEM;
	}
	for (i = 0; i < npages; i++)
		chan->ring_pas[i] = chan->ring_pa + ((u64)i << PAGE_SHIFT);

	gpu_lock();
	locked = stats_on() ? nvxnu_nanotime() : 0;
	{
		struct nvif_mem_ram_v0 rargs = {.version = 0, .dma = (dma_addr_t *)chan->ring_pas};
		chan->ring_mem->bytes = CHAN_BYTES;
		ret = nvif_mem_ctor_type(&gpu.mmu, "chanRing", NVIF_CLASS_MEM_GF100, gpu.mem_type,
					 PAGE_SHIFT, CHAN_BYTES, &rargs, sizeof(rargs),
					 &chan->ring_mem->mem);
		if (ret) {
			kfree(chan->ring_mem);
			chan->ring_mem = NULL;
		}
	}
	if (!ret) {
		ret = nvif_vmm_get(&vm->vmm, ADDR, false, PAGE_SHIFT, 0, CHAN_BYTES,
				   &chan->ring_vma);
	}
	if (!ret) {
		ret = nvif_vmm_map(&vm->vmm, chan->ring_vma.addr, CHAN_BYTES, &margs, sizeof(margs),
				   &chan->ring_mem->mem, 0);
		chan->ring_va = chan->ring_vma.addr;
	}
	if (ret)
		goto fail;

	/* nouveau_channel_ctor(): GPFIFO channel on the GR runlist, in this VM, GPFIFO after the pushbuf */
	runm = nvif_fifo_runlist(&gpu.dev, NV_DEVICE_HOST_RUNLIST_ENGINES_GR);
	args.chan.version = 0;
	args.chan.namelen = 32;
	args.chan.runlist = runm ? __ffs64(runm) : 0;
	args.chan.runq = 0;
	args.chan.priv = 0;
	/* NOLINTNEXTLINE(bugprone-signed-bitwise): nouveau's BIT is int */
	args.chan.devm = BIT(0);
	args.chan.vmm = nvif_handle(&vm->vmm.object);
	args.chan.ctxdma = 0;
	args.chan.offset = chan->ring_va + CHAN_PUSH_BYTES;
	args.chan.length = CHAN_GPFIFO_BYTES;
	args.chan.huserd = 0;
	args.chan.ouserd = 0;
	snprintf((char *)args.chan.name, 32, "%s", chan->owner); /* nvkm names it in its reports */
	ret = nvif_object_ctor(&gpu.dev.object, "chan", 0, MAXWELL_CHANNEL_GPFIFO_A, &args,
			       sizeof(args), &chan->user);
	chan->chid = args.chan.chid;
	if (!ret && critical && tgm_all_clients() && compositor_timeslice_on())
		chan_timeslice(chan->chid, RAMFC_TIMESLICE_HIGH);
	if (!ret && critical && vm_owner)
		chan_prioritize(chan);
	if (!ret)
		ret = nvif_object_map(&chan->user, NULL, 0); /* USERD (BAR1) for GP_PUT */
	if (ret)
		goto fail;

	/* engine objects: these allocate the GR context (golden image on first use) and bind the CE */
	for (i = 0; i < 5; i++) {
		ret = nvif_object_ctor(&chan->user, "obj", 0x00010000 + i, classes[i], NULL, 0,
				       &chan->obj[chan->nobj]);
		if (ret) {
			pr_err("class %04x on channel: %d\n", classes[i], ret);
		} else {
			chan->nobj++;
		}
	}
	host->version = 0;
	host->type = NVIF_CHAN_EVENT_V0_KILLED;
	ret = nvif_event_ctor(&chan->user, "chanKilled", args.chan.chid, chan_killed, false,
			      &kargs.ev, sizeof(kargs), &chan->kill);
	if (!ret)
		nvif_event_allow(&chan->kill);
	/* nouveau_fence_context_new(): the fence's nonstall interrupt (uchan.c NVIF_CHAN_EVENT_V0_NON_STALL_INTR); its
	 * allow sets PFIFO_INTR_EN_0 bit 31 (gf100_fifo_nonstall_allow). Without it, fences are still seen by polling. */
	host->type = NVIF_CHAN_EVENT_V0_NON_STALL_INTR;
	if (!nvif_event_ctor(&chan->user, "fenceNonStallIntr", args.chan.chid, chan_nonstall, false,
			     &kargs.ev, sizeof(kargs), &chan->nonstall))
		nvif_event_allow(&chan->nonstall);
	ret = nvif_chan906f_ctor(&chan->chan, chan->user.map.ptr,
				 (u8 *)chan->ring + CHAN_PUSH_BYTES, CHAN_GPFIFO_BYTES, chan->ring,
				 chan->ring_va, CHAN_PUSH_BYTES,
				 (u8 *)chan->ring + CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES,
				 chan->ring_va + CHAN_PUSH_BYTES + CHAN_GPFIFO_BYTES);
	if (stats_on()) {
		__atomic_fetch_add(&stats.lock_new, 1, __ATOMIC_RELAXED);
		stats_add(&stats.lock_new_sum, &stats.lock_new_max, nvxnu_nanotime() - locked);
	}
	mutex_unlock(&gpu.lock);
	if (ret) {
		gpu_lock();
		goto fail;
	}

	/* bind subchannels 0..4 (NV906F SET_OBJECT = method 0, data = class) */
	push = &chan->chan.push;
	ret = chan_push_wait(chan, 10);
	if (ret) {
		gpu_lock();
		goto fail;
	}
	for (i = 0; i < 5; i++) {
		PUSH_DATA(push, (1u << 29) | (1u << 16) | ((u32)i << 13) | (0x0000u >> 2));
		PUSH_DATA(push, classes[i]);
	}
	PUSH_KICK(push);
	WRITE_ONCE(chan->progress_held, tgm_hold_held_ns(chan->chid, nvxnu_nanotime()));
	chan->app = vm_owner != NULL;
	mutex_lock(&live_lock);
	chan->id = ++live_ids;
	list_add_tail(&chan->live, &live_chans);
	mutex_unlock(&live_lock);
	*pchan = chan;
	return 0;

fail:
	mutex_unlock(&gpu.lock);
	chan_free(chan);
	return ret;
}

/* The gate: its subchannel binds are kicked after gpu.lock is dropped. */
int tgm_chan_new(struct tgm_vm *vm, const void *vm_owner, volatile uint32_t *fence_cpu,
		 uint64_t fence_gpu_va, const char *owner, int critical, struct tgm_chan **pchan)
{
	int ret = 0;

	gate_enter();
	ret = chan_new(vm, vm_owner, fence_cpu, fence_gpu_va, owner, critical, pchan);
	gate_leave();
	return ret;
}

static void order_forget(struct tgm_chan *chan);

static void chan_abandon(struct tgm_chan *chan)
{
	u64 now = nvxnu_nanotime();
	u64 end = now + 2 * TGM_ISOLATE_NS;

	if (chan->kernel)
		tgm_chan_hung(chan, "channel destroy", chan->submitted, NSEC_PER_SEC);
	mutex_lock(&live_lock);
	chan_isolate(chan, HANG_REPORT "destroyed with its work still running 1 s later", now);
	mutex_unlock(&live_lock);
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if (READ_ONCE(chan->error))
			return;
		if (!nvxnu_idle(seq, end) && !READ_ONCE(chan->error)) {
			tgm_chan_hung(chan, "channel destroy: kill", chan->submitted,
				      nvxnu_nanotime() - now);
		}
	}
}

void tgm_chan_del(struct tgm_chan **pchan)
{
	struct tgm_chan *chan = *pchan;
	u32 waiter = 0;
	u64 end = 0;
	u64 held = 0;
	if (!chan)
		return;
	waiter = tgm_chan_waiter(chan);
	if (chan->tracked)
		tgm_hold_track(gpu.device, chan->chid, 0);
	/* bounded wait for idle (CONTRACT.md: <= 1 s), then nvkm tears the channel down (preempt + runlist) */
	end = nvxnu_nanotime() + NSEC_PER_SEC;
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if (chan->error || (s32)(tgm_chan_completed(chan) - chan->submitted) >= 0 ||
		    nvxnu_nanotime() >= end)
			break;
		if (!nvxnu_idle(seq, end) && (s32)(tgm_chan_completed(chan) - chan->submitted) >= 0)
			nvxnu_wake_due(seq, "fence", chan->submitted);
	}
	/* tearing down a channel that still runs would free pages the GPU is using: its owner is gone, so it is killed
	 * as a hung client's is, and the teardown waits for that */
	if (gpu.stage >= 3 && !chan->error && (s32)(tgm_chan_completed(chan) - chan->submitted) < 0)
		chan_abandon(chan);
	/* the idle clock's count: what it finished, and (a dead channel) what it never will */
	idle_fenced(chan);
	__atomic_fetch_add(&idle.retired, chan->submitted - chan->gov_done, __ATOMIC_SEQ_CST);
	chan->gov_done = chan->submitted;
	mutex_lock(&live_lock);
	list_del(&chan->live);
	order_forget(chan);
	mutex_unlock(&live_lock);
	gpu_lock();
	held = stats_on() ? nvxnu_nanotime() : 0;
	chan_free(chan);
	if (stats_on()) {
		__atomic_fetch_add(&stats.lock_free, 1, __ATOMIC_RELAXED);
		stats_add(&stats.lock_free_sum, &stats.lock_free_max, nvxnu_nanotime() - held);
	}
	mutex_unlock(&gpu.lock);
	*pchan = NULL;
	nvxnu_word_wake(&waiter_words[waiter]);
}

/* fence pages:
 * Every connection's fence page (its channels' seqno words) is one page of a single pool range, so the GPU present's
 * address space maps all of them once and waits on any user channel with a semaphore acquire. Pages are handed out
 * round-robin and zeroed, so a page freed with an acquire still pending on it is not reused at once. */
#define TGM_FENCE_USER_PAGES \
	64 /* one per connection (TegraGM20BUserClient.cpp kMaxConnectionsAll) */
/* and one after them for the kernel's own client channels (tgm_fence_kernel), so every address space's fence view holds
 * their fences too */
#define TGM_FENCE_PAGES (TGM_FENCE_USER_PAGES + 1)
/* A fence page is a whole VM page at most 16 KiB (XNU's PAGE_MAX_SIZE): its client maps it read-only, and a mapping is
 * whole VM pages, so a smaller slot would show the client its neighbours'. The GPU maps the range in 4 KiB pages. */
#define TGM_FENCE_SLOT ((size_t)16 * 1024)
#define TGM_FENCE_BYTES (TGM_FENCE_PAGES * TGM_FENCE_SLOT)
#define TGM_FENCE_GPU_PAGES (TGM_FENCE_BYTES / PAGE_SIZE)

static struct {
	void *va;
	u64 pa;
	u64 used; /* bitmap of pages in use */
	u32 next;
} fences;

static u64 fences_pas
	[TGM_FENCE_GPU_PAGES]; /* the range's GPU pages, for the address spaces that map all of it */

/* live_lock held: the fence range, made once and kept */
static int fences_alloc(void)
{
	u32 i = 0;

	if (fences.va)
		return 0;
	fences.va = tgm_pool_alloc(TGM_FENCE_BYTES, 0, &fences.pa);
	if (!fences.va)
		return -ENOMEM;
	for (i = 0; i < TGM_FENCE_GPU_PAGES; i++)
		fences_pas[i] = fences.pa + i * PAGE_SIZE;
	return 0;
}

int tgm_fence_page_get(volatile uint32_t **va, uint64_t *pa)
{
	u32 i = 0;
	u32 n = 0;
	int ret = -ENOMEM;

	mutex_lock(&live_lock);
	for (n = 0; !fences_alloc() && n < TGM_FENCE_USER_PAGES; n++) {
		i = (fences.next + n) % TGM_FENCE_USER_PAGES;
		if (fences.used & BIT_ULL(i))
			continue;
		fences.used |= BIT_ULL(i);
		fences.next = i + 1;
		memset((u8 *)fences.va + i * TGM_FENCE_SLOT, 0, PAGE_SIZE);
		*va = (volatile uint32_t *)((u8 *)fences.va + i * TGM_FENCE_SLOT);
		*pa = fences.pa + i * TGM_FENCE_SLOT;
		ret = 0;
		break;
	}
	mutex_unlock(&live_lock);
	return ret;
}

void tgm_fence_page_put(const volatile uint32_t *va)
{
	uintptr_t off = (uintptr_t)va - (uintptr_t)fences.va;
	if (!va)
		return;
	mutex_lock(&live_lock);
	fences.used &= ~BIT_ULL(off / TGM_FENCE_SLOT);
	mutex_unlock(&live_lock);
}

int tgm_fence_kernel(unsigned int which, volatile uint32_t **va, uint64_t *page_pa,
		     uint32_t *offset)
{
	const size_t off =
		TGM_FENCE_USER_PAGES * TGM_FENCE_SLOT + which * sizeof(struct TegraGM20BAbiFence);
	int ret = 0;

	if (which >= TGM_FENCE_KERNEL_CHANNELS)
		return -EINVAL;
	mutex_lock(&live_lock);
	ret = fences_alloc();
	mutex_unlock(&live_lock);
	if (ret)
		return ret;
	*va = (volatile uint32_t *)((u8 *)fences.va + off);
	*page_pa = fences.pa + (off & ~(PAGE_SIZE - 1));
	*offset = (u32)(off & (PAGE_SIZE - 1));
	return 0;
}

/* order between the channels of a task:
 * Two drivers in one process (the render server's raw GM20B path and NVK) each have their own connection, address space and
 * channels, and Metal's order between their command buffers is the order the process submits them in. tgm_chan_order
 * (TegraGM20BABI.h method 14) keeps it on the GPU, with no CPU wait: it queues semaphore ACQUIRE_GEQs (acquire switch on, as
 * the present's) on the fence words of the task's other channels, which the waiting channel's next tgm_chan_exec kicks as an
 * entry of their own ahead of its pushes. Every address space maps the whole fence pool read-only for them (fences_map, from
 * tgm_vm_new), as the present's does. Each acquire names a seqno already submitted when it was queued (AfterTask: the other
 * channels' last; BeforeTask: one up to the channel's last), and a channel's entries run in order, so no two submissions can
 * wait for each other. A source channel's teardown waits for it to go idle and drops what was queued on it (order_forget),
 * so no acquire is left on a fence page handed out again; one already kicked has passed by then. A waiter stuck on one is a
 * hang like any other (tgm_chan_check_progress), and its line names the acquire (tgm_chan_hung). */
static DEFINE_MUTEX(fences_mem_lock);
static struct tgm_mem *fences_mem; /* the whole pool as GPU memory, made once and kept */

static int fences_map(struct tgm_vm *vm)
{
	int ret = 0;

	mutex_lock(&fences_mem_lock);
	if (!fences_mem) {
		/* the pool exists from the first connection on; make sure it does now */
		mutex_lock(&live_lock);
		ret = fences_alloc();
		mutex_unlock(&live_lock);
		if (!ret)
			ret = tgm_mem_new(vm, fences_pas, TGM_FENCE_GPU_PAGES, &fences_mem);
	}
	mutex_unlock(&fences_mem_lock);
	if (!ret)
		ret = vm_kmap(vm, fences_mem, TGM_FENCE_BYTES, 1, &vm->fences_va);
	return ret;
}

void tgm_chan_set_group(struct tgm_chan *chan, const void *group)
{
	mutex_lock(&live_lock);
	chan->group = group;
	mutex_unlock(&live_lock);
}

/* live_lock held: `waiter`'s next submission waits for `source` to complete `seqno` (one entry per source, the later seqno) */
static int order_add(struct tgm_chan *waiter, struct tgm_chan *source, u32 seqno)
{
	uintptr_t off = (uintptr_t)source->fence_cpu - (uintptr_t)fences.va;
	u32 i = 0;

	if (!waiter->vm->fences_va || !fences.va || off >= TGM_FENCE_BYTES)
		return -ENODEV;
	for (i = 0; i < waiter->norder; i++) {
		if (waiter->order[i].id != source->id)
			continue;
		if ((s32)(seqno - waiter->order[i].seqno) > 0)
			waiter->order[i].seqno = seqno;
		return 0;
	}
	if (waiter->norder == TGM_ORDER_MAX)
		return -ENOSPC;
	waiter->order[waiter->norder].va = waiter->vm->fences_va + off;
	waiter->order[waiter->norder].cpu = source->fence_cpu;
	waiter->order[waiter->norder].id = source->id;
	waiter->order[waiter->norder].seqno = seqno;
	waiter->order[waiter->norder].chid = source->chid;
	waiter->norder++;
	return 0;
}

int tgm_chan_order(struct tgm_chan *chan, uint32_t flags, uint32_t seqno, uint32_t *queued)
{
	struct tgm_chan *other = NULL;
	u32 n = 0;
	u32 last = 0;
	int ret = 0;

	*queued = 0;
	if (chan->kernel || !flags || (flags & ~(TGM_ORDER_AFTER | TGM_ORDER_BEFORE)))
		return -EINVAL;
	if (READ_ONCE(chan->error))
		return -EIO;
	if (flags & TGM_ORDER_BEFORE) {
		if (!seqno)
			seqno = chan->submitted;
		if ((s32)(seqno - chan->submitted) > 0)
			return -EINVAL; /* only submitted work: an acquire never waits for a later submission */
		if ((s32)(tgm_chan_completed(chan) - seqno) >= 0)
			flags &= ~TGM_ORDER_BEFORE; /* done already: nothing to wait for */
	}
	mutex_lock(&live_lock);
	list_for_each_entry(other, &live_chans, live)
	{
		if (other == chan || other->kernel || !chan->group || other->group != chan->group ||
		    READ_ONCE(other->error))
			continue;
		last = READ_ONCE(other->submitted);
		if ((flags & TGM_ORDER_AFTER) && (s32)(tgm_chan_completed(other) - last) < 0) {
			ret = order_add(chan, other, last);
			if (ret)
				break;
			n++;
		}
		if (flags & TGM_ORDER_BEFORE) {
			ret = order_add(other, chan, seqno);
			if (ret)
				break;
			n++;
		}
	}
	mutex_unlock(&live_lock);
	*queued = n;
	return ret;
}

/* live_lock held, `chan` leaving: what other channels had queued on its fence word goes (it is idle: nothing to wait for) */
static void order_forget(struct tgm_chan *chan)
{
	struct tgm_chan *other = NULL;
	u32 i = 0;

	list_for_each_entry(other, &live_chans, live)
	{
		for (i = 0; i < other->norder;) {
			if (other->order[i].id == chan->id) {
				other->order[i] = other->order[--other->norder];
			} else {
				i++;
			}
		}
	}
}

/* From tgm_chan_exec, before a submission's own entries: the acquires tgm_chan_order queued for it, as one entry. One whose
 * source passed its seqno already is left out. 1 if any was pushed. */
static int order_waits(struct tgm_chan *chan)
{
	struct nvif_push *push = &chan->chan.push;
	struct tgm_order wait[TGM_ORDER_MAX];
	u32 i = 0;
	u32 n = 0;
	u32 kicked = 0;

	mutex_lock(&live_lock);
	n = chan->norder;
	memcpy(wait, chan->order, n * sizeof(wait[0]));
	chan->norder = 0;
	mutex_unlock(&live_lock);
	for (i = 0; i < n; i++) {
		if ((s32)(READ_ONCE(*wait[i].cpu) - wait[i].seqno) >= 0)
			continue;
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_MTHD(push, NV906F, SEMAPHOREA,
			  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(wait[i].va)),
			  SEMAPHOREB, lower_32_bits(wait[i].va), SEMAPHOREC, wait[i].seqno,
			  SEMAPHORED,
			  NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
				  NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
		chan->waited = wait[i];
		kicked++;
		/* the compositor never waits behind an app channel the deadline holds */
		if (chan->high)
			tgm_hold_need(gpu.device, wait[i].chid);
	}
	if (kicked)
		PUSH_KICK(push);
	return kicked != 0;
}

/* CPU readers of GPU-written memory:
 * The GPU is not IO-coherent; its sysmem PTEs are volatile (no GPU L2 lines), so a submission's writes are in DRAM once
 * its fence has passed, but the CPU may still hold stale lines of those pages (e.g. from copying the same IOSurface
 * three frames ago). For a reader outside the GPU client, NativeLegacyDisplay's copy of a swapped IOSurface: wait until
 * every submission made so far on every live channel has completed (or its channel died), then clean+invalidate
 * [va, va + bytes) to the point of coherency. With no user channel it returns at once and touches no cache, so the CPU
 * renderer's path pays nothing. Returns 0, or -ETIMEDOUT after timeout_ns (the range is invalidated either way). */
#define TGM_SYNC_MAX 512 /* 64 connections x 8 channels */

static int sync_done(u64 id, u32 seqno)
{
	struct tgm_chan *chan = NULL;
	int done = 1; /* a channel that is gone was drained or stopped by tgm_chan_del */
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		if (chan->id == id) {
			done = chan->error || (s32)(tgm_chan_completed(chan) - seqno) >= 0;
			break;
		}
	}
	mutex_unlock(&live_lock);
	return done;
}

static int sync_wait(u64 id, u32 seqno, u64 end)
{
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if (sync_done(id, seqno))
			return 0;
		if (nvxnu_nanotime() >= end)
			return -ETIMEDOUT;
		if (!nvxnu_idle(seq, end) && sync_done(id, seqno))
			nvxnu_wake_due(seq, "fence", seqno);
	}
}

int tegra_gm20b_sync_for_cpu(const void *va, uint64_t bytes, uint64_t timeout_ns)
{
	static struct {
		u64 id;
		u32 seqno;
	} want[TGM_SYNC_MAX]; /* 8 KiB: not on the kernel stack; sync_lock */

	static DEFINE_MUTEX(sync_lock);
	struct tgm_chan *chan = NULL;
	enum tgm_hang verdict = TGM_HANG_STUCK;
	unsigned int n = 0;
	unsigned int i = 0;
	int ret = 0;
	u64 end = 0;

	mutex_lock(&sync_lock);
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		if (n < TGM_SYNC_MAX) {
			want[n].id = chan->id;
			want[n].seqno = READ_ONCE(chan->submitted);
			n++;
		}
	}
	mutex_unlock(&live_lock);
	if (!n) {
		mutex_unlock(&sync_lock);
		return 0;
	}
	end = nvxnu_nanotime() + timeout_ns;
	for (i = 0; i < n && !ret; i++) {
		ret = sync_wait(want[i].id, want[i].seqno, end);
		if (!ret || gpu.stage < 3)
			continue;
		/* the caller would copy a surface the GPU may still be writing: what the GPU is stuck on is killed and
		 * waited for; slow work only times the copy out */
		verdict = TGM_HANG_PROGRESSING;
		mutex_lock(&live_lock);
		chan = live_by(want[i].id, 0);
		if (chan)
			verdict = hang_recover(chan, "sync_for_cpu", want[i].seqno, timeout_ns);
		if (verdict == TGM_HANG_STUCK)
			tgm_chan_hung(chan, "sync_for_cpu", want[i].seqno, timeout_ns);
		mutex_unlock(&live_lock);
		if (verdict == TGM_HANG_RECOVERING) {
			ret = sync_wait(want[i].id, want[i].seqno,
					nvxnu_nanotime() + 2 * TGM_ISOLATE_NS);
		}
	}
	mutex_unlock(&sync_lock);
	nvxnu_dcache_clean_inval(va, bytes);
	return ret;
}

/* The IOSurface lock's wait for the submissions that may write a surface (TegraGM20BCoherency.cpp) */
#define TGM_WRITER_HANG_NS (1000 * NSEC_PER_MSEC)

uint32_t tgm_chan_uid(struct tgm_chan *chan)
{
	return (u32)chan->id;
}

/* live_lock held */
static struct tgm_chan *live_by_uid(u32 uid)
{
	struct tgm_chan *chan = NULL;

	list_for_each_entry(chan, &live_chans, live)
	{
		if ((u32)chan->id == uid)
			return chan;
	}
	return NULL;
}

int tgm_writer_done(uint32_t uid, uint32_t seqno)
{
	struct tgm_chan *chan = NULL;
	int done = 0;

	mutex_lock(&live_lock);
	chan = live_by_uid(uid);
	done = !chan || chan->error || (s32)(tgm_chan_completed(chan) - seqno) >= 0;
	mutex_unlock(&live_lock);
	return done;
}

/* No bound of its own: a writer that stops progressing is recovered (killed, its fence released) as
 * tegra_gm20b_sync_for_cpu's is, which ends the wait; a lost interrupt is nvxnu_wake_due's panic. */
void tgm_writer_wait(uint32_t uid, uint32_t seqno)
{
	struct tgm_chan *chan = NULL;
	enum tgm_hang verdict = TGM_HANG_STUCK;

	for (;;) {
		const u64 end = nvxnu_nanotime() + TGM_WRITER_HANG_NS;

		for (;;) {
			const u32 seq = nvxnu_wake_seq();

			if (tgm_writer_done(uid, seqno))
				return;
			if (nvxnu_nanotime() >= end)
				break;
			if (!nvxnu_idle(seq, end) && tgm_writer_done(uid, seqno))
				nvxnu_wake_due(seq, "fence", seqno);
		}
		if (gpu.stage < 3)
			continue;
		verdict = TGM_HANG_PROGRESSING;
		mutex_lock(&live_lock);
		chan = live_by_uid(uid);
		if (chan)
			verdict = hang_recover(chan, "IOSurface lock", seqno, TGM_WRITER_HANG_NS);
		if (verdict == TGM_HANG_STUCK)
			tgm_chan_hung(chan, "IOSurface lock", seqno, TGM_WRITER_HANG_NS);
		mutex_unlock(&live_lock);
	}
}

int tgm_writer_fence_va(struct tgm_vm *vm, uint32_t uid, uint64_t *va)
{
	struct tgm_chan *chan = NULL;
	uintptr_t off = 0;
	int ret = -ENOENT;

	mutex_lock(&live_lock);
	chan = live_by_uid(uid);
	off = chan ? (uintptr_t)chan->fence_cpu - (uintptr_t)fences.va : TGM_FENCE_BYTES;
	if (chan && !chan->error && vm->fences_va && fences.va && off < TGM_FENCE_BYTES) {
		*va = vm->fences_va + off;
		ret = 0;
	}
	mutex_unlock(&live_lock);
	return ret;
}

int tgm_writers_all_done(void)
{
	struct tgm_chan *chan = NULL;
	int done = 1;

	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		if (!chan->error &&
		    (s32)(tgm_chan_completed(chan) - READ_ONCE(chan->submitted)) < 0) {
			done = 0;
			break;
		}
	}
	mutex_unlock(&live_lock);
	return done;
}

void tgm_writers_wait_all(void)
{
	while (tegra_gm20b_sync_for_cpu(NULL, 0, TGM_WRITER_HANG_NS) == -ETIMEDOUT) {
	}
}

/* Implicit order between processes (TegraGM20BABI.h method 17, TegraGM20BFences.h): a submission's acquires on the writers of
 * the surfaces it touches, queued as tgm_chan_order's are. Each writer names a submission kicked before the call (Exec records
 * it once tgm_chan_exec returned), so no two submissions can wait for each other. */
int tgm_chan_follow(struct tgm_chan *chan, const uint64_t *writers, uint32_t n)
{
	struct tgm_fence_source state;
	struct tgm_chan *src = NULL;
	uint32_t i = 0;
	int ret = 0;

	mutex_lock(&live_lock);
	for (i = 0; i < n && !ret; i++) {
		src = live_by_uid(tgm_fence_uid(writers[i]));
		if (!src || src->kernel)
			continue;
		state.self = src == chan;
		state.killed = READ_ONCE(src->error) != 0;
		state.completed = tgm_chan_completed(src);
		if (tgm_fence_wanted(writers[i], &state))
			ret = order_add(chan, src, tgm_fence_seqno(writers[i]));
	}
	mutex_unlock(&live_lock);
	return ret;
}

int tgm_chan_follow_importers(struct tgm_chan *chan, uint64_t pa, uint32_t *pending)
{
	struct tgm_fence_source state;
	struct tgm_chan *other = NULL;
	u32 last = 0;
	int ret = 0;

	*pending = 0;
	mutex_lock(&live_lock);
	list_for_each_entry(other, &live_chans, live)
	{
		if (other->kernel || !vm_imports(other->vm, pa))
			continue;
		last = READ_ONCE(other->submitted);
		state.self = other == chan;
		state.killed = READ_ONCE(other->error) != 0;
		state.completed = tgm_chan_completed(other);
		if (!tgm_fence_wanted((u64)(u32)other->id << 32 | last, &state))
			continue;
		(*pending)++;
		ret = order_add(chan, other, last);
		if (ret)
			break;
	}
	mutex_unlock(&live_lock);
	return ret;
}

void tgm_cpu_clean_pages(const uint64_t *pas, uint32_t npages)
{
	nvxnu_dcache_clean_inval_pages(pas, npages);
}

/* Host methods of MAXWELL_CHANNEL_GPFIFO_A not in the trimmed cl906f.h (open-gpu-doc classes/host/clb06f.h) */
#define NVB06F_FB_FLUSH 0x0024
#define NVB06F_MEM_OP_C 0x0030
#define NVB06F_MEM_OP_D 0x0034
#define NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE (0x0eu << 27)
#define NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY (0x10u << 27)
#define NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE (0x09u << 27)
#define NVB06F_MEM_OP_C_TLB_INVALIDATE_PDB_ALL 0x1u /* GPC 1:1 0: the GPCs' TLBs too */
#define NVB06F_WFI 0x0078

/* One 16-byte semaphore release (writes payload and the GPU's PTIMER ns), after wait-for-idle */
static void push_stamp(struct nvif_push *push, u64 va, u32 payload)
{
	/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
	/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
	PUSH_MTHD(push, NV906F, SEMAPHOREA,
		  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(va)), SEMAPHOREB,
		  lower_32_bits(va), SEMAPHOREC, payload, SEMAPHORED,
		  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 16BYTE));
	/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
}

/* Push dwords chan_fence() may write, at most */
#define CHAN_FENCE_DWORDS 32

/* Whether nvif_chan_gpfifo_wait(gpfifo_nr, push_nr) would find its room now, counted as nvif counts it (the GET reports
 * on the semaphore page, its post size, a wrap of the push buffer that kicks one more entry). */
static int chan_room(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)
{
	struct nvif_chan *c = &chan->chan;
	struct nvif_push *push = &c->push;
	u32 post = c->func->gpfifo.post ? c->func->gpfifo.post_size : 0;
	s32 free = 0;

	if (gpfifo_nr)
		push_nr += post;
	if (push_nr)
		gpfifo_nr++;
	if (push->cur + push_nr > push->end) {
		u32 nr = push_nr + post;
		u32 cur = push->cur - (u32 *)push->mem.object.map.ptr;
		u32 get = c->func->push.read_get(c);

		if (get <= cur ? push->hw.max - cur < nr && (!get || get - 1 < nr)
			       : get - cur - 1 < nr)
			return 0;
		gpfifo_nr++;
	}
	if ((u32)c->gpfifo.free >= gpfifo_nr)
		return 1;
	free = (s32)c->func->gpfifo.read_get(c) - (s32)c->gpfifo.cur - 1;
	if (free < 0)
		free += c->gpfifo.max + 1;
	return (u32)free >= gpfifo_nr;
}

/* Room for gpfifo_nr entries and push_nr dwords, waited for by interrupt: every submission's GET report comes before its
 * fence's nonstall interrupt (chan_fence), whose pass ends in nvxnu_wake(), so nvif's own wait then finds the room at
 * once and never spins. -ETIMEDOUT after 1 s. */
static int chan_wait_room(struct tgm_chan *chan, u32 gpfifo_nr, u32 push_nr)
{
	u64 end = nvxnu_nanotime() + NSEC_PER_SEC;
	int ret = 0;

	for (;;) {
		u32 seq = nvxnu_wake_seq();

		if (chan_room(chan, gpfifo_nr, push_nr))
			break;
		if (!nvxnu_idle(seq, end) && !chan_room(chan, gpfifo_nr, push_nr)) {
			ret = -ETIMEDOUT;
			break;
		}
	}
	return ret ? ret : nvif_chan_gpfifo_wait(&chan->chan, gpfifo_nr, push_nr);
}

/* nvif's PUSH_WAIT for push_nr dwords, waited for as chan_wait_room does. */
static int chan_push_wait(struct tgm_chan *chan, u32 push_nr)
{
	struct nvif_push *push = &chan->chan.push;

	return push->cur + push_nr > push->end ? chan_wait_room(chan, 1, push_nr) : 0;
}

/* Fence push: semaphore release of the next 32-bit seqno after wait-for-idle, then nvif's GET report, then a nonstall
 * interrupt (nouveau nvc0_fence_emit32), then kick. The L2's dirty lines go to DRAM first, and the end of the submission
 * is stamped for the stats. The GET report comes before the interrupt so that a wait for room (chan_room) finds the room
 * this submission frees once its interrupt has woken it; nvif's own report at the kick lands after the interrupt. Space
 * was reserved by the caller (chan_wait_room / chan_push_wait): CHAN_FENCE_DWORDS. */
static u32 chan_fence(struct tgm_chan *chan)
{
	struct nvif_push *push = &chan->chan.push;
	u32 next = chan->submitted + 1;
	idle_wake(); /* backstop: tgm_chan_exec and tgm_present_copy raise it before their first kick */
	if (l2_on() || chan->kernel) {
		/* engines idle, their writes committed to the L2 (FB flush: nvgpu gk20a_mm_fb_flush), the L2's dirty lines
		 * written to DRAM, and those writes committed before the release below. Always on the present channel: its
		 * completion promises the display every byte in DRAM (TegraScanoutCopy.h). */
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_NVSQ(push, NV906F, NVB06F_WFI, 0);
		PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
		PUSH_NVSQ(push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
			  NVB06F_MEM_OP_D_OPERATION_L2_FLUSH_DIRTY);
		PUSH_NVSQ(push, NV906F, NVB06F_FB_FLUSH, 0);
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	}
	if (stats_on()) {
		push_stamp(push, chan_stamps_va(chan, next) + offsetof(struct tgm_stamps, end),
			   next);
	}
	/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
	/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
	PUSH_MTHD(push, NV906F, SEMAPHOREA,
		  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(chan->fence_gpu)),
		  SEMAPHOREB, lower_32_bits(chan->fence_gpu), SEMAPHOREC, next, SEMAPHORED,
		  NVDEF(NV906F, SEMAPHORED, OPERATION, RELEASE) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_WFI, EN) |
			  NVDEF(NV906F, SEMAPHORED, RELEASE_SIZE, 4BYTE));
	/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	nvif_chan_gpfifo_post(&chan->chan);
	/* NOLINTNEXTLINE(bugprone-signed-bitwise): nouveau's PUSH_MTHD computes in int */
	PUSH_MTHD(push, NV906F, NON_STALL_INTERRUPT, 0);
	__asm__ volatile(
		"dsb sy" ::
			: "memory"); /* client WC writes and our push reach DRAM before GP_PUT */
	chan->submit_ns = nvxnu_nanotime();
	if (stats_on())
		WRITE_ONCE(chan->stat_sub_ns[next % CHAN_NSTAMPS], chan->submit_ns);
	if ((s32)(tgm_chan_completed(chan) - chan->submitted) >= 0) {
		WRITE_ONCE(chan->progress_held, tgm_hold_held_ns(chan->chid, chan->submit_ns));
		WRITE_ONCE(chan->busy_ns, chan->submit_ns);
	}
	if (chan->high) {
		/* its fence may pass before tgm_sched_kicked returns: taken as kicked behind an app until it has */
		WRITE_ONCE(chan->queued[next % CHAN_QUEUED].kick_ns, chan->submit_ns);
		WRITE_ONCE(chan->queued[next % CHAN_QUEUED].behind, -1);
		WRITE_ONCE(chan->queued[next % CHAN_QUEUED].app, 1);
	}
	switch_emc_gpu_submit();
	PUSH_KICK(push);
	if (chan->high) {
		struct tgm_sched_queued queued;
		enum tgm_sched_kick kick = tgm_sched_kicked(gpu.device, chan->chid, &queued);

		WRITE_ONCE(chan->queued[next % CHAN_QUEUED].asked_pt, queued.asked_pt);
		WRITE_ONCE(chan->queued[next % CHAN_QUEUED].behind, queued.behind);
		WRITE_ONCE(chan->queued[next % CHAN_QUEUED].app, kick != TGM_KICK_CLEAR);
		tgm_deadline_kicked(!chan->kernel, kick != TGM_KICK_CLEAR);
		if (stats_on())
			WRITE_ONCE(chan->stat_kick[next % CHAN_NSTAMPS], kick);
	}
	chan->submitted = next;
	idle_kicked();
	if (stats_on())
		idle_stats();
	return next;
}

__attribute__((weak)) void switch_sched_render_submitted(void)
{
}

static void frame_submitted(struct tgm_chan *chan, u32 seqno, u64 start)
{
	u64 now = nvxnu_nanotime();
	u64 none = 0;

	nvxnu_kdebug(SWITCH_KDBG_SUBMIT, chan->chid, seqno, chan->critical);
	if (!chan->critical) {
		__atomic_fetch_add(&frame_gpu.others, 1, __ATOMIC_RELAXED);
		return;
	}
	switch_sched_render_submitted();
	__atomic_compare_exchange_n(&frame_gpu.first, &none, start, false, __ATOMIC_RELAXED,
				    __ATOMIC_RELAXED);
	__atomic_store_n(&frame_gpu.last, start, __ATOMIC_RELAXED);
	__atomic_fetch_add(&frame_gpu.submits, 1, __ATOMIC_RELAXED);
	__atomic_fetch_add(&frame_gpu.exec_ns, now - start, __ATOMIC_RELAXED);
}

void tegra_gm20b_frame_take(struct SwitchFrameGpu *out)
{
	out->firstSubmit = __atomic_exchange_n(&frame_gpu.first, 0, __ATOMIC_RELAXED);
	out->lastSubmit = __atomic_exchange_n(&frame_gpu.last, 0, __ATOMIC_RELAXED);
	out->submits = __atomic_exchange_n(&frame_gpu.submits, 0, __ATOMIC_RELAXED);
	out->others = __atomic_exchange_n(&frame_gpu.others, 0, __ATOMIC_RELAXED);
	out->execNs = __atomic_exchange_n(&frame_gpu.exec_ns, 0, __ATOMIC_RELAXED);
	out->imports = __atomic_exchange_n(&frame_gpu.imports, 0, __ATOMIC_RELAXED);
	out->binds = __atomic_exchange_n(&frame_gpu.binds, 0, __ATOMIC_RELAXED);
	out->allocs = __atomic_exchange_n(&frame_gpu.allocs, 0, __ATOMIC_RELAXED);
	out->frees = __atomic_exchange_n(&frame_gpu.frees, 0, __ATOMIC_RELAXED);
	out->importNs = __atomic_exchange_n(&frame_gpu.import_ns, 0, __ATOMIC_RELAXED);
	out->bindNs = __atomic_exchange_n(&frame_gpu.bind_ns, 0, __ATOMIC_RELAXED);
	out->allocNs = __atomic_exchange_n(&frame_gpu.alloc_ns, 0, __ATOMIC_RELAXED);
	out->cleanNs = __atomic_exchange_n(&frame_gpu.clean_ns, 0, __ATOMIC_RELAXED);
	out->cleanBytes = __atomic_exchange_n(&frame_gpu.clean_bytes, 0, __ATOMIC_RELAXED);
}

/* Present order: held by a user submission from its present wait to its kick, and by the present
 * from its snapshot of the user channels to its kick, so every user submission is either in a present's snapshot (the
 * copy waits for it) or kicked after that present (it waits for the copy). */

static int chan_exec(struct tgm_chan *chan, const struct tgm_push *pushes, uint32_t count,
		     uint32_t *seqno, const struct tgm_prof *prof)
{
	u64 start = SWITCH_FRAME_TIMELINE ? nvxnu_nanotime() : 0;
	u32 i = 0;
	u32 stamps = 0;
	u32 first = 0;
	int ret = 0;
	int tlb = 0;
	u32 waits = 0;

	if (chan->error)
		return -EIO;
	if (!count) {
		*seqno = chan->submitted;
		return 0;
	}
	idle_wake(); /* the run rate before any of this submission is kicked */
	chan_timeslice_due(chan);
	/* from its first submission: a channel made before its connection's GPU priority came (the compositor's, at boot)
	 * is never held for the compositor's own frame */
	if (chan->app && !chan->high && !chan->tracked) {
		WRITE_ONCE(chan->tracked, 1);
		tgm_hold_track(gpu.device, chan->chid, 1);
	}
	/* a push profile (see "push profile"): only with the stats' stamps, one in flight per channel; one whose completion
	 * the stats' interrupt pass no longer looks at (more than CHAN_NSTAMPS behind) is given up */
	{
		u32 ps = READ_ONCE(chan->prof_seqno);
		if (ps && (s32)(tgm_chan_completed(chan) - ps) > CHAN_NSTAMPS)
			WRITE_ONCE(chan->prof_seqno, 0);
	}
	if (prof &&
	    (!stats_on() || chan->kernel || count > TGM_PROF_MAX || READ_ONCE(chan->prof_seqno)))
		prof = NULL;
	for (i = 0; prof && i < count; i++)
		stamps += prof->cls[i] != TGM_PC_SKIP;
	/* nouveau_exec_job_run(): room for every user entry plus our fence push and, each kicked as its own entry before
	 * them, a present wait, the task order's acquires and (stats) a start stamp, one more nvif GET semaphore
	 * with each. A profile's stamps: an entry each, a stamp and a GET semaphore. */
	ret = chan_wait_room(chan, count + 5 + stamps,
			     CHAN_FENCE_DWORDS + 32 + 5 * TGM_ORDER_MAX + 16 * stamps);
	if (ret == -ETIMEDOUT && gpu.stage >= 3) {
		tgm_chan_stalled(chan, "exec: GPFIFO still full after 1 s", chan->submitted,
				 NSEC_PER_SEC);
	}
	if (ret)
		return READ_ONCE(chan->error) ? -EIO : ret == -ETIMEDOUT ? -EBUSY : ret;
	if (READ_ONCE(chan->norder)) {
		/* the task's other channels' work it follows (tgm_chan_order), first, so the start stamp below times the work
		 * itself */
		waits = (u32)order_waits(chan);
	}
	/* a high channel's start stamp, in every kernel: the end of the preemption latency of an app it was queued behind */
	if (stats_on() || chan->high) {
		u32 next = chan->submitted + 1;
		if (stats_on()) {
			WRITE_ONCE(chan->stat_sub_ptimer[next % CHAN_NSTAMPS],
				   nvkm_timer_read(gpu.device->timer));
		}
		if (chan->high)
			WRITE_ONCE(chan->queued[next % CHAN_QUEUED].waits, waits);
		push_stamp(&chan->chan.push,
			   chan_stamps_va(chan, next) + offsetof(struct tgm_stamps, start), next);
		PUSH_KICK(&chan->chan.push);
	}
	if (prof) {
		memcpy(chan->prof_cls, prof->cls, count);
		memcpy(chan->prof_draws, prof->draws, count * sizeof(prof->draws[0]));
		memcpy(chan->prof_sdraws, prof->sampleDraws, count * sizeof(prof->sampleDraws[0]));
		WRITE_ONCE(chan->prof_count, count);
		WRITE_ONCE(chan->prof_seqno,
			   chan->submitted + 1); /* before the fence that completes it */
	}
	/* The L2 first: page-table lines a GPU-ordered map wrote around (TegraGM20BTlb.h) go before the TLB refetches. */
	tlb = tgm_tlb_take(&chan->tlb, &chan->vm->tlb);
	if (l2_on() || tlb) {
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_NVSQ(&chan->chan.push, NV906F, NVB06F_MEM_OP_C, 0, NVB06F_MEM_OP_D,
			  NVB06F_MEM_OP_D_OPERATION_L2_SYSMEM_INVALIDATE);
		if (tlb) {
			PUSH_NVSQ(&chan->chan.push, NV906F, NVB06F_MEM_OP_C,
				  NVB06F_MEM_OP_C_TLB_INVALIDATE_PDB_ALL, NVB06F_MEM_OP_D,
				  NVB06F_MEM_OP_D_OPERATION_MMU_TLB_INVALIDATE);
		}
		PUSH_KICK(&chan->chan.push);
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	}
	first = chan->chan.gpfifo.cur;
	for (i = 0; i < count; i++) {
		nvif_chan_gpfifo_push(&chan->chan, pushes[i].va, pushes[i].dwords << 2,
				      pushes[i].no_prefetch || ((l2_on() || tlb) && !i));
		if (prof && prof->cls[i] != TGM_PC_SKIP) {
			push_stamp(&chan->chan.push,
				   chan->ring_va + CHAN_PROF + i * sizeof(struct tgm_stamp),
				   chan->submitted + 1);
			PUSH_KICK(&chan->chan.push);
		}
	}
	tgm_hang_log_submit(&chan->hang, chan->submitted + 1, first, chan->chan.gpfifo.cur);

	*seqno = chan_fence(chan);
	if (SWITCH_FRAME_TIMELINE && !chan->kernel)
		frame_submitted(chan, *seqno, start);
	if (stats_on())
		stats_tick();
	return 0;
}

int tgm_chan_exec(struct tgm_chan *chan, const struct tgm_push *pushes, uint32_t count,
		  uint32_t *seqno, const struct tgm_prof *prof)
{
	int ret = 0;

	gate_enter();
	ret = chan_exec(chan, pushes, count, seqno, prof);
	gate_leave();
	return ret;
}

/* GPU present (TegraDisplayFlip/TegraScanoutCopy.h):
 * The display's swap path hands a frame (an IOSurface, pitch-linear BGRA) to the GPU instead
 * of waiting for the GPU and swizzling it with the CPU (TegraGM20BUserClient.cpp tegra_gm20b_scanout_copy). One kernel
 * channel in its own address space, which maps the scanout surfaces, the swapped surfaces and every connection's fence
 * page (tgm_fence_page_get), does per frame:
 *  - a semaphore ACQUIRE_GEQ (acquire switch on: the channel yields while it waits) on the last submitted seqno of every
 *    user channel whose address space imports the surface (its first page, tgm_vm_import_note; every user channel if
 *    none does), so the copy runs after the compositor's rendering in GPU order, no
 *    CPU waits for it, and an app's GPU work does not hold up the frame;
 *  - one MAXWELL_DMA_COPY_A LAUNCH_DMA, pitch source (the surface's row bytes) to a BLOCKLINEAR destination: GOBs of
 *    64 bytes x 8 lines (GOB_HEIGHT_FERMI_8), blocks one GOB wide and 16 GOBs high, width 5120 bytes: the layout window A
 *    column-scans (NativeScanout.h native_legacy_block_copy);
 *  - the kernel fence (chan_fence: WFI, the L2 flush, semaphore release, NON_STALL_INTERRUPT).
 * The fence's nonstall interrupt completes it: the caller's callback (native_legacy_gpu_complete, which any context may
 * call) runs from the interrupt pass. A present that has not completed 1 s after it was queued panics, naming what it
 * waits for. Every mapping is volatile: the source was written by work whose fence flushed the L2 (or bypassed it), the
 * destination is read by the display controller only. Methods from open-gpu-doc classes/dma-copy/clb0b5.h.
 * No user submission waits for a present: the compositor never writes a surface a copy still reads. Core Animation
 * recycles a framebuffer only when a later swap completes, and the display offers the next swap only once the copy
 * before it completed (NativeLegacyDisplay offerGpuCopy). */
#define NVB0B5_LAUNCH_DMA 0x0300
#define NVB0B5_LAUNCH_DMA_NON_PIPELINED (2u << 0)
#define NVB0B5_LAUNCH_DMA_FLUSH_ENABLE (1u << 2)
#define NVB0B5_LAUNCH_DMA_SRC_PITCH (1u << 7)       /* SRC_MEMORY_LAYOUT 7:7, PITCH 1 */
#define NVB0B5_LAUNCH_DMA_DST_BLOCKLINEAR (0u << 8) /* DST_MEMORY_LAYOUT 8:8, BLOCKLINEAR 0 */
#define NVB0B5_LAUNCH_DMA_MULTI_LINE (1u << 9)
#define NVB0B5_OFFSET_IN_UPPER 0x0400
#define NVB0B5_OFFSET_IN_LOWER 0x0404
#define NVB0B5_OFFSET_OUT_UPPER 0x0408
#define NVB0B5_OFFSET_OUT_LOWER 0x040c
#define NVB0B5_PITCH_IN 0x0410
#define NVB0B5_PITCH_OUT 0x0414
#define NVB0B5_LINE_LENGTH_IN 0x0418
#define NVB0B5_LINE_COUNT 0x041c
#define NVB0B5_SET_DST_BLOCK_SIZE 0x070c
#define NVB0B5_BLOCK_SIZE_FERMI_8(log2h) \
	(((log2h) << 4) | (1u << 12)) /* WIDTH 1 GOB, HEIGHT 2^log2h, DEPTH 1, FERMI_8 */
#define NVB0B5_SET_DST_WIDTH 0x0710
#define NVB0B5_SET_DST_HEIGHT 0x0714
#define NVB0B5_SET_DST_DEPTH 0x0718
#define NVB0B5_SET_DST_LAYER 0x071c
#define NVB0B5_SET_DST_ORIGIN 0x0720
#define NVB0B5_SET_SRC_BLOCK_SIZE 0x0728
#define NVB0B5_SET_SRC_WIDTH 0x072c
#define NVB0B5_SET_SRC_HEIGHT 0x0730
#define NVB0B5_SET_SRC_DEPTH 0x0734
#define NVB0B5_SET_SRC_LAYER 0x0738
#define NVB0B5_SET_SRC_ORIGIN 0x073c
#define TGM_PRESENT_NS \
	(1000 *        \
	 NSEC_PER_MSEC) /* as the display's own limit: compositor jobs up to ~90 ms were seen */

static struct {
	struct mutex lock;
	struct tgm_vm *vm;
	struct tgm_chan *chan;
	struct tgm_mem *fence_mem, *fences_mem;
	void *fence; /* the channel's own fence page (pool) */
	u64 fence_pa, fence_gpu, fences_gpu;
	void *deadline_work;
	u32 pending; /* seqno of the present in flight, 0: none (atomic: the interrupt pass completes it) */
	u64 queued;
	void (*done)(void *);
	void *arg;
	u32 waits; /* the acquires of the present in flight, for the deadline's line */

	struct {
		u64 id;
		u32 seqno;
	} wait[TGM_SYNC_MAX];
	volatile struct tgm_stamp
		*wait_end[TGM_SYNC_MAX]; /* stats: their end stamps (pool memory, kept mapped) */
	u32 stamped;  /* stats: seqno of the present in flight, whose copy is stamped */
	u32 presents; /* stats */
	u64 lat_sum, lat_max;
	u64 recovered; /* when the present in flight last found a recovery under way, or work still progressing */
} present = {.lock = {NULL}};

/* From the present channel's nonstall interrupt (the interrupt pass, nvkm's event lock held): a present whose fence passed
 * is done, and its callback runs here (the display's only signals its work loop). */
static void present_fenced(struct tgm_chan *chan)
{
	u32 pending = __atomic_load_n(&present.pending, __ATOMIC_ACQUIRE);
	void (*done)(void *) = NULL;
	void *arg = NULL;
	u64 queued = 0;

	if (!pending || (s32)(tgm_chan_completed(chan) - pending) < 0)
		return;
	done = present.done;
	arg = present.arg;
	queued = present.queued;
	if (!__atomic_compare_exchange_n(&present.pending, &pending, 0, false, __ATOMIC_ACQ_REL,
					 __ATOMIC_RELAXED))
		return;
	nvxnu_work_cancel(present.deadline_work);
	if (stats_on()) {
		__atomic_fetch_add(&present.presents, 1, __ATOMIC_RELAXED);
		stats_add(&present.lat_sum, &present.lat_max, nvxnu_nanotime() - present.queued);
	}
	tgm_deadline_presented(queued);
	done(arg);
}

/* TGM_PRESENT_NS after a present was queued, if it has not completed: name the first user channel it still waits for. */
static void present_deadline(void *arg)
{
	struct tgm_chan *chan = NULL;
	struct tgm_chan *waited = NULL;
	char state[TGM_HANG_LINE];
	char what[TGM_HANG_LINE];
	u64 age = 0;
	u32 i = 0;
	u32 seqno = 0;

	(void)arg;
	mutex_lock(&present.lock);
	if (!__atomic_load_n(&present.pending, __ATOMIC_ACQUIRE)) {
		mutex_unlock(&present.lock);
		return;
	}
	/* a deadline of an earlier present that was already running when that one completed: this one has its own time,
	 * which a recovery under way restarts */
	age = nvxnu_nanotime() - max(present.queued, present.recovered);
	if (age < TGM_PRESENT_NS) {
		nvxnu_work_schedule_after(present.deadline_work, TGM_PRESENT_NS - age);
		mutex_unlock(&present.lock);
		return;
	}
	age = nvxnu_nanotime() - present.queued;
	if ((s32)(tgm_chan_completed(present.chan) - present.pending) >= 0) {
		nvxnu_panic("tegra-gm20b: present fence %u complete after %llu ms but its nonstall "
			    "interrupt never came",
			    present.pending, age / 1000000);
	}
	mutex_lock(&live_lock);
	for (i = 0; i < present.waits && !waited; i++) {
		list_for_each_entry(chan, &live_chans, live)
		{
			if (chan->id == present.wait[i].id &&
			    (s32)(tgm_chan_completed(chan) - present.wait[i].seqno) < 0) {
				waited = chan;
				seqno = present.wait[i].seqno;
				break;
			}
		}
	}
	if (hang_recover(waited ? waited : present.chan, "present",
			 waited ? seqno : present.pending, age) != TGM_HANG_STUCK) {
		mutex_unlock(&live_lock);
		present.recovered = nvxnu_nanotime();
		nvxnu_work_schedule_after(present.deadline_work, TGM_PRESENT_NS);
		mutex_unlock(&present.lock);
		return;
	}
	if (waited) {
		chan_describe(waited, what, sizeof(what));
	} else {
		scnprintf(what, sizeof(what), "none of its %u user fences (the copy itself)",
			  present.waits);
	}
	chan_describe(present.chan, state, sizeof(state));
	nvxnu_panic("tegra-gm20b: present not done %llu ms after it was queued, waiting for "
		    "%s%s%u: present %s",
		    (nvxnu_nanotime() - present.queued) / 1000000, what, waited ? " to reach " : "",
		    waited ? seqno : 0, state);
}

/* stats, from stats_fences (interrupt pass) before present_fenced completes the present: PTIMER from the end of
 * the last user submission present `seqno` waited for to the copy's start (its start stamp, after the acquires), ~0 when
 * it waited for none or a stamp is missing. The wait list is that present's: the next is queued only after
 * present_fenced. A user channel's stamps are in its ring, pool memory that stays mapped even after the channel is
 * gone; a reused page fails the payload check. */
static u64 present_gap(u32 seqno, u64 start)
{
	u64 last = 0;
	u32 i = 0;

	if (present.stamped != seqno || !present.waits)
		return ~0ULL;
	for (i = 0; i < present.waits; i++) {
		volatile struct tgm_stamp *end = present.wait_end[i];
		if (!end || end->payload != present.wait[i].seqno)
			return ~0ULL;
		if (end->ns > last)
			last = end->ns;
	}
	return start >= last ? start - last : 0;
}

static void present_stats(u32 *presents, u64 *sum, u64 *max)
{
	*presents = __atomic_exchange_n(&present.presents, 0, __ATOMIC_RELAXED);
	*sum = __atomic_exchange_n(&present.lat_sum, 0, __ATOMIC_RELAXED);
	*max = __atomic_exchange_n(&present.lat_max, 0, __ATOMIC_RELAXED);
}

int tgm_present_open(struct tgm_vm **pvm)
{
	volatile u32 *probe = NULL;
	u64 probe_pa = 0;
	int ret = 0;

	if (gpu.stage < 3)
		return -ENODEV;
	mutex_lock(&present.lock);
	if (present.chan)
		goto out;
	/* the fence pages exist from the first connection on; make sure they do now */
	ret = tgm_fence_page_get(&probe, &probe_pa);
	if (ret)
		goto out;
	tgm_fence_page_put(probe);
	present.deadline_work = nvxnu_work_alloc(present_deadline, NULL);
	ret = present.deadline_work ? tgm_vm_new(&present.vm) : -ENOMEM;
	if (!ret) {
		ret = pfence_get(
			present.vm); /* the channel's own fence: the page every address space may wait on */
	}
	if (!ret) {
		present.fence = pfence.va;
		present.fence_pa = pfence.pa;
		present.fence_mem = pfence.mem;
		ret = tgm_vm_kmap(present.vm, present.fence_mem, PAGE_SIZE, &present.fence_gpu);
	}
	if (!ret)
		ret = tgm_mem_new(present.vm, fences_pas, TGM_FENCE_GPU_PAGES, &present.fences_mem);
	if (!ret) {
		ret = tgm_vm_kmap(present.vm, present.fences_mem, TGM_FENCE_BYTES,
				  &present.fences_gpu);
	}
	if (!ret) {
		ret = tgm_chan_new(present.vm, NULL, present.fence, present.fence_gpu, "present", 1,
				   &present.chan);
	}
	if (!ret) {
		present.chan->kernel = CHAN_PRESENT;
		gpu_lock();
		chan_prioritize(present.chan);
		mutex_unlock(&gpu.lock);
	} else {
		/* stays unusable: every offer is refused and the display copies with the CPU */
		nvxnu_log("tegra-gm20b: GPU present unavailable: %d\n", ret);
	}
out:
	*pvm = present.chan ? present.vm : NULL;
	mutex_unlock(&present.lock);
	if (present.chan)
		return 0;
	return ret ? ret : -ENODEV;
}

/* Present mappings: volatile, at a fixed address of the present's user range. */
int tgm_present_map(uint64_t va, struct tgm_mem *mem, uint64_t bytes, uint8_t kind)
{
	return present.vm ? vm_map(present.vm, va, bytes, mem, 0, kind, 0, 1, 0, NULL) : -ENODEV;
}

int tgm_present_unmap(uint64_t va, uint64_t bytes)
{
	return present.vm ? tgm_vm_unmap(present.vm, va, bytes) : -ENODEV;
}

/* A present waits only for the address spaces that import its surface; a surface no address space imports by its first
 * page is the all-channel wait. */
static int present_writer_on(void)
{
	return 1;
}

static int present_copy(uint64_t src_va, uint64_t src_pa, uint64_t stride, int src_block,
			uint32_t width, uint32_t height, uint64_t dst_va, uint32_t dst_pitch,
			uint32_t block_height_log2, void (*done)(void *), void *arg)
{
	struct tgm_chan *chan = present.chan;
	struct tgm_chan *user = NULL;
	struct nvif_push *push = NULL;
	u64 va = 0;
	u32 n = 0;
	u32 room = 0;
	int ret = 0;
	int matched = 0;

	if (!chan || chan->error)
		return -ENODEV;
	idle_wake(); /* the run rate before the copy is kicked */
	/* dst_va 0: no copy, only the acquires and the fence (direct scanout: the display scans the surface itself once the
	 * compositor's rendering of it is done) */
	if (dst_va && (block_height_log2 > 5 || dst_pitch < width * 4))
		return -EINVAL;
	mutex_lock(&present.lock);
	if (__atomic_load_n(&present.pending, __ATOMIC_ACQUIRE)) {
		mutex_unlock(&present.lock);
		return -EBUSY;
	}
	/* push room for an acquire per user channel there is now (one made meanwhile has nothing for this frame) */
	mutex_lock(&live_lock);
	list_for_each_entry(user, &live_chans, live) room += room < TGM_SYNC_MAX && !user->kernel;
	mutex_unlock(&live_lock);
	push = &chan->chan.push;
	ret = chan_push_wait(
		chan, room * 5 + 47 +
			      CHAN_FENCE_DWORDS); /* acquires, a start stamp, the copy, the fence */
	if (ret == -ETIMEDOUT) {
		tgm_chan_stalled(chan, "present: push buffer still full after 1 s", chan->submitted,
				 NSEC_PER_SEC);
	}
	if (ret) {
		mutex_unlock(&present.lock);
		return ret;
	}
	/* what the surface's writers (or, with none found, every user channel) have submitted so far and not completed yet */
	mutex_lock(&live_lock);
	list_for_each_entry(user, &live_chans, live)
	{
		if (present_writer_on() && !user->kernel && vm_imports(user->vm, src_pa))
			matched = 1;
	}
	list_for_each_entry(user, &live_chans, live)
	{
		uintptr_t off = (uintptr_t)user->fence_cpu - (uintptr_t)fences.va;
		if (user->kernel || READ_ONCE(user->error) || off >= TGM_FENCE_BYTES || n == room ||
		    (matched && !vm_imports(user->vm, src_pa)) ||
		    (s32)(tgm_chan_completed(user) - user->submitted) >= 0)
			continue;
		present.wait[n].id = user->id;
		present.wait[n].seqno = READ_ONCE(user->submitted);
		tgm_hold_need(gpu.device, user->chid);
		present.wait_end[n] =
			stats_on() ? &chan_stamps(user, present.wait[n].seqno)->end : NULL;
		va = present.fences_gpu + off;
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_MTHD(push, NV906F, SEMAPHOREA,
			  NVVAL(NV906F, SEMAPHOREA, OFFSET_UPPER, upper_32_bits(va)), SEMAPHOREB,
			  lower_32_bits(va), SEMAPHOREC, present.wait[n].seqno, SEMAPHORED,
			  NVDEF(NV906F, SEMAPHORED, OPERATION, ACQ_GEQ) |
				  NVDEF(NV906F, SEMAPHORED, ACQUIRE_SWITCH, ENABLED));
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
		n++;
	}
	mutex_unlock(&live_lock);
	present.waits = n;
	if (stats_on()) { /* the copy's start, after the acquires: stats_fences times the copy and the gap before it */
		present.stamped = chan->submitted + 1;
		push_stamp(push,
			   chan_stamps_va(chan, present.stamped) +
				   offsetof(struct tgm_stamps, start),
			   present.stamped);
	}
	if (dst_va) {
		/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
		/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
		PUSH_NVSQ(push, NVA0B5, NVB0B5_OFFSET_IN_UPPER, upper_32_bits(src_va),
			  NVB0B5_OFFSET_IN_LOWER, lower_32_bits(src_va), NVB0B5_OFFSET_OUT_UPPER,
			  upper_32_bits(dst_va), NVB0B5_OFFSET_OUT_LOWER, lower_32_bits(dst_va),
			  NVB0B5_PITCH_IN, (u32)stride, NVB0B5_PITCH_OUT, 0, NVB0B5_LINE_LENGTH_IN,
			  width * 4, NVB0B5_LINE_COUNT, height);
		PUSH_NVSQ(push, NVA0B5, NVB0B5_SET_DST_BLOCK_SIZE,
			  NVB0B5_BLOCK_SIZE_FERMI_8(block_height_log2), NVB0B5_SET_DST_WIDTH,
			  dst_pitch, NVB0B5_SET_DST_HEIGHT, height, NVB0B5_SET_DST_DEPTH, 1,
			  NVB0B5_SET_DST_LAYER, 0, NVB0B5_SET_DST_ORIGIN, 0);
		if (src_block) {
			PUSH_NVSQ(push, NVA0B5, NVB0B5_SET_SRC_BLOCK_SIZE,
				  NVB0B5_BLOCK_SIZE_FERMI_8(TEGRA_SCANOUT_TILED_BLOCK_HEIGHT_LOG2),
				  NVB0B5_SET_SRC_WIDTH, width * 4, NVB0B5_SET_SRC_HEIGHT, height,
				  NVB0B5_SET_SRC_DEPTH, 1, NVB0B5_SET_SRC_LAYER, 0,
				  NVB0B5_SET_SRC_ORIGIN, 0);
		}
		PUSH_NVSQ(push, NVA0B5, NVB0B5_LAUNCH_DMA,
			  NVB0B5_LAUNCH_DMA_NON_PIPELINED | NVB0B5_LAUNCH_DMA_FLUSH_ENABLE |
				  (src_block ? 0 : NVB0B5_LAUNCH_DMA_SRC_PITCH) |
				  NVB0B5_LAUNCH_DMA_DST_BLOCKLINEAR | NVB0B5_LAUNCH_DMA_MULTI_LINE);
		/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	}
	/* set before the kick: the nonstall interrupt can come before chan_fence returns */
	present.done = done;
	present.arg = arg;
	present.queued = nvxnu_nanotime();
	__atomic_store_n(&present.pending, chan->submitted + 1, __ATOMIC_RELEASE);
	nvxnu_work_schedule_after(
		present.deadline_work,
		TGM_PRESENT_NS); /* before the kick, so the completion cancels it */
	chan_fence(chan);
	if (matched && stats_on())
		__atomic_fetch_add(&stats.writer, 1, __ATOMIC_RELAXED);
	if (!n && stats_on())
		__atomic_fetch_add(&stats.idle, 1, __ATOMIC_RELAXED);
	mutex_unlock(&present.lock);
	return 0;
}

/* Refused while the gate is shut: the display copies that frame on the CPU. */
int tgm_present_copy(uint64_t src_va, uint64_t src_pa, uint64_t stride, int src_block,
		     uint32_t width, uint32_t height, uint64_t dst_va, uint32_t dst_pitch,
		     uint32_t block_height_log2, void (*done)(void *), void *arg)
{
	int ret = 0;

	if (!gate_try())
		return -EBUSY;
	ret = present_copy(src_va, src_pa, stride, src_block, width, height, dst_va, dst_pitch,
			   block_height_log2, done, arg);
	gate_leave();
	return ret;
}

/* pool scrub: the copy engine zeroes pool pages:
 * The CPU would spend 0.3 s of every boot zeroing the pool at M1 (256 MiB through the write-combined aperture), and a
 * thread call's memset 4.5 ms per 16 MiB of freed pool BO. So its pages start dirty (shim nvxnu_pool_init) and the
 * GPU zeroes them: a kernel channel (CHAN_SCRUB) in an address space of its own, which maps
 * the whole pool once (volatile, at scrub.pool_va), runs MAXWELL_DMA_COPY_A fills of remapped constant 0 (as Mesa's
 * nvk_cmd_fill_memory_ce and M3), each range followed by the kernel fence (chan_fence: WFI, FB flush, L2 flush of
 * dirty lines, then the semaphore release and the nonstall interrupt), so the zeros are in DRAM before the range goes
 * back clean (nvxnu_pool_free_zeroed) from that interrupt's pass. Nothing waits for it on the CPU: until then a range is
 * the scrubber's (allocated), and an allocation takes other pages, zeroing on the CPU only those a free left dirty.
 *  - Boot: once M3 passed, the scrubber's work item claims the free dirty pages (shim nvxnu_pool_claim_dirty) a run of at
 *    most SCRUB_CHUNK at a time, with at most SCRUB_INFLIGHT bytes queued, and claims more as ranges complete, so it
 *    never holds more than that of the pool from an allocation.
 *  - A freed pool BO (TegraGM20BUserClient.cpp poolZeroLater): tgm_pool_scrub queues its range.
 * The CPU has no cacheable view of the pool, so the zeros need no cache maintenance. A range not back SCRUB_NS after it
 * was queued panics with one line, as a present does: its nonstall interrupt never came, or the copy never ran. */
#define SCRUB_JOBS 64
#define SCRUB_CHUNK (16UL << 20)
#define SCRUB_INFLIGHT (48UL << 20)
#define SCRUB_NS (1000 * NSEC_PER_MSEC)
#define SCRUB_LINE \
	(128U << 10) /* bytes per line of a fill: 32768 four-byte elements, Mesa's max_dim */

static struct {
	struct mutex lock; /* submissions: the ring's head, the channel's push buffer */
	struct tgm_vm *vm;
	struct tgm_chan *chan;
	struct tgm_mem *pool_mem, *fence_mem;
	u64 *pool_pas;
	u64 pool_va, fence_gpu, fence_pa;
	volatile u32 *fence;
	void *work, *deadline;

	struct {
		u64 pa, bytes, queued;
		u32 seqno;
	} job[SCRUB_JOBS];

	u32 head,
		tail; /* job[tail % SCRUB_JOBS .. head) queued; head moves under lock, tail in scrub_fenced only */
	u64 inflight;  /* bytes queued (atomic) */
	int boot_done; /* the boot scrub found nothing left to claim */
	u64 boot_start, boot_bytes;
	/* stats: this window's ranges zeroed, their bytes and queue-to-done time (ns) */
	u64 n, bytes, ns, max_ns;
} scrub = {.lock = {NULL}};

/* One range: copy-engine fills, then the fence. Under scrub.lock, with a free job slot. 0 or a push error. */
static int scrub_submit(u64 pa, u64 bytes)
{
	struct tgm_chan *chan = scrub.chan;
	struct nvif_push *push = &chan->chan.push;
	u64 va = scrub.pool_va + (pa - nvxnu_pool_base_pa());
	u64 lines = bytes / SCRUB_LINE;
	u64 rest = bytes % SCRUB_LINE;
	u32 slot = 0;
	u32 seqno = 0;
	int ret = chan_wait_room(chan, 4, 32 + CHAN_FENCE_DWORDS);

	if (ret)
		return ret;
	/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
	/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
	PUSH_MTHD(push, NVA0B5, SET_REMAP_CONST_A, 0);
	PUSH_MTHD(push, NVA0B5, SET_REMAP_COMPONENTS,
		  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, DST_X, CONST_A) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, COMPONENT_SIZE, FOUR) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, NUM_SRC_COMPONENTS, ONE) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, NUM_DST_COMPONENTS, ONE));
	PUSH_MTHD(push, NVA0B5, PITCH_IN, SCRUB_LINE, PITCH_OUT, SCRUB_LINE);
	if (lines) { /* whole 128 KiB lines, one multi-line fill */
		PUSH_MTHD(push, NVA0B5, OFFSET_OUT_UPPER, upper_32_bits(va), OFFSET_OUT_LOWER,
			  lower_32_bits(va));
		PUSH_MTHD(push, NVA0B5, LINE_LENGTH_IN, SCRUB_LINE / 4, LINE_COUNT, (u32)lines);
		PUSH_MTHD(push, NVA0B5, LAUNCH_DMA,
			  NVDEF(NVA0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
				  NVDEF(NVA0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
				  NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
				  NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
				  NVDEF(NVA0B5, LAUNCH_DMA, MULTI_LINE_ENABLE, TRUE) |
				  NVDEF(NVA0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE));
	}
	if (rest) { /* the last pages, one line (a multiple of 4 KiB) */
		va += lines * SCRUB_LINE;
		PUSH_MTHD(push, NVA0B5, OFFSET_OUT_UPPER, upper_32_bits(va), OFFSET_OUT_LOWER,
			  lower_32_bits(va));
		PUSH_MTHD(push, NVA0B5, LINE_LENGTH_IN, (u32)(rest / 4), LINE_COUNT, 1);
		PUSH_MTHD(push, NVA0B5, LAUNCH_DMA,
			  NVDEF(NVA0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
				  NVDEF(NVA0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
				  NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
				  NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
				  NVDEF(NVA0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE));
	}
	/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	slot = scrub.head % SCRUB_JOBS;
	scrub.job[slot].pa = pa;
	scrub.job[slot].bytes = bytes;
	scrub.job[slot].queued = nvxnu_nanotime();
	scrub.job[slot].seqno = seqno = chan->submitted + 1;
	__atomic_fetch_add(&scrub.inflight, bytes, __ATOMIC_RELAXED);
	/* the job is visible to scrub_fenced before its fence can complete */
	__atomic_store_n(&scrub.head, scrub.head + 1, __ATOMIC_RELEASE);
	if (chan_fence(chan) != seqno) {
		nvxnu_panic("tegra-gm20b: pool scrub fence %u is not the job's %u", chan->submitted,
			    seqno);
	}
	nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS);
	return 0;
}

/* Room for one more range: a free job slot (the boot's claims also keep to SCRUB_INFLIGHT). Under scrub.lock. */
static int scrub_room(void)
{
	return scrub.head - __atomic_load_n(&scrub.tail, __ATOMIC_ACQUIRE) < SCRUB_JOBS;
}

/* The work item: claim and queue the free pages not known to be zero, as long as there is room (see above). */
static void scrub_work(void *arg)
{
	u64 pa = 0;
	unsigned long bytes = 0;
	(void)arg;
	if (!gate_try())
		return; /* the free dirty pages stay as they are: the wake kicks this again */
	mutex_lock(&scrub.lock);
	while (scrub.chan && !READ_ONCE(scrub.chan->error) && scrub_room() &&
	       __atomic_load_n(&scrub.inflight, __ATOMIC_RELAXED) + SCRUB_CHUNK <= SCRUB_INFLIGHT) {
		bytes = nvxnu_pool_claim_dirty(SCRUB_CHUNK, (unsigned long long *)&pa);
		if (!bytes) {
			if (!scrub.boot_done) {
				scrub.boot_done = 1;
				nvxnu_log("tegra-gm20b: pool scrub: %llu MiB queued to the copy "
					  "engine %llu ms after M3 (the CPU cleared it at M1 "
					  "before)\n",
					  scrub.boot_bytes >> 20,
					  (nvxnu_nanotime() - scrub.boot_start) / NSEC_PER_MSEC);
			}
			break;
		}
		if (scrub_submit(pa, bytes)) {
			/* back as it was, dirty (the CPU zeroes it if taken), without kicking this work again */
			dma_free_attrs(&gpu_pdev.dev, bytes, NULL, pa, 0);
			break;
		}
		if (!scrub.boot_done)
			scrub.boot_bytes += bytes;
	}
	mutex_unlock(&scrub.lock);
	gate_leave();
}

/* Pages went back dirty: the work item claims and queues them (any context; nothing before the scrubber is up). */
static void scrub_kick(void)
{
	if (READ_ONCE(scrub.chan))
		nvxnu_work_schedule(scrub.work);
}

/* From the scrub channel's nonstall interrupt: every range whose fence passed goes back clean; then more claims. */
static void scrub_fenced(struct tgm_chan *chan)
{
	u32 done = tgm_chan_completed(chan);
	u32 tail = scrub.tail;
	u32 head = __atomic_load_n(&scrub.head, __ATOMIC_ACQUIRE);
	u64 now = nvxnu_nanotime();
	u64 took = 0;
	int freed = 0;

	while (tail != head && (s32)(done - scrub.job[tail % SCRUB_JOBS].seqno) >= 0) {
		u32 slot = tail % SCRUB_JOBS;
		nvxnu_pool_free_zeroed(scrub.job[slot].pa, scrub.job[slot].bytes);
		__atomic_fetch_sub(&scrub.inflight, scrub.job[slot].bytes, __ATOMIC_RELAXED);
		took = now - scrub.job[slot].queued;
		__atomic_fetch_add(&scrub.n, 1, __ATOMIC_RELAXED);
		__atomic_fetch_add(&scrub.bytes, scrub.job[slot].bytes, __ATOMIC_RELAXED);
		stats_add(&scrub.ns, &scrub.max_ns, took);
		__atomic_store_n(&scrub.tail, ++tail, __ATOMIC_RELEASE);
		freed = 1;
	}
	if (tail == head)
		nvxnu_work_cancel(scrub.deadline);
	if (freed && nvxnu_pool_dirty_free_pages())
		nvxnu_work_schedule(scrub.work);
}

/* A range not back SCRUB_NS after it was queued: the client channel the GPU is stuck on is killed and the range gets
 * SCRUB_NS more; with none, one line and a panic. */
static void scrub_deadline(void *arg)
{
	/* the last time a recovery or progress restarted the wait (one work item: no race) */
	static u64 recovered;
	u32 tail = __atomic_load_n(&scrub.tail, __ATOMIC_ACQUIRE);
	u32 head = __atomic_load_n(&scrub.head, __ATOMIC_ACQUIRE);
	u32 slot = tail % SCRUB_JOBS;
	enum tgm_hang verdict = TGM_HANG_STUCK;
	u64 age = 0;
	char state[TGM_HANG_LINE];

	(void)arg;
	if (tail == head)
		return;
	age = nvxnu_nanotime() - max(scrub.job[slot].queued, recovered);
	if (age < SCRUB_NS) {
		nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS - age);
		return;
	}
	age = nvxnu_nanotime() - scrub.job[slot].queued;
	if ((s32)(tgm_chan_completed(scrub.chan) - scrub.job[slot].seqno) >= 0) {
		nvxnu_panic(
			"tegra-gm20b: pool scrub fence %u complete after %llu ms but its nonstall "
			"interrupt never came",
			scrub.job[slot].seqno, age / 1000000);
	}
	mutex_lock(&live_lock);
	verdict = hang_recover(scrub.chan, "pool scrub", scrub.job[slot].seqno, age);
	mutex_unlock(&live_lock);
	if (verdict != TGM_HANG_STUCK) {
		recovered = nvxnu_nanotime();
		nvxnu_work_schedule_after(scrub.deadline, SCRUB_NS);
		return;
	}
	chan_describe(scrub.chan, state, sizeof(state));
	nvxnu_panic("tegra-gm20b: pool scrub of %llu KiB at %#llx not done %llu ms after it was "
		    "queued: scrub %s",
		    scrub.job[slot].bytes >> 10, scrub.job[slot].pa, age / 1000000, state);
}

/* After M3: the scrubber's address space (the whole pool mapped once), fence page and channel; then the boot scrub. One
 * line and no scrubber if any of it fails: pool pages are then zeroed by the CPU as they are taken. */
static void scrub_open(void)
{
	u64 npages = nvxnu_pool_size() >> PAGE_SHIFT;
	u64 i = 0;
	struct tgm_chan *chan = NULL;
	void *fence_va = NULL;
	int ret = 0;

	mutex_init(&scrub.lock);
	scrub.boot_start = nvxnu_nanotime();
	scrub.work = nvxnu_work_alloc(scrub_work, NULL);
	scrub.deadline = nvxnu_work_alloc(scrub_deadline, NULL);
	scrub.pool_pas =
		kcalloc(npages, sizeof(u64), GFP_KERNEL); /* nvkm keeps the array (mem.c) */
	fence_va = tgm_pool_alloc(PAGE_SIZE, 0, &scrub.fence_pa);
	ret = scrub.work && scrub.deadline && scrub.pool_pas && fence_va ? tgm_vm_new(&scrub.vm)
									 : -ENOMEM;
	for (i = 0; !ret && i < npages; i++)
		scrub.pool_pas[i] = nvxnu_pool_base_pa() + (i << PAGE_SHIFT);
	if (!ret)
		ret = tgm_mem_new(scrub.vm, scrub.pool_pas, (u32)npages, &scrub.pool_mem);
	if (!ret)
		ret = tgm_vm_kmap(scrub.vm, scrub.pool_mem, npages << PAGE_SHIFT, &scrub.pool_va);
	if (!ret)
		ret = tgm_mem_new(scrub.vm, &scrub.fence_pa, 1, &scrub.fence_mem);
	if (!ret)
		ret = tgm_vm_kmap(scrub.vm, scrub.fence_mem, PAGE_SIZE, &scrub.fence_gpu);
	scrub.fence = fence_va;
	if (!ret)
		ret = tgm_chan_new(scrub.vm, NULL, scrub.fence, scrub.fence_gpu, "scrub", 1, &chan);
	if (ret) {
		/* no teardown: what was made stays unused (a few pages); allocations zero on the CPU */
		nvxnu_log("tegra-gm20b: pool scrub unavailable (%d): pool pages are zeroed by the "
			  "CPU "
			  "as they are taken\n",
			  ret);
		return;
	}
	chan->kernel =
		CHAN_SCRUB; /* before anyone can submit on it: its nonstall goes to scrub_fenced */
	__atomic_store_n(&scrub.chan, chan, __ATOMIC_RELEASE);
	nvxnu_work_schedule(scrub.work);
}

int tgm_pool_scrub(uint64_t pa, uint64_t bytes)
{
	int ret = 0;
	if (!__atomic_load_n(&scrub.chan, __ATOMIC_ACQUIRE))
		return -ENODEV;
	if ((pa | bytes) & (PAGE_SIZE - 1) || !bytes || pa < nvxnu_pool_base_pa() ||
	    pa + bytes > nvxnu_pool_base_pa() + nvxnu_pool_size())
		return -EINVAL;
	if (!gate_try()) {
		if (READ_ONCE(gpu.railed))
			__atomic_fetch_add(&rail_ups.cpu_zeroed, bytes, __ATOMIC_RELAXED);
		return -EBUSY; /* the caller zeroes it on the CPU */
	}
	mutex_lock(&scrub.lock);
	if (!READ_ONCE(scrub.chan->error) && scrub_room()) {
		ret = scrub_submit(pa, bytes);
	} else {
		ret = -EBUSY;
	}
	mutex_unlock(&scrub.lock);
	gate_leave();
	return ret;
}

/* stats: this window's scrubbed ranges for the stats2 line */
static void scrub_stats(char *buf, unsigned int len, u64 window_ms)
{
	u64 n = __atomic_exchange_n(&scrub.n, 0, __ATOMIC_RELAXED);
	u64 bytes = __atomic_exchange_n(&scrub.bytes, 0, __ATOMIC_RELAXED);
	u64 ns = __atomic_exchange_n(&scrub.ns, 0, __ATOMIC_RELAXED);
	u64 max = __atomic_exchange_n(&scrub.max_ns, 0, __ATOMIC_RELAXED);
	(void)window_ms;
	scnprintf(buf, len,
		  "scrub %llu (%llu MiB, queued to done mean %llu us max %llu us), %lu KiB dirty",
		  n, bytes >> 20, n ? ns / n / 1000 : 0, max / 1000,
		  nvxnu_pool_dirty_free_pages() << (PAGE_SHIFT - 10u));
}

/* PTOP's engine list as nvkm parsed it (gk104_top_parse) and the PBDMAs serving each runlist: a copy engine on GR's
 * runlist (the GRCE) waits for the 3D engine at every switch of a channel to it; one on a runlist of its own overlaps. */
static void log_engines(void)
{
	struct nvkm_device *device = gpu.device;
	struct nvkm_top_device *tdev = NULL;
	struct nvkm_runl *runl = NULL;
	char line[256];
	unsigned int at = 0;
	int i = 0;

	if (!device->top || !device->fifo)
		return;
	list_for_each_entry(tdev, &device->top->device, head)
	{
		at += scnprintf(line + at, sizeof(line) - at,
				"%s%s%d engine %d runlist %d fault %d reset %d intr %d",
				at ? "; " : "",
				tdev->type < NVKM_SUBDEV_NR ? nvkm_subdev_type[tdev->type] : "?",
				tdev->inst, tdev->engine, tdev->runlist, tdev->fault, tdev->reset,
				tdev->intr);
	}
	nvkm_runl_foreach(runl, device->fifo)
	{
		at += scnprintf(line + at, sizeof(line) - at, "; runlist %d pbdma", runl->id);
		for (i = 0; i < runl->runq_nr; i++)
			at += scnprintf(line + at, sizeof(line) - at, " %d", runl->runq[i]->id);
	}
	nvxnu_log("tegra-gm20b: engines: %s\n", line);
}

static int m3(char *line, unsigned int len)
{
	struct tgm_vm *vm = NULL;
	struct tgm_chan *chan = NULL;
	struct tgm_mem *mem = NULL;
	u64 pa = 0;
	u64 pas[2];
	u64 va = 0;
	u64 fill = 0;
	u64 end = 0;
	u32 *page = NULL;
	u32 seqno = 0;
	u32 bad = 0;
	u32 i = 0;
	struct nvif_push *push = NULL;
	int ret = 0;
	int user = 0;

	/* page 0: fence word at +0, page 1: fill target. The copy engine fills page 1 through a user
	 * (L2-cacheable) mapping, so the check below also proves the fence's L2 flush. */
	page = tgm_pool_alloc(2 * PAGE_SIZE, 0, &pa);
	if (!page)
		return -ENOMEM;
	pas[0] = pa;
	pas[1] = pa + PAGE_SIZE;
	ret = tgm_vm_new(&vm);
	if (!ret)
		ret = tgm_mem_new(vm, pas, 2, &mem);
	if (!ret)
		ret = tgm_vm_kmap(vm, mem, 2 * PAGE_SIZE, &va);
	fill = va + PAGE_SIZE;
	if (!ret && l2_on()) {
		fill = 0x100000;
		ret = tgm_vm_map(vm, fill, PAGE_SIZE, mem, PAGE_SIZE, 0, 0);
		user = !ret;
	}
	if (!ret)
		ret = tgm_chan_new(vm, NULL, (volatile u32 *)page, va, "self-test", 1, &chan);
	if (ret)
		goto done;

	/* MAXWELL_DMA_COPY_A on subchannel 4: remap const A (0xc0ffee42) over 1024 dwords of page 1 */
	push = &chan->chan.push;
	ret = chan_push_wait(chan, 12 + CHAN_FENCE_DWORDS);
	if (ret)
		goto done;
	/* nouveau's NVDEF/NVVAL/PUSH_* macros compute in int and repeat their branches. */
	/* NOLINTBEGIN(bugprone-signed-bitwise,bugprone-branch-clone) */
	PUSH_MTHD(push, NVA0B5, SET_REMAP_CONST_A, 0xc0ffee42);
	PUSH_MTHD(push, NVA0B5, SET_REMAP_COMPONENTS,
		  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, DST_X, CONST_A) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, COMPONENT_SIZE, FOUR) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, NUM_SRC_COMPONENTS, ONE) |
			  NVDEF(NVA0B5, SET_REMAP_COMPONENTS, NUM_DST_COMPONENTS, ONE));
	PUSH_MTHD(push, NVA0B5, OFFSET_OUT_UPPER, upper_32_bits(fill), OFFSET_OUT_LOWER,
		  lower_32_bits(fill));
	PUSH_MTHD(push, NVA0B5, LINE_LENGTH_IN, PAGE_SIZE / 4, LINE_COUNT, 1);
	PUSH_MTHD(push, NVA0B5, LAUNCH_DMA,
		  NVDEF(NVA0B5, LAUNCH_DMA, DATA_TRANSFER_TYPE, NON_PIPELINED) |
			  NVDEF(NVA0B5, LAUNCH_DMA, FLUSH_ENABLE, TRUE) |
			  NVDEF(NVA0B5, LAUNCH_DMA, SRC_MEMORY_LAYOUT, PITCH) |
			  NVDEF(NVA0B5, LAUNCH_DMA, DST_MEMORY_LAYOUT, PITCH) |
			  NVDEF(NVA0B5, LAUNCH_DMA, REMAP_ENABLE, TRUE));
	/* NOLINTEND(bugprone-signed-bitwise,bugprone-branch-clone) */
	seqno = chan_fence(chan);
	end = nvxnu_nanotime() + NSEC_PER_SEC;
	for (;;) {
		u32 seq = nvxnu_wake_seq();
		if (READ_ONCE(page[0]) == seqno || chan->error || nvxnu_nanotime() >= end)
			break;
		if (!nvxnu_idle(seq, end) && READ_ONCE(page[0]) == seqno)
			nvxnu_wake_due(seq, "fence", seqno);
	}
	for (i = 0; i < PAGE_SIZE / 4; i++)
		bad += page[PAGE_SIZE / 4 + i] != 0xc0ffee42;
	ret = READ_ONCE(page[0]) != seqno ? -ETIMEDOUT : bad ? -EIO : 0;
done:
	scnprintf(line, len, "M3 %s vm=%s chan=%s fence=%u/%u ce_fill_bad=%u/1024%s err=%u ret=%d",
		  ret ? "FAIL" : "ok", vm ? "ok" : "no", chan ? "ok" : "no", page ? page[0] : 0,
		  seqno, bad, l2_on() ? " (L2-cached target)" : "", chan ? chan->error : 0, ret);
	tgm_chan_del(&chan);
	if (user)
		tgm_vm_unmap(vm, fill, PAGE_SIZE);
	tgm_mem_del(&mem);
	tgm_vm_del(&vm);
	tgm_pool_free(page, 2 * PAGE_SIZE, pa);
	return ret;
}

/* system sleep (TegraGM20BSleep.h):
 * SC7 powers the SoC off with DRAM in self-refresh: the GPU's registers, falcons, L2 and PTIMER are lost; its memory is
 * kept (pool and nvkm instmem pages: instance blocks, RAMFC, page tables, runlists, GR context buffers, ACR's blob, rings,
 * fence pages). Every nvif object is in the one client m2() made (the user client's connections, the present and the
 * scrubber are its vmm, mem and channel objects), so nouveau's own suspend and resume (nouveau_do_suspend/_resume: the
 * channels idle, nvif_client_suspend, nvif_client_resume) take all of them. Around them:
 *  sleep  the gate shut (submissions and new channels wait for the wake, the present and the scrubber are refused), every
 *         channel's last fence (interrupt-driven, TGM_HANG_NS, else tgm_chan_hung), gpu.lock taken and held to the wake
 *         (every other nvif call waits), the idle clock off, nvif_client_suspend (each GR context unbound after a
 *         preempt that saves it, each channel blocked, preempted and off its runlist, events blocked), the L2's dirty
 *         lines written back (the GPU writes RAMFC and contexts through it: nvkm's gk20a instmem flushes it before every
 *         CPU look, instmem/gk20a.c), nvkm_device_fini(NVKM_SUSPEND) (what nvkm_udevice_fini would run if m2() held no
 *         device reference), interrupts off, the MC client drained, then nvkm_device_tegra_power_down()'s steps.
 *  wake   nvkm_device_tegra_power_up()'s steps, interrupts on, then the boot's order: M1's PRI ring, timer and GPCPLL,
 *         clk_raise() (the relock under M1's quiet conditions, before any falcon runs), m2_device_init() (ACR's HS load
 *         again, which checks the GPU still reports the WPR it was built for; the PMU; every engine with a user, GR's
 *         FECS/GPCCS through the PMU), nvif_client_resume (channels bound and back on their runlists, GR contexts
 *         rebound, events allowed), one fence on a kernel channel, the idle clock, gpu.lock, the gate. */
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

	mutex_lock(&gate_lock);
	tgm_gate_close(&sc7_gate);
	inside = __atomic_load_n(&sc7_gate.inside, __ATOMIC_SEQ_CST);
	while (!tgm_gate_drained(&sc7_gate)) {
		if (!nvxnu_mutex_sleep(gate_lock.lck, &sc7_gate.inside, end) &&
		    !tgm_gate_drained(&sc7_gate)) {
			nvxnu_panic("tegra-gm20b: sleep: %u callers still in the GPU %llu ms after "
				    "its gate shut",
				    __atomic_load_n(&sc7_gate.inside, __ATOMIC_SEQ_CST),
				    (unsigned long long)(TGM_HANG_NS / 1000000));
		}
	}
	mutex_unlock(&gate_lock);
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
		mutex_lock(&live_lock);
		list_for_each_entry(chan, &live_chans, live)
		{
			chans++;
			if (busy || READ_ONCE(chan->error) ||
			    (s32)(tgm_chan_completed(chan) - chan->submitted) >= 0)
				continue;
			/* the gate is shut, so no work comes after this: what is stuck is killed, slow work gets more
			 * time, each up to SLEEP_RECOVERIES times */
			if (nvxnu_nanotime() >= end) {
				if (++recoveries > SLEEP_RECOVERIES ||
				    hang_recover(chan, "system sleep", chan->submitted,
						 nvxnu_nanotime() - start) == TGM_HANG_STUCK) {
					tgm_chan_hung(chan, "system sleep", chan->submitted,
						      nvxnu_nanotime() - start);
				}
				end = nvxnu_nanotime() + TGM_HANG_NS;
			}
			busy = 1;
			id = chan->id;
			seqno = chan->submitted;
		}
		mutex_unlock(&live_lock);
		if (!busy)
			break;
		waits++;
		if (!nvxnu_idle(seq, end) && sync_done(id, seqno))
			nvxnu_wake_due(seq, "fence", seqno);
	}
	tgm_journal("tegra-gm20b: sleep: %u channels idle (%u fence waits): %llu us", chans, waits,
		    us_since(start));
}

static void sleep_idle_off(void)
{
	sc7.khz = nvkm_clk_read(gpu.device->clk, nv_clk_src_gpc);
	sc7.idle_on = 0;
	if (!idle.clk)
		return;
	nvxnu_mutex_lock(idle.clk);
	sc7.idle_on = READ_ONCE(idle.on);
	WRITE_ONCE(idle.on, 0);
	if (READ_ONCE(idle.low)) {
		idle.low_ns += nvxnu_nanotime() - idle.low_since;
		WRITE_ONCE(idle.low, 0);
		sc7.khz = (int)idle.run_khz;
	}
	nvxnu_mutex_unlock(idle.clk);
}

static void sleep_nvkm(void)
{
	struct nvkm_device *device = gpu.device;
	u64 start = nvxnu_nanotime();
	int ret = nvif_client_suspend(&gpu.client, false);

	if (ret)
		nvxnu_panic("tegra-gm20b: sleep: nvkm object suspend failed: %d", ret);
	tgm_journal("tegra-gm20b: sleep: nvkm objects suspended (GR contexts saved, channels off "
		    "the runlist): %llu us",
		    us_since(start));
	start = nvxnu_nanotime();
	stamp_last = tgm_stamp_monotonic(stamp_last, nvkm_timer_read(device->timer));
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
	struct nvkm_device_tegra *tdev = gpu.device->func->tegra(gpu.device);
	u64 start = nvxnu_nanotime();
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
	u64 start = nvxnu_nanotime();
	int ret = power_up(gpu.device->func->tegra(gpu.device));
	int uv = 0;

	if (ret) {
		nvxnu_panic("tegra-gm20b: wake: GPU power-up failed: %d (VDD_GPU, clocks, clamp)",
			    ret);
	}
	nvxnu_irq_resume();
	tegra_gm20b_rail_get_uv(&uv);
	tgm_journal("tegra-gm20b: wake: VDD_GPU %d uV, clocks on, out of reset and clamp: %llu us",
		    uv, us_since(start));
}

/* After nvkm_timer_init, which restarts PTIMER from ktime: forward to the last GPU time before the power-down when the
 * GPU's clock had run ahead of ktime, so no time after the wake is behind one before it. */
static void stamp_reseed(struct nvkm_device *device)
{
	u64 now = (u64)ktime_to_ns(ktime_get());
	u64 floor = tgm_stamp_monotonic(stamp_last, now);

	if (floor == now)
		return;
	device->timer->func->time(device->timer, floor);
	nvkm_timer_alarm_trigger(device->timer);
}

/* M1 after nvkm_device_tegra_new() */
static void wake_m1(void)
{
	struct nvkm_device *device = gpu.device;
	u64 start = nvxnu_nanotime();
	u32 boot0 = nvkm_rd32(device, 0x000000);
	u32 ring = 0;
	int ret = 0;

	if (boot0 != gpu.boot0) {
		nvxnu_panic("tegra-gm20b: wake: BOOT_0 reads %08x after the power-up, %08x before "
			    "sleep",
			    boot0, gpu.boot0);
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
	struct nvkm_device *device = gpu.device;
	u64 start = nvxnu_nanotime();
	char line[200] = "";
	int ret = clk_raise(line, sizeof(line));
	int khz = nvkm_clk_read(device->clk, nv_clk_src_gpc);

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
	struct nvkm_device *device = gpu.device;
	struct nvkm_acr *acr = device->acr;
	u64 start = nvxnu_nanotime();
	u64 lo = 0;
	u64 hi = 0;
	const char *at = "";
	u32 mc[5];
	int ret = m2_device_init(device, &at);

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
	u64 start = nvxnu_nanotime();
	u32 chans = 0;
	u32 dead = 0;
	int ret = nvif_client_resume(&gpu.client);

	if (ret)
		nvxnu_panic("tegra-gm20b: wake: nvkm object resume failed: %d", ret);
	mutex_lock(&live_lock);
	list_for_each_entry(chan, &live_chans, live)
	{
		chans++;
		dead += READ_ONCE(chan->error) != 0;
	}
	mutex_unlock(&live_lock);
	tgm_journal("tegra-gm20b: wake: nvkm objects resumed (%u channels back on the runlist, %u "
		    "dead ones left off): %llu us",
		    chans - dead, dead, us_since(start));
}

/* One kernel fence through the host, PBDMA, semaphore release and nonstall interrupt before any user work. The scrubber's
 * channel when there is one: no caller waiting at the gate holds scrub.lock (present.lock is held around a channel
 * creation, which waits at the gate, while present.chan is still NULL). */
static void wake_fence(void)
{
	struct tgm_chan *chan = scrub.chan ? scrub.chan : present.chan;
	struct mutex *lock = scrub.chan ? &scrub.lock : &present.lock;
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

/* The idle clock back and the gate open, gpu.lock still held (system sleep's wake_open releases it first). */
static const char *wake_open_locked(void)
{
	const char *clock = "not in use";

	if (idle.clk && sc7.idle_on) {
		nvxnu_mutex_lock(idle.clk);
		if (((nvkm_rd32(gpu.device, GPCPLL_COEFF_R) >> 16) & 0x3f) == idle.run_pl) {
			idle.log_start = nvxnu_nanotime();
			__atomic_store_n(&idle.last_busy, idle.log_start, __ATOMIC_SEQ_CST);
			WRITE_ONCE(idle.on, 1);
			clock = "on";
		} else {
			clock = "OFF for this boot";
			nvxnu_log("tegra-gm20b: wake: GPCPLL COEFF %08x is not the idle clock's PL "
				  "%u: idle clock off, GPU at %d kHz\n",
				  nvkm_rd32(gpu.device, GPCPLL_COEFF_R), idle.run_pl, sc7.khz);
		}
		nvxnu_mutex_unlock(idle.clk);
	}
	return clock;
}

static void gate_open(void)
{
	mutex_lock(&gate_lock);
	tgm_gate_open(&sc7_gate);
	nvxnu_wakeup(&sc7_gate.shut);
	mutex_unlock(&gate_lock);
	scrub_kick();
}

static void wake_open(void)
{
	u64 start = nvxnu_nanotime();
	const char *clock = wake_open_locked();

	mutex_unlock(&gpu.lock);
	gate_open();
	tgm_journal("tegra-gm20b: wake: submissions open, idle clock %s: %llu us", clock,
		    us_since(start));
}

void tgm_sleep(void)
{
	sc7.plan = tgm_sleep_plan(gpu.stage, gpu.device != NULL);
	switch (sc7.plan) {
	case TGM_SLEEP_NOTHING:
		return;
	case TGM_SLEEP_POWER:
		switch_sleep_mark(SWITCH_STEP_GPU_POWER_DOWN);
		sleep_power_down();
		switch_sleep_mark(SWITCH_STEP_GPU_DOWN);
		tgm_journal("tegra-gm20b: sleep: no nvkm client (bring-up stopped after stage %d): "
			    "powered down",
			    gpu.stage);
		return;
	case TGM_SLEEP_FULL:
		break;
	}
	switch_sleep_mark(SWITCH_STEP_GPU_GATE);
	sleep_gate();
	tgm_deadline_suspend();
	switch_sleep_mark(SWITCH_STEP_GPU_FENCES);
	sleep_fences();
	mutex_lock(&gpu.lock);
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
			    gpu.stage);
		return;
	case TGM_SLEEP_FULL:
		break;
	}
	wake_power_up();
	wake_m1();
	wake_clock();
	wake_device();
	tgm_recover_arm(gpu.device);
	wake_nvkm();
	wake_fence();
	tgm_clkgate_enable(gpu.device);
	wake_open();
	tgm_deadline_resume();
}

/* Rail-gating (TegraGM20BRail.h), from TegraGM20B.cpp's rail worker: system sleep's sleep steps, its durable trail
 * left alone, then gpu.lock released with the GPU marked down and the gate left shut. 0 (down), or -EBUSY for a GPU
 * without an nvkm client (it stays as it is). */
int tgm_rail_down(void)
{
	u64 start = nvxnu_nanotime();

	if (__atomic_load_n(&gpu.railed, __ATOMIC_SEQ_CST))
		return 0;
	if (tgm_sleep_plan(gpu.stage, gpu.device != NULL) != TGM_SLEEP_FULL)
		return -EBUSY;
	sleep_gate();
	tgm_deadline_suspend();
	sleep_fences();
	mutex_lock(&gpu.lock);
	sleep_idle_off();
	sleep_nvkm();
	sleep_power_down();
	__atomic_store_n(&gpu.railed, 1, __ATOMIC_SEQ_CST);
	mutex_unlock(&gpu.lock);
	/* callers waiting at the gate now wake the GPU themselves (gate_enter) */
	mutex_lock(&gate_lock);
	nvxnu_wakeup(&sc7_gate.shut);
	mutex_unlock(&gate_lock);
	tgm_journal("tegra-gm20b: rail down in %llu us", us_since(start));
	return 0;
}

/* The caller holds gpu.lock and finds the GPU rail-gated: system sleep's wake steps under that lock, and the gate open.
 * Not wake_fence: its kernel channel's lock may be the caller's (present.lock around a channel creation), and the
 * caller's own work is the GPU's next test. `site` is the caller's return address, NULL for the display. */
static void rail_up_locked(const void *site)
{
	u64 start = nvxnu_nanotime();
	const char *clock = NULL;

	tgm_rail_moving();
	wake_power_up();
	wake_m1();
	wake_clock();
	wake_device();
	tgm_recover_arm(gpu.device);
	wake_nvkm();
	tgm_clkgate_enable(gpu.device);
	clock = wake_open_locked();
	/* The gate opens before railed clears: a closer acts only once it has seen the GPU up (tgm_rail_down, the worker's
	 * tgm_railed(), SwitchSuspend's tgm_rail_sc7_hold), so its close can never land before this open and be undone. */
	gate_open();
	__atomic_store_n(&gpu.railed, 0, __ATOMIC_SEQ_CST);
	tgm_deadline_resume();
	if (site) {
		if (!rail_ups.demand++)
			rail_ups.first_site = site;
	} else {
		rail_ups.display++;
	}
	/* the first site as an offset from tgm_stage: KASLR-free, resolved against the kernel's symbols */
	tgm_journal("tegra-gm20b: rail up in %llu us, idle clock %s (rail: %u up (display %u, "
		    "demand %u by "
		    "tgm_stage%+ld); %llu KiB zeroed on the CPU while down)",
		    us_since(start), clock, rail_ups.display + rail_ups.demand, rail_ups.display,
		    rail_ups.demand,
		    rail_ups.first_site
			    ? (long)((unsigned long)rail_ups.first_site - (unsigned long)tgm_stage)
			    : 0L,
		    (unsigned long long)(__atomic_load_n(&rail_ups.cpu_zeroed, __ATOMIC_RELAXED) >>
					 10));
	tgm_rail_moved(site != NULL);
}

/* The rail worker, for the display coming on: the GPU up if it is rail-gated, on this thread. */
void tgm_rail_up(void)
{
	mutex_lock(&gpu.lock);
	if (gpu.railed)
		rail_up_locked(NULL);
	mutex_unlock(&gpu.lock);
}

int tgm_railed(void)
{
	return __atomic_load_n(&gpu.railed, __ATOMIC_SEQ_CST);
}

/* System sleep with the GPU rail-gated, on SwitchSuspend's one thread: gpu.lock held from its suspend to its resume, as
 * tgm_sleep holds it, so no caller wakes the GPU in between; whoever needs it after the resume wakes it then. 1 when
 * held (the GPU was down), 0 when the caller must take system sleep's own path (tgm_sleep). */
int tgm_rail_sc7_hold(void)
{
	mutex_lock(&gpu.lock);
	if (gpu.railed)
		return 1;
	mutex_unlock(&gpu.lock);
	return 0;
}

void tgm_rail_sc7_release(void)
{
	mutex_unlock(&gpu.lock);
}

int tgm_bringup(int stage, char *line, unsigned int len)
{
	int ret = 0;
	line[0] = 0;
	if (gpu.stage < 1 && stage >= 1) {
		ret = m1(line, len);
		if (ret)
			return ret;
		gpu.stage = 1;
	}
	if (gpu.stage < 2 && stage >= 2) {
		ret = clk_raise(line, len); /* GPCPLL above 76.8 MHz before ACR/PMU/GR */
		if (!ret)
			ret = m2(line, len);
		if (ret)
			return ret;
		tgm_clkgate_load(gpu.device);
		tgm_clkgate_guard_reset(gpu.device);
		tgm_recover_init(gpu.device);
		tgm_recover_arm(gpu.device);
		tgm_sched_init(gpu.device);
		gpu.stage = 2;
	}
	if (gpu.stage < 3 && stage >= 3) {
		ret = m3(line, len);
		if (ret)
			return ret;
		tgm_clkgate_enable(gpu.device);
		gpu.stage = 3;
		if (stats_on())
			log_engines();
		if (tgm_all_clients())
			iso.deadline = nvxnu_work_alloc(iso_deadline, NULL);
		nvxnu_errors_fatal(iso.deadline ? errors_contained : NULL);
		if (tgm_coherency_init())
			nvxnu_log("tegra-gm20b: no coherency registry: imports are cleaned once\n");
		idle_start(); /* 768 MHz while busy, 76.8 MHz once truly idle */
		scrub_open(); /* the copy engine zeroes the pool from here on (see "pool scrub") */
	}
	return ret;
}
