/*
 * GM20B's own clock slowdown on thermal and over-current events, armed as nvgpu arms it at every power-on
 * (gm20b_init_therm_setup_hw, common/therm/therm_gm20b.c, through nvgpu_init_therm_support in gk20a_finalize_poweron).
 * THERM slows the GPC clock only while one of its EXT_THERM inputs is asserted: _0 by 2, _1 by 4, _2 by 8, stepping
 * down through the gradual table. The inputs come from outside the GPU (on Tegra, SOC_THERM's GPU throttle; which of
 * its levels drives which input is not documented here), and no input asserted leaves the clock alone. Each write is
 * nvgpu's, in its order, with its fields from hw_therm_gm20b.h. CLK_SLOWDOWN_0's idle factor (0x020160 bits 21:16, the
 * GPC clock divided while GR idles: 4 is FPDIV_BY3 in gpmuif_pg.h) is not part of it: nvgpu's and nouveau's GM20B clock
 * init clear it (gm20b_init_clk_setup_hw, gm20b_clk_init) and nvgpu clears it again around every clock count. Nothing in
 * nvgpu's GM20B code writes L4T's 4, so it comes from its PMU firmware (inferred: ELPG runs there):
 *
 * Copyright (c) 2015-2018, NVIDIA CORPORATION.  All rights reserved.
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
#ifndef TEGRA_GM20B_THERM_H
#define TEGRA_GM20B_THERM_H

#include <stdint.h>

/* A whole-register write (keep 0) or nvgpu's read, OR and write back (keep ~0: every other bit as it was). */
struct tgm_therm_write {
	uint32_t addr, keep, value;
};

#define TGM_THERM_SLOW_BY_1P5 0x1u
#define TGM_THERM_SLOW_BY_2 0x2u
#define TGM_THERM_SLOW_BY_4 0x6u
#define TGM_THERM_SLOW_BY_8 0xeu
#define TGM_THERM_EVT_SLOW(factor) ((factor) << 8)
#define TGM_THERM_GRAD_TABLE(f0, f1, f2, f3, f4) \
	((f0) | ((f1) << 6) | ((f2) << 12) | ((f3) << 18) | ((f4) << 24))

static const struct tgm_therm_write tgm_therm_setup[] = {
	{0x00020798u, 0u, 0x7u}, /* USE_A: EXT_THERM_0, _1, _2 */
	{0x00020700u, 0u, TGM_THERM_EVT_SLOW(TGM_THERM_SLOW_BY_2)},
	{0x00020704u, 0u, TGM_THERM_EVT_SLOW(TGM_THERM_SLOW_BY_4)},
	{0x00020708u, 0u, TGM_THERM_EVT_SLOW(TGM_THERM_SLOW_BY_8)},
	{0x000202c8u, 0u,
	 TGM_THERM_GRAD_TABLE(TGM_THERM_SLOW_BY_1P5, TGM_THERM_SLOW_BY_2, TGM_THERM_SLOW_BY_4,
			      TGM_THERM_SLOW_BY_8, TGM_THERM_SLOW_BY_8)},
	{0x000202ccu, 0u,
	 TGM_THERM_GRAD_TABLE(TGM_THERM_SLOW_BY_8, TGM_THERM_SLOW_BY_8, TGM_THERM_SLOW_BY_8,
			      TGM_THERM_SLOW_BY_8, TGM_THERM_SLOW_BY_8)},
	{0x000203c0u, ~0u, 1u << 16}, /* CLK_TIMING(0): GRAD_SLOWDOWN enabled */
	{0x00020130u, ~0u,
	 (1u << 31) | (1u << 24)}, /* CONFIG2: GRAD_ENABLE, SLOWDOWN_FACTOR_EXTENDED */
	{0x000202c4u, 0u, 32u},    /* GRAD_STEPPING1: PDIV_DURATION */
	{0x000202c0u, ~0u, 1u},    /* GRAD_STEPPING0: FEATURE enabled */
};

static inline uint32_t tgm_therm_value(const struct tgm_therm_write *w, uint32_t old)
{
	return (old & w->keep) | w->value;
}

struct nvkm_device;
void tgm_therm_arm(struct nvkm_device *device);

#endif /* TEGRA_GM20B_THERM_H */
