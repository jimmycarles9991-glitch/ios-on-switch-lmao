/*
 * GM20B L2: the compbit backing store in system memory, after NVIDIA's nvgpu (common/ltc/ltc_gm20b.c).
 *
 * Copyright (c) 2014-2018 NVIDIA CORPORATION.  All rights reserved.
 * Copyright 2015 Red Hat Inc.
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
#include "priv.h"

#include <subdev/fb.h>
#include <subdev/timer.h>

#include "TegraGM20BComp.h"

#define CBC_CTRL1_INVALIDATE 0x00000002
#define CBC_CTRL1_CLEAR 0x00000004

/* CBC operations report completion only through each slice's CBC_CTRL1; none raises an interrupt. */
static void
gm20b_ltc_cbc_done(struct nvkm_ltc *ltc, u32 op, const char *what)
{
	struct nvkm_device *device = ltc->subdev.device;
	int c, s;

	for (c = 0; c < ltc->ltc_nr; c++) {
		for (s = 0; s < ltc->lts_nr; s++) {
			const u32 addr = 0x14046c + (c * 0x2000) + (s * 0x200);

			if (nvkm_wait_msec(device, 2000, addr, op, 0) < 0)
				nvxnu_panic("tegra-gm20b: CBC %s still busy after 2 s (%06x %08x)", what,
					    addr, nvkm_rd32(device, addr));
		}
	}
}

static void
gm20b_ltc_cbc_wait(struct nvkm_ltc *ltc)
{
	gm20b_ltc_cbc_done(ltc, CBC_CTRL1_CLEAR, "clear");
}

static int
gm20b_ltc_oneinit(struct nvkm_ltc *ltc)
{
	struct nvkm_device *device = ltc->subdev.device;
	struct nvkm_fb *fb = device->fb;
	const u32 param = nvkm_rd32(device, 0x17e280);
	const u32 line_bits = (nvkm_rd32(device, 0x100c80) & 0x00001000) ? 16 : 17;
	struct tgm_cbc cbc = {};
	u64 pa = 0;

	ltc->ltc_nr = nvkm_rd32(device, 0x12006c);
	ltc->lts_nr = tgm_cbc_slices(param);
	ltc->num_tags = 0;

	/* Ordinary memory as nvgpu's (nvgpu_ltc_alloc_cbc), not the GPU pool, which every process's coherent memory
	 * shares. DRAM keeps it through system sleep and only the L2 touches it, so nothing saves or restores it. */
	if (tgm_cbc_size(param, ltc->ltc_nr, nvxnu_ram_bytes() >> 20, line_bits, &cbc) &&
	    !nvxnu_contig_alloc(cbc.bytes, &pa) && tgm_cbc_base(pa, ltc->ltc_nr, &ltc->tag_base))
		ltc->num_tags = cbc.lines;

	nvxnu_log("tegra-gm20b: CBC: %u comptag lines, %llu KiB at %llx (base %x), %u LTC x %u slices, "
		  "%u per %u-byte line (param %08x)\n",
		  ltc->num_tags, ltc->num_tags ? cbc.bytes >> 10 : 0, pa, ltc->tag_base, ltc->ltc_nr,
		  ltc->lts_nr, tgm_cbc_comptags_per_cacheline(param), tgm_cbc_cacheline_bytes(param),
		  param);

	nvkm_mm_fini(&fb->tags.mm);
	return nvkm_mm_init(&fb->tags.mm, 0, 1, ltc->num_tags ? ltc->num_tags - 1 : 0, 1);
}

/* nvgpu's gm20b_ltc_init_fs_state, fb_gm20b_init_fs_state (FBHUB's active LTCs) and gm20b_ltc_init_cbc. */
static void
gm20b_ltc_init(struct nvkm_ltc *ltc)
{
	struct nvkm_device *device = ltc->subdev.device;

	nvkm_wr32(device, 0x17e27c, ltc->ltc_nr);
	nvkm_wr32(device, 0x17e000, ltc->ltc_nr);
	nvkm_wr32(device, 0x100800, ltc->ltc_nr);
	nvkm_wr32(device, 0x17e318, nvkm_rd32(device, 0x140518) | 0x00008000);
	/* EVICTED_CB, ILLEGAL_COMPSTAT and ILLEGAL_COMPSTAT_ACCESS stay masked as nvgpu has them: every LTC report is
	 * fatal here (nvxnu_errors_fatal). */
	nvkm_wr32(device, 0x17e20c, nvkm_rd32(device, 0x17e20c) & ~0x40300000);

	nvkm_wr32(device, 0x17e278, ltc->tag_base);
	if (ltc->num_tags) {
		nvkm_mask(device, 0x17e26c, 0, CBC_CTRL1_INVALIDATE);
		gm20b_ltc_cbc_done(ltc, CBC_CTRL1_INVALIDATE, "invalidate");
	}
}

static const struct nvkm_ltc_func
gm20b_ltc = {
	.oneinit = gm20b_ltc_oneinit,
	.init = gm20b_ltc_init,
	.intr = gm107_ltc_intr,
	.cbc_clear = gm107_ltc_cbc_clear,
	.cbc_wait = gm20b_ltc_cbc_wait,
	.zbc_color = 16,
	.zbc_depth = 16,
	.zbc_clear_color = gm107_ltc_zbc_clear_color,
	.zbc_clear_depth = gm107_ltc_zbc_clear_depth,
	.invalidate = gf100_ltc_invalidate,
	.flush = gf100_ltc_flush,
};

int
gm20b_ltc_new(struct nvkm_device *device, enum nvkm_subdev_type type, int inst,
	      struct nvkm_ltc **pltc)
{
	return nvkm_ltc_new_(&gm20b_ltc, device, type, inst, pltc);
}
