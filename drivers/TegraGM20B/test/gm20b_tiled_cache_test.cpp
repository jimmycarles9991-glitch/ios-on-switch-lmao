// SPDX-License-Identifier: MIT
// TegraGM20BTiledCache.h against deko3d's Queue::setup3DEngine (source/maxwell/gpu_3d_base.cpp, MIT): its masked
// WriteHardwareReg calls, transcribed below, folded over the zero the netlist's sw_ctx leaves in every one of these
// registers (gm20b sw_ctx.bin lists 0x418e40..0x418e6c, each 0), give exactly the words the golden context gets.
#include <cstdint>
#include <map>
#include <gtest/gtest.h>
#include "drivers/TegraGM20B/TegraGM20BTiledCache.h"

namespace
{
struct MaskedWrite {
	uint32_t reg, value, mask;
};

constexpr MaskedWrite kDeko3d[] = {
	{0x418e40, 0x00000007, 0x0000000f}, {0x418e58, 0x00000842, 0x0000ffff},
	{0x418e40, 0x00000070, 0x000000f0}, {0x418e58, 0x04f10000, 0xffff0000},
	{0x418e40, 0x00000700, 0x00000f00}, {0x418e5c, 0x00000053, 0x0000ffff},
	{0x418e40, 0x00007000, 0x0000f000}, {0x418e5c, 0x00e90000, 0xffff0000},
	{0x418e40, 0x00070000, 0x000f0000}, {0x418e60, 0x000000ea, 0x0000ffff},
	{0x418e40, 0x00700000, 0x00f00000}, {0x418e60, 0x00eb0000, 0xffff0000},
	{0x418e40, 0x07000000, 0x0f000000}, {0x418e64, 0x00000208, 0x0000ffff},
	{0x418e40, 0x70000000, 0xf0000000}, {0x418e64, 0x02090000, 0xffff0000},
	{0x418e44, 0x00000007, 0x0000000f}, {0x418e68, 0x0000020a, 0x0000ffff},
	{0x418e44, 0x00000070, 0x000000f0}, {0x418e68, 0x020b0000, 0xffff0000},
	{0x418e44, 0x00000700, 0x00000f00}, {0x418e6c, 0x00000644, 0x0000ffff},
};

std::map<uint32_t, uint32_t> deko3dOverZero()
{
	std::map<uint32_t, uint32_t> regs;
	for (uint32_t r = TGM_TC_BUNDLE_CTRL; r < TGM_TC_BUNDLE_CTRL + 4 * TGM_TC_CTRL_WORDS;
	     r += 4)
		regs[r] = 0;
	for (uint32_t r = TGM_TC_BUNDLE_ADDR; r < TGM_TC_BUNDLE_ADDR + 4 * TGM_TC_ADDR_WORDS;
	     r += 4)
		regs[r] = 0;
	for (const MaskedWrite &w : kDeko3d) {
		EXPECT_TRUE(regs.count(w.reg)) << std::hex << w.reg;
		regs[w.reg] = (regs[w.reg] & ~w.mask) | (w.value & w.mask);
	}
	return regs;
}

TEST(TiledCache, GoldenWordsAreDeko3dsWritesOverZero)
{
	uint32_t ctrl[TGM_TC_CTRL_WORDS] = {~0u, ~0u};
	uint32_t addr[TGM_TC_ADDR_WORDS] = {~0u, ~0u, ~0u, ~0u, ~0u, ~0u};
	tgm_tiled_cache_table(ctrl, addr);

	std::map<uint32_t, uint32_t> ours;
	for (uint32_t i = 0; i < TGM_TC_CTRL_WORDS; i++)
		ours[TGM_TC_BUNDLE_CTRL + 4 * i] = ctrl[i];
	for (uint32_t i = 0; i < TGM_TC_ADDR_WORDS; i++)
		ours[TGM_TC_BUNDLE_ADDR + 4 * i] = addr[i];
	EXPECT_EQ(ours, deko3dOverZero());
	EXPECT_EQ(ctrl[0], 0x77777777u);
	EXPECT_EQ(ctrl[1], 0x00000777u);
}
}  // namespace
