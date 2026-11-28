/* SPDX-License-Identifier: MIT */
/*
 * Three GR context registers NVN on the Tegra X1 sets otherwise than NVIDIA's netlist (gm20b sw_ctx.bin): deko3d's
 * Queue::setup3DEngine (source/maxwell/gpu_3d_base.cpp, MIT) writes them through its WriteHardwareReg macro, a masked
 * write. What the bits do is undocumented. Like the tiled cache's table (TegraGM20BTiledCache.h), the kernel writes them
 * between sw_ctx and the golden save, so every GR context starts with them.
 *
 * The masks are the narrowest that give NVN's values from the netlist's, as c38-audit/perf1/learn/deko3d.md's
 * golden-context table records both; test/gm20b_nvn_golden_test.cpp folds them over the netlist's values.
 *
 * A debug kernel's boot-arg switch-no-nvn-golden=<bits> leaves out the writes its bits name (bit i: entry i), one boot
 * per arm of the A/B.
 */
#ifndef TEGRA_GM20B_NVN_GOLDEN_H
#define TEGRA_GM20B_NVN_GOLDEN_H
#include <stdint.h>

struct tgm_masked_write {
	uint32_t reg, mask, value;
};

#define TGM_NVN_GOLDEN_WRITES 3u

static const struct tgm_masked_write tgm_nvn_golden[TGM_NVN_GOLDEN_WRITES] = {
	{0x404468u, 0x07000000u, 0x07000000u},
	{0x418800u, 0x01800001u, 0x00000001u},
	{0x419a04u, 0x00000002u, 0x00000002u},
};

static inline uint32_t tgm_masked(uint32_t old, const struct tgm_masked_write *w)
{
	return (old & ~w->mask) | (w->value & w->mask);
}

#endif
