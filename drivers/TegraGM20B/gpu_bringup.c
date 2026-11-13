// SPDX-License-Identifier: MIT
// GM20B bring-up (M1, M2, M3), the platform device, the GPU lock and the sleep gate's entry points.
#include "gpu_internal.h"

/* mainline tegra210.dtsi gpu@57000000: BAR0, BAR1, SPI 157 ("stall") / 158 ("nonstall") */
static struct resource gpu_res[] = {
	{.start = 0x57000000, .end = 0x57ffffff, .name = "bar0", .flags = IORESOURCE_MEM},
	{.start = 0x58000000, .end = 0x58ffffff, .name = "bar1", .flags = IORESOURCE_MEM},
};
struct platform_device tgm_gpu_pdev = {
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
struct gpu_state tgm_gpu = {.lock = {NULL}};

#if TGM_STATS
/* Rail-gating's count of wakes, for its journal line: how many the display asked for, how many work at tgm_gpu.lock or the
 * gate did, where the first such caller came from, and the pool bytes its callers zeroed on the CPU while it was down. */
struct rail_ups_state tgm_rail_ups;

#endif
/* The last GPU time handed out (tgm_timestamp_ns) or read before a power-down (sleep_nvkm), under tgm_gpu.lock: the floor of
 * every GPU time after it (tgm_stamp_monotonic). */
u64 tgm_stamp_last;

/* tgm_gpu.lock for an nvif call. A rail-gated GPU is woken here, by the caller that needs it, under the lock it owns: the
 * next holder finds the GPU up, and nothing reaches its registers while it is down. */
noinline void tgm_gpu_lock(void)
{
	mutex_lock(&tgm_gpu.lock);
	if (unlikely(tgm_gpu.railed))
		tgm_rail_up_locked(__builtin_return_address(0));
}

int tgm_stage(void)
{
	return tgm_gpu.stage;
}
#if TGM_STATS
/* Debug kernels: clock gating off (nvgpu's disable values, ELCG run) or on again, for a frame-time A/B within one boot;
 * under tgm_gpu.lock, once the bring-up has reached M3. */
int tgm_clkgate_switch(int on)
{
	int ret = 0;

	tgm_gpu_lock();
	if (tgm_gpu.stage < 3) {
		ret = -EBUSY;
	} else if (on) {
		tgm_clkgate_enable(tgm_gpu.device);
	} else {
		tgm_clkgate_disable(tgm_gpu.device);
	}
	mutex_unlock(&tgm_gpu.lock);
	nvxnu_log("tegra-gm20b: clock gating %s (debug A/B)%s\n", on ? "on" : "off",
		  ret ? ": not yet, the bring-up has not reached M3" : "");
	return ret;
}
#endif
volatile u32 tgm_waiter_words[TGM_WAITERS];
#if TGM_STATS
/* stats: when each word was last woken (ns), and this window's wakes and fence waits (the "waits" line) */
u64 tgm_waiter_woken_ns[TGM_WAITERS];
struct wstat_state tgm_wstat;

#endif
uint32_t tgm_waiter_seq(uint32_t waiter)
{
	return nvxnu_word_seq(&tgm_waiter_words[waiter % TGM_WAITERS]);
}

int tgm_waiter_idle(uint32_t waiter, uint32_t seq, uint64_t deadline_ns)
{
	return nvxnu_word_idle(&tgm_waiter_words[waiter % TGM_WAITERS], seq, deadline_ns);
}

void tgm_waiter_unwoken(uint32_t waiter, uint32_t seq, uint32_t seqno)
{
	nvxnu_word_wake_due(&tgm_waiter_words[waiter % TGM_WAITERS], seq, "fence", seqno);
}

/* System sleep's gate (TegraGM20BSleepGate.h, "system sleep" below): what touches the GPU outside tgm_gpu.lock goes through it. */
struct tgm_gate tgm_sc7_gate;
DEFINE_MUTEX(tgm_gate_lock);

/* In, waiting for the wake while the gate is shut. */
void tgm_gate_enter(void)
{
	enum tgm_gate_entry entry = TGM_GATE_IN;

	while ((entry = tgm_gate_try_enter(&tgm_sc7_gate)) != TGM_GATE_IN) {
		if (READ_ONCE(tgm_gpu.railed)) {
			if (entry == TGM_GATE_SHUT_WAKE)
				gate_wake_closer();
			tgm_gpu_lock();
			mutex_unlock(&tgm_gpu.lock);
			continue;
		}
		mutex_lock(&tgm_gate_lock);
		if (entry == TGM_GATE_SHUT_WAKE)
			nvxnu_wakeup(&tgm_sc7_gate.inside);
		while (tgm_gate_is_shut(&tgm_sc7_gate) && !READ_ONCE(tgm_gpu.railed))
			nvxnu_mutex_sleep(tgm_gate_lock.lck, &tgm_sc7_gate.shut, ~0ULL);
		mutex_unlock(&tgm_gate_lock);
	}
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
	if (TGM_STATS && nvxnu_trace_paced())
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
	ret = nvkm_device_tegra_new(&gm20b_platform_data, &tgm_gpu_pdev, "", "error", &device);
	if (ret) {
		scnprintf(line, len, "M1 FAIL nvkm_device_tegra_new %d", ret);
		return ret;
	}
	tgm_gpu.device = device;
	nvxnu_step("tegra-gm20b: M1 step: device constructed, claim GPU interrupts\n");
	/* nvkm's handler is recorded (request_irq in nvkm_device_ctor) and nothing is armed yet: claim the GPU lines
	 * before the first nvkm init that may wait */
	nvxnu_irq_start();
	boot0 = nvkm_rd32(device, 0x000000);
	tgm_gpu.boot0 = boot0;
	tegra_gm20b_rail_get_uv(&uv);
	nvxnu_step("tegra-gm20b: M1 step: BOOT_0 %08x, rail %d uV; init PRI ring\n", boot0, uv);

	ret = nvkm_subdev_init(device->privring);
	if (!ret)
		ret = nvkm_subdev_init(&device->timer->subdev);
	if (!ret)
		tgm_clock_anchor_publish(device);
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

/* One M2 trace line: the host's PMC/bus registers (always answer) and, over the PRI ring, the ring master's interrupt
 * status and the GPCPLL output, so a step that leaves the ring or the GPU clock dead shows on the next line. */
static void m2_step(struct nvkm_device *device, const char *what, const char *name)
{
	if (!TGM_STATS)
		return;
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
int tgm_m2_device_init(struct nvkm_device *device, const char **at)
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
	struct nvkm_device *device = tgm_gpu.device;
	struct nvkm_engine *gr = NULL;
	const char *at = "";
	u64 units = 0;
	int ret = tgm_m2_device_init(
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
	tgm_zcull_init(device);

	mutex_init(&tgm_gpu.lock);
	at = "nvif_driver_init";
	ret = nvif_driver_init(NULL, "", "error", "tegra-gm20b", device->handle, &tgm_gpu.client);
	if (!ret) {
		/* every object made below inherits this printer (nvif/object.c: object->parent = parent->parent) */
		nvif_parent_ctor(&tgm_nvif_parent_func, &tgm_nvif_parent);
		tgm_gpu.client.object.parent = &tgm_nvif_parent;
		at = "nvif_device_ctor";
		ret = nvif_device_ctor(&tgm_gpu.client, "dev", &tgm_gpu.dev);
	}
	if (!ret) {
		/* the MMU class is a child of the device object, as nouveau_drm.c creates it (on the client: -22) */
		at = "nvif_mmu_ctor";
		ret = nvif_mmu_ctor(&tgm_gpu.dev.object, "mmu", NVIF_CLASS_MMU_GF100, &tgm_gpu.mmu);
	}
	if (!ret) {
		at = "nvif_mmu_type";
		tgm_gpu.mem_type = nvif_mmu_type(&tgm_gpu.mmu, NVIF_MEM_HOST | NVIF_MEM_KIND);
		if (tgm_gpu.mem_type < 0)
			tgm_gpu.mem_type = nvif_mmu_type(&tgm_gpu.mmu, NVIF_MEM_HOST);
		ret = tgm_gpu.mem_type < 0 ? tgm_gpu.mem_type : 0;
	}
	if (ret) {
		scnprintf(line, len, "M2 FAIL %s %d (GR came up: see the M2 ok line above)", at,
			  ret);
	}
	return ret;
}

void tgm_get_info(struct tgm_info *info)
{
	u64 units = tgm_gpu.stage >= 2 ? nvkm_gr_units(tgm_gpu.device->gr) : 0;
	memset(info, 0, sizeof(*info));
	if (!tgm_gpu.device)
		return;
	info->boot0 = tgm_gpu.boot0;
	info->chipset = tgm_gpu.device->chipset;
	info->gpc_count = units & 0xff;
	info->tpc_count = (units >> 8) & 0xffff;
	info->comptag_lines = tgm_gpu.device->ltc->num_tags;
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

	if (tgm_gpu.stage < 2 || !tgm_gpu.device->ltc->num_tags)
		return -ENODEV;
	gr = gf100_gr(tgm_gpu.device->gr);
	tgm_gpu_lock();
	if (depth && format == FERMI_A_ZBC_DEPTH_V0_FMT_FP32)
		ret = gf100_gr_zbc_depth_get(gr, format, ds[0], l2[0]);
	for (i = 0; !depth && i < ARRAY_SIZE(color_formats); i++) {
		if (format == color_formats[i])
			ret = gf100_gr_zbc_color_get(gr, format, ds, l2);
	}
	mutex_unlock(&tgm_gpu.lock);
	return ret;
}

uint64_t tgm_timestamp_ns(void)
{
	u64 t = 0;
	if (tgm_gpu.stage < 2)
		return 0;
	mutex_lock(&tgm_gpu.lock);
	/* a rail-gated GPU's time is ktime, without a wake: PTIMER restarts from it (stamp_reseed) */
	t = tgm_gpu.railed ? (u64)ktime_to_ns(ktime_get()) : nvif_device_time(&tgm_gpu.dev);
	tgm_stamp_last = t = tgm_stamp_monotonic(tgm_stamp_last, t);
	mutex_unlock(&tgm_gpu.lock);
	return t;
}

/* PTOP's engine list as nvkm parsed it (gk104_top_parse) and the PBDMAs serving each runlist: a copy engine on GR's
 * runlist (the GRCE) waits for the 3D engine at every switch of a channel to it; one on a runlist of its own overlaps. */
static void log_engines(void)
{
	struct nvkm_device *device = tgm_gpu.device;
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
	push->cur += tgm_fill_setup(push->cur, 0xc0ffee42);
	push->cur += tgm_fill_lines(push->cur, fill, PAGE_SIZE, 1, 0);
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

int tgm_bringup(int stage, char *line, unsigned int len)
{
	int ret = 0;
	line[0] = 0;
	if (tgm_gpu.stage < 1 && stage >= 1) {
		ret = m1(line, len);
		if (ret)
			return ret;
		tgm_gpu.stage = 1;
	}
	if (tgm_gpu.stage < 2 && stage >= 2) {
		ret = tgm_clk_raise(line, len); /* GPCPLL above 76.8 MHz before ACR/PMU/GR */
		if (!ret)
			ret = m2(line, len);
		if (ret)
			return ret;
		tgm_clkgate_load(tgm_gpu.device);
		tgm_clkgate_guard_reset(tgm_gpu.device);
		tgm_recover_init(tgm_gpu.device);
		tgm_recover_arm(tgm_gpu.device);
		tgm_sched_init(tgm_gpu.device);
		tgm_gpu.stage = 2;
	}
	if (tgm_gpu.stage < 3 && stage >= 3) {
		ret = m3(line, len);
		if (ret)
			return ret;
		tgm_clkgate_enable(tgm_gpu.device);
		tgm_gpu.stage = 3;
		if (stats_on())
			log_engines();
		if (tgm_all_clients())
			tgm_iso.deadline = nvxnu_work_alloc(tgm_iso_deadline, NULL);
		nvxnu_errors_fatal(tgm_iso.deadline ? tgm_errors_contained : NULL);
		if (tgm_coherency_init())
			nvxnu_log("tegra-gm20b: no coherency registry: imports are cleaned once\n");
		tgm_idle_start(); /* 768 MHz while busy, 76.8 MHz once truly idle */
		tgm_scrub_open(); /* the copy engine zeroes the pool from here on (see "pool scrub") */
	}
	return ret;
}
