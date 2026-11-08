/* SPDX-License-Identifier: MIT */
/*
 * Maxwell's tiled cache (the binning rasterizer: the tile coalescer in the GPCs' screen work distribution buffers
 * primitives and draws them one screen tile at a time, so a tile's blending stays in the L2), the kernel's part.
 *
 * NVN on the Tegra X1 gives the coalescer a table of bundles in gr_pri_gpcs_swdx_tc_bundle_ctrl (a nibble per entry) and
 * tc_bundle_addr (16 bits per entry); deko3d's Queue::setup3DEngine (gpu_3d_base.cpp, MIT) writes it entry by entry
 * through its WriteHardwareReg macro. These are context-switched registers that the netlist's sw_ctx sets to zero, so the
 * words below, written between sw_ctx and the golden save, equal deko3d's masked writes and every GR context starts
 * with them. test/gm20b_tiled_cache_test.cpp folds deko3d's writes over zero and compares.
 *
 * The cache's own state (tile size, configuration, the enable) are 3D methods: clients send them at context init when
 * GetInfo has TegraGM20BInfoTiledCache. Boot-arg switch-no-tiled-cache leaves the table out and the bit clear.
 */
#ifndef TEGRA_GM20B_TILED_CACHE_H
#define TEGRA_GM20B_TILED_CACHE_H
#include <stdint.h>

#define TGM_TC_BUNDLE_CTRL 0x418e40u
#define TGM_TC_BUNDLE_ADDR 0x418e58u
#define TGM_TC_CTRL_WORDS 2u
#define TGM_TC_ADDR_WORDS 6u

static inline void tgm_tiled_cache_table(uint32_t ctrl[TGM_TC_CTRL_WORDS],
					 uint32_t addr[TGM_TC_ADDR_WORDS])
{
	static const uint16_t bundles[] = {0x0842, 0x04f1, 0x0053, 0x00e9, 0x00ea, 0x00eb,
					   0x0208, 0x0209, 0x020a, 0x020b, 0x0644};
	uint32_t i = 0;

	for (i = 0; i < TGM_TC_CTRL_WORDS; i++)
		ctrl[i] = 0;
	for (i = 0; i < TGM_TC_ADDR_WORDS; i++)
		addr[i] = 0;
	for (i = 0; i < sizeof(bundles) / sizeof(bundles[0]); i++) {
		ctrl[i / 8] |= 7u << (i % 8 * 4);
		addr[i / 2] |= (uint32_t)bundles[i] << (i % 2 * 16);
	}
}

#endif
