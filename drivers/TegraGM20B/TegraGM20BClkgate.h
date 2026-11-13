/*
 * GM20B clock gating as nvgpu programs it: second-level (SLCG) and block-level (BLCG) gating register lists, and
 * engine-level gating (ELCG) on every engine with its idle filter. All of it is the GPU's own hardware gating its
 * clocks while a unit has nothing to do, with no state lost and no wake-up cost on our side.
 *
 * The lists are nvgpu's gm20b_gating_reglist.c (switch-l4t-kernel-nvgpu 1ae0167d, kept verbatim in
 * nvidia/gm20b_gating_reglist.c), the "prod" value of each entry, in the order nvgpu loads them on GM20B:
 * nvgpu_init_priv_ring (priring), nvgpu_init_mm_setup_hw (fb, ltc), gk20a_init_fifo_setup_hw (fifo),
 * nvgpu_cg_init_gr_load_gating_prod (bus, chiplet, gr, perf, xbar; then BLCG bus, gr, xbar), gk20a_init_ce_support
 * (ce2) and nvgpu_pmu_enable_hw (pmu). nvgpu leaves the pwr_csb and therm lists unloaded and so do we. ELCG:
 * gm20b_elcg_init_idle_filters (each engine's idle filter 2 x 2^9 = 1024 clocks; the FECS and HUBMMU idle filters 0)
 * and gm20b_therm_init_elcg_mode(ELCG_AUTO). The engine BLCG mode (THERM_GATE_CTRL blk_clk) stays as reset left it, as
 * nvgpu's boot leaves it. GR's reset in recovery runs with gating off and loads every list again after it
 * (tgm_clkgate_guard_reset).
 *
 * The tables are:
 *
 * Copyright (c) 2014-2018, NVIDIA CORPORATION.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#ifndef TEGRA_GM20B_CLKGATE_H
#define TEGRA_GM20B_CLKGATE_H

#include <stdint.h>

struct tgm_cg_reg {
	uint32_t addr, prod, disable;
};

static const struct tgm_cg_reg tgm_slcg_priring[] = {
	{0x001200a8, 0x00000000, 0x00000001},
};
static const struct tgm_cg_reg tgm_slcg_fb[] = {
	{0x00100d14, 0x00000000, 0xfffffffe},
	{0x00100c9c, 0x00000000, 0x000001fe},
};
static const struct tgm_cg_reg tgm_slcg_ltc[] = {
	{0x0017e050, 0x00000000, 0xfffffffe},
	{0x0017e35c, 0x00000000, 0xfffffffe},
};
static const struct tgm_cg_reg tgm_blcg_fb[] = {
	{0x00100d10, 0x0000c242, 0x00000000}, {0x00100d30, 0x0000c242, 0x00000000},
	{0x00100d3c, 0x00000242, 0x00000000}, {0x00100d48, 0x0000c242, 0x00000000},
	{0x00100c98, 0x00000242, 0x00000000},
};
static const struct tgm_cg_reg tgm_blcg_ltc[] = {
	{0x0017e030, 0x00000044, 0x00000000},
	{0x0017e040, 0x00000044, 0x00000000},
	{0x0017e3e0, 0x00000044, 0x00000000},
	{0x0017e3c8, 0x00000044, 0x00000000},
};
static const struct tgm_cg_reg tgm_slcg_fifo[] = {
	{0x000026ac, 0x00000100, 0x0001fffe},
};
static const struct tgm_cg_reg tgm_blcg_fifo[] = {
	{0x000026a4, 0x0000c242, 0x00000000},
};
static const struct tgm_cg_reg tgm_slcg_bus[] = {
	{0x00001c04, 0x00000000, 0x000003fe},
};
static const struct tgm_cg_reg tgm_slcg_chiplet[] = {
	{0x0010c07c, 0x00000000, 0x00000007},
	{0x0010e07c, 0x00000000, 0x00000007},
	{0x0010d07c, 0x00000000, 0x00000007},
	{0x0010e17c, 0x00000000, 0x00000007},
};
static const struct tgm_cg_reg tgm_slcg_gr[] = {
	{0x004041f4, 0x00000000, 0x03fffffe}, {0x0040917c, 0x00020008, 0x0003fffe},
	{0x00409894, 0x00000040, 0x0003fffe}, {0x004078c4, 0x00000000, 0x000001fe},
	{0x00406004, 0x00000000, 0x0001fffe}, {0x00405864, 0x00000000, 0x000001fe},
	{0x00405910, 0xfffffff0, 0xfffffffe}, {0x00408044, 0x00000000, 0x000007fe},
	{0x00407004, 0x00000000, 0x0000007e}, {0x0041a17c, 0x00020008, 0x0003fffe},
	{0x0041a894, 0x00000040, 0x0003fffe}, {0x00418504, 0x00000000, 0x0007fffe},
	{0x0041860c, 0x00000000, 0x000001fe}, {0x0041868c, 0x00000000, 0x0000001e},
	{0x0041871c, 0x00000000, 0x0000003e}, {0x00418388, 0x00000000, 0x00000001},
	{0x0041882c, 0x00000000, 0x0001fffe}, {0x00418bc0, 0x00000000, 0x000001fe},
	{0x00418974, 0x00000000, 0x0001fffe}, {0x00418c74, 0xffffffc0, 0xfffffffe},
	{0x00418cf4, 0xfffffffc, 0xfffffffe}, {0x00418d74, 0xffffffe0, 0xfffffffe},
	{0x00418f10, 0xffffffe0, 0xfffffffe}, {0x00418e10, 0xfffffffe, 0xfffffffe},
	{0x00419024, 0x000001fe, 0x000001fe}, {0x0041889c, 0x00000000, 0x000001fe},
	{0x00419d64, 0x00000000, 0x000001ff}, {0x00419a44, 0x00000000, 0x0000000e},
	{0x00419a4c, 0x00000000, 0x000001fe}, {0x00419a54, 0x00000000, 0x0000003e},
	{0x00419a5c, 0x00000000, 0x0000000e}, {0x00419a64, 0x00000000, 0x000001fe},
	{0x00419a6c, 0x00000000, 0x0000000e}, {0x00419a74, 0x00000000, 0x0000000e},
	{0x00419a7c, 0x00000000, 0x0000003e}, {0x00419a84, 0x00000000, 0x0000000e},
	{0x0041986c, 0x00000104, 0x00fffffe}, {0x00419cd8, 0x00000000, 0x001ffffe},
	{0x00419ce0, 0x00000000, 0x001ffffe}, {0x00419c74, 0x0000001e, 0x0000001e},
	{0x00419fd4, 0x00000000, 0x0003fffe}, {0x00419fdc, 0xffedff00, 0xfffffffe},
	{0x00419fe4, 0x00001b00, 0x00001ffe}, {0x00419ff4, 0x00000000, 0x00003ffe},
	{0x00419ffc, 0x00000000, 0x0001fffe}, {0x0041be2c, 0x04115fc0, 0xfffffffe},
	{0x0041bfec, 0xfffffff0, 0xfffffffe}, {0x0041bed4, 0xfffffff6, 0xfffffffe},
	{0x00408814, 0x00000000, 0x0001fffe}, {0x0040881c, 0x00000000, 0x0001fffe},
	{0x00408a84, 0x00000000, 0x0001fffe}, {0x00408a8c, 0x00000000, 0x0001fffe},
	{0x00408a94, 0x00000000, 0x0001fffe}, {0x00408a9c, 0x00000000, 0x0001fffe},
	{0x00408aa4, 0x00000000, 0x0001fffe}, {0x00408aac, 0x00000000, 0x0001fffe},
	{0x004089ac, 0x00000000, 0x0001fffe}, {0x00408a24, 0x00000000, 0x000001ff},
};
static const struct tgm_cg_reg tgm_slcg_perf[] = {
	{0x001be018, 0x000001ff, 0x00000000},
	{0x001bc018, 0x000001ff, 0x00000000},
	{0x001b8018, 0x000001ff, 0x00000000},
	{0x001b4124, 0x00000001, 0x00000000},
};
static const struct tgm_cg_reg tgm_slcg_xbar[] = {
	{0x0013cbe4, 0x00000000, 0x1ffffffe},
	{0x0013cc04, 0x00000000, 0x1ffffffe},
};
static const struct tgm_cg_reg tgm_blcg_bus[] = {
	{0x00001c00, 0x00000042, 0x00000000},
};
static const struct tgm_cg_reg tgm_blcg_gr[] = {
	{0x004041f0, 0x00004046, 0x00000000}, {0x00409890, 0x0000007f, 0x00000000},
	{0x004098b0, 0x0000007f, 0x00000000}, {0x004078c0, 0x00000042, 0x00000000},
	{0x00406000, 0x00004044, 0x00000000}, {0x00405860, 0x00004042, 0x00000000},
	{0x0040590c, 0x00004044, 0x00000000}, {0x00408040, 0x00004044, 0x00000000},
	{0x00407000, 0x00004041, 0x00000000}, {0x00405bf0, 0x00004044, 0x00000000},
	{0x0041a890, 0x0000007f, 0x00000000}, {0x0041a8b0, 0x0000007f, 0x00000000},
	{0x00418500, 0x00004044, 0x00000000}, {0x00418608, 0x00004042, 0x00000000},
	{0x00418688, 0x00004042, 0x00000000}, {0x00418718, 0x00000042, 0x00000000},
	{0x00418828, 0x00000044, 0x00000000}, {0x00418bbc, 0x00004042, 0x00000000},
	{0x00418970, 0x00004042, 0x00000000}, {0x00418c70, 0x00004044, 0x00000000},
	{0x00418cf0, 0x00004044, 0x00000000}, {0x00418d70, 0x00004044, 0x00000000},
	{0x00418f0c, 0x00004044, 0x00000000}, {0x00418e0c, 0x00004044, 0x00000000},
	{0x00419020, 0x00004042, 0x00000000}, {0x00419038, 0x00000042, 0x00000000},
	{0x00418898, 0x00000042, 0x00000000}, {0x00419a40, 0x00000042, 0x00000000},
	{0x00419a48, 0x00004042, 0x00000000}, {0x00419a50, 0x00004042, 0x00000000},
	{0x00419a58, 0x00004042, 0x00000000}, {0x00419a60, 0x00004042, 0x00000000},
	{0x00419a68, 0x00004042, 0x00000000}, {0x00419a70, 0x00004042, 0x00000000},
	{0x00419a78, 0x00004042, 0x00000000}, {0x00419a80, 0x00004042, 0x00000000},
	{0x00419868, 0x00000042, 0x00000000}, {0x00419cd4, 0x00000002, 0x00000000},
	{0x00419cdc, 0x00000002, 0x00000000}, {0x00419c70, 0x00004044, 0x00000000},
	{0x00419fd0, 0x00004044, 0x00000000}, {0x00419fd8, 0x00004046, 0x00000000},
	{0x00419fe0, 0x00004044, 0x00000000}, {0x00419fe8, 0x00000042, 0x00000000},
	{0x00419ff0, 0x00004045, 0x00000000}, {0x00419ff8, 0x00000002, 0x00000000},
	{0x00419f90, 0x00000002, 0x00000000}, {0x0041be28, 0x00000042, 0x00000000},
	{0x0041bfe8, 0x00004044, 0x00000000}, {0x0041bed0, 0x00004044, 0x00000000},
	{0x00408810, 0x00004042, 0x00000000}, {0x00408818, 0x00004042, 0x00000000},
	{0x00408a80, 0x00004042, 0x00000000}, {0x00408a88, 0x00004042, 0x00000000},
	{0x00408a90, 0x00004042, 0x00000000}, {0x00408a98, 0x00004042, 0x00000000},
	{0x00408aa0, 0x00004042, 0x00000000}, {0x00408aa8, 0x00004042, 0x00000000},
	{0x004089a8, 0x00004042, 0x00000000}, {0x004089b0, 0x00000042, 0x00000000},
	{0x004089b8, 0x00004042, 0x00000000},
};
static const struct tgm_cg_reg tgm_blcg_xbar[] = {
	{0x0013cbe0, 0x00000042, 0x00000000},
	{0x0013cc00, 0x00000042, 0x00000000},
};
static const struct tgm_cg_reg tgm_slcg_ce2[] = {
	{0x00106f28, 0x00000000, 0x000007fe},
};
static const struct tgm_cg_reg tgm_slcg_pmu[] = {
	{0x0010a17c, 0x00020008, 0x0003fffe},
	{0x0010aa74, 0x00000000, 0x00007ffe},
	{0x0010ae74, 0x00000000, 0x0000000f},
};
static const struct tgm_cg_reg tgm_blcg_pmu[] = {
	{0x0010aa70, 0x00000045, 0x00000000},
};

#define TGM_CG_LIST(name) {#name, tgm_##name, sizeof(tgm_##name) / sizeof(tgm_##name[0])}

static const struct tgm_cg_list {
	const char *name;
	const struct tgm_cg_reg *regs;
	unsigned int count;
} tgm_cg_lists[] = {
	TGM_CG_LIST(slcg_priring), TGM_CG_LIST(slcg_fb),   TGM_CG_LIST(slcg_ltc),
	TGM_CG_LIST(blcg_fb),      TGM_CG_LIST(blcg_ltc),  TGM_CG_LIST(slcg_fifo),
	TGM_CG_LIST(blcg_fifo),    TGM_CG_LIST(slcg_bus),  TGM_CG_LIST(slcg_chiplet),
	TGM_CG_LIST(slcg_gr),      TGM_CG_LIST(slcg_perf), TGM_CG_LIST(slcg_xbar),
	TGM_CG_LIST(blcg_bus),     TGM_CG_LIST(blcg_gr),   TGM_CG_LIST(blcg_xbar),
	TGM_CG_LIST(slcg_ce2),     TGM_CG_LIST(slcg_pmu),  TGM_CG_LIST(blcg_pmu),
};

/* NV_THERM (nvgpu hw_therm_gm20b.h): one GATE_CTRL per engine (the engine's index in the device info table). */
#define TGM_THERM_GATE_CTRL(engine) (0x00020200u + (uint32_t)(engine) * 4u)
#define TGM_THERM_FECS_IDLE_FILTER 0x00020288u
#define TGM_THERM_HUBMMU_IDLE_FILTER 0x0002028cu

/* An engine's GATE_CTRL with ELCG on (eng_clk auto, bits 1:0) behind nvgpu's idle filter (exponent 9 in 12:8,
 * mantissa 2 in 15:13); the other fields as they were. */
static inline uint32_t tgm_elcg_gate_ctrl(uint32_t gate_ctrl)
{
	gate_ctrl &= ~(0x3u | (0x1fu << 8) | (0x7u << 13));
	return gate_ctrl | 0x1u | (9u << 8) | (2u << 13);
}

/* ELCG off as nvgpu's ELCG_RUN leaves it (gm20b_therm_init_elcg_mode): eng_clk run, eng_pwr auto (bits 5:4). */
static inline uint32_t tgm_elcg_gate_ctrl_run(uint32_t gate_ctrl)
{
	return (gate_ctrl & ~(0x3u | (0x3u << 4))) | (0x1u << 4);
}

struct nvkm_device;
/* nvgpu's order around the golden context (gk20a_init_gr_prepare, then gk20a_init_gr_setup_hw): load writes every
 * list, the FECS/HUBMMU idle filters and THERM's slowdown (TegraGM20BTherm.h) once GR is up and before its golden
 * context is made, so the image captures the gating; enable loads them again once it exists and then turns ELCG on for
 * GR and the copy engines. After the SC7 wake or a rail-up (GR re-initialized from the same golden image) enable alone. */
void tgm_clkgate_load(struct nvkm_device *device);
void tgm_clkgate_enable(struct nvkm_device *device);
/* Debug kernels' A/B (gpu_bringup.c tgm_clkgate_switch): every list's "disable" value and ELCG run, as nvgpu turns gating off. */
void tgm_clkgate_disable(struct nvkm_device *device);
/* Once GR exists (M2): GR's reset in a channel's recovery (nouveau's nvkm_runl_rc) runs with gating off and gets every list
 * back with ELCG on after it, as nvgpu resets GR (gk20a_fifo_handle_mmu_fault_locked between nvgpu_cg_pg_disable and
 * nvgpu_cg_pg_enable; gk20a_init_gr_prepare keeps ELCG at run until GR is initialised again). With ELCG left on, GR's idle
 * clock gates under FECS's halt and GR's re-initialisation, and the reset drops GR's SLCG/BLCG lists. */
void tgm_clkgate_guard_reset(struct nvkm_device *device);

#endif /* TEGRA_GM20B_CLKGATE_H */
