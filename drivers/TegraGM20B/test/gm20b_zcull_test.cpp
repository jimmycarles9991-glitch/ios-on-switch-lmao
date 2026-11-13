// SPDX-License-Identifier: MIT
// TegraGM20BZcull.h against nvgpu's zcull report (L4T r32.7.6 gk20a/gr_gk20a.c gr_gk20a_init_zcull and
// gr_gk20a_get_zcull_info), transcribed below as the reference with GM20B's hw_gr_gm20b.h constants, for the X1's GR
// (one GPC, two TPCs) at every plausible zcull bank count and RAM size; and the layout's record is the one NVK and Mesa
// read (drm_nouveau_get_zcull_info's order).
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BZcull.h"

namespace
{
struct Reference {
	uint32_t width_align, height_align, pixel_squares, aliquots, region_bytes, region_header,
		subregion_header, subregion_width_align, subregion_height_align, subregions;
};

Reference nvgpu(uint32_t gpc_count, uint32_t tpc_count, uint32_t gpc0_tpcs, uint32_t zcb_count,
		uint32_t aliquots)
{
	const uint32_t num_gpcs = 1; /* GPU_LIT_NUM_GPCS */
	Reference r = {};
	r.width_align = tpc_count * 16;
	r.height_align = 32;
	r.pixel_squares = zcb_count * 16 * 16 * tpc_count / (gpc_count * gpc0_tpcs);
	r.aliquots = aliquots;
	r.region_bytes = gpc_count * 0x20;
	r.region_header = num_gpcs * 0x20;
	r.subregion_header = num_gpcs * 0xc0;
	r.subregion_width_align = tpc_count * 0x10;
	r.subregion_height_align = 0x40;
	r.subregions = 0x10;
	return r;
}

TEST(Zcull, LayoutIsNvgpus)
{
	for (uint32_t zcbs = 1; zcbs <= 4; zcbs++)
		for (uint32_t aliquots : {0x100u, 0x400u, 0x800u, 0x1400u, 0xffffu}) {
			TegraGM20BAbiZcullInfo z = {};
			ASSERT_TRUE(tgm_zcull_layout(1, 2, 2, zcbs << 16 | 2,
						     aliquots | 0xabcd0000u, 0x1500, &z));
			const Reference r = nvgpu(1, 2, 2, zcbs, aliquots);
			EXPECT_EQ(z.width_align_pixels, r.width_align);
			EXPECT_EQ(z.height_align_pixels, r.height_align);
			EXPECT_EQ(z.pixel_squares_by_aliquots, r.pixel_squares);
			EXPECT_EQ(z.aliquot_total, r.aliquots);
			EXPECT_EQ(z.region_byte_multiplier, r.region_bytes);
			EXPECT_EQ(z.region_header_size, r.region_header);
			EXPECT_EQ(z.subregion_header_size, r.subregion_header);
			EXPECT_EQ(z.subregion_width_align_pixels, r.subregion_width_align);
			EXPECT_EQ(z.subregion_height_align_pixels, r.subregion_height_align);
			EXPECT_EQ(z.subregion_count, r.subregions);
			EXPECT_EQ(z.ctxsw_size, 0x1500u);
			EXPECT_EQ(z.ctxsw_align, 0x1000u);
		}
}

TEST(Zcull, TheX1sRegisters)
{
	TegraGM20BAbiZcullInfo z = {};
	ASSERT_TRUE(tgm_zcull_layout(1, 2, 2, 0x00040002, 0x01ff0800, 0x1500, &z));
	EXPECT_EQ(z.aliquot_total, 2048u);
	EXPECT_EQ(z.pixel_squares_by_aliquots, 1024u);
	EXPECT_EQ(z.width_align_pixels, 32u);
	EXPECT_EQ(z.subregion_width_align_pixels, 32u);
	EXPECT_EQ(z.subregion_height_align_pixels, 64u);
}

TEST(Zcull, NoRamOrNoImageIsNoZcull)
{
	EXPECT_FALSE(tgm_zcull_layout(1, 2, 2, 0xbadf1100, 0x01ff0800, 0x1500, nullptr));
	EXPECT_FALSE(tgm_zcull_layout(1, 2, 2, 0x00040002, 0xbadf1100, 0x1500, nullptr));
	TegraGM20BAbiZcullInfo z = {};
	EXPECT_FALSE(tgm_zcull_layout(1, 2, 2, 1 << 16, 0, 0x1500,
				      &z)); /* the emulator's GR: no RAM size */
	EXPECT_FALSE(tgm_zcull_layout(1, 2, 2, 0, 0x400, 0x1500, &z));  /* no zcull bank */
	EXPECT_FALSE(tgm_zcull_layout(1, 2, 2, 1 << 16, 0x400, 0, &z)); /* FECS reported no image */
	EXPECT_FALSE(tgm_zcull_layout(0, 0, 0, 1 << 16, 0x400, 0x1500, &z));
}

TEST(Zcull, RecordIsDrmNouveauGetZcullInfos)
{
	EXPECT_EQ(offsetof(TegraGM20BAbiZcullInfo, aliquot_total), 12u);
	EXPECT_EQ(offsetof(TegraGM20BAbiZcullInfo, region_byte_multiplier), 16u);
	EXPECT_EQ(offsetof(TegraGM20BAbiZcullInfo, subregion_count), 28u);
	EXPECT_EQ(offsetof(TegraGM20BAbiZcullInfo, subregion_width_align_pixels), 32u);
	EXPECT_EQ(offsetof(TegraGM20BAbiZcullInfo, ctxsw_size), 40u);
	EXPECT_EQ(sizeof(TegraGM20BAbiZcullInfo), 48u);
}
}  // namespace
