// SPDX-License-Identifier: MIT
// TegraGM20BNvnGolden.h: its masked writes folded over the netlist's values of the three registers (gm20b sw_ctx.bin)
// give NVN's (c38-audit/perf1/learn/deko3d.md's golden-context table), and nvkm's gm20b golden context runs them
// between sw_ctx and the golden save, a release kernel taking no boot-arg that leaves one out.
#include <cstdint>
#include <gtest/gtest.h>

#include "bazel/testing/source_text.h"
#include "drivers/TegraGM20B/TegraGM20BNvnGolden.h"

namespace
{
using switch_ios::test::Block;
using switch_ios::test::CodeText;
using switch_ios::test::SourceText;

struct Register {
	uint32_t reg, netlist, nvn;
};

constexpr Register kTable[] = {
	{0x404468, 0x00ffffff, 0x07ffffff},
	{0x418800, 0x7006863a, 0x7006863b},
	{0x419a04, 0x00000005, 0x00000007},
};

TEST(GM20BNvnGolden, TheWritesOverTheNetlistGiveNvnsValues)
{
	ASSERT_EQ(sizeof kTable / sizeof kTable[0], TGM_NVN_GOLDEN_WRITES);
	for (uint32_t i = 0; i < TGM_NVN_GOLDEN_WRITES; i++) {
		const tgm_masked_write &w = tgm_nvn_golden[i];
		EXPECT_EQ(w.reg, kTable[i].reg);
		EXPECT_EQ(w.value & ~w.mask, 0u) << std::hex << w.reg;
		EXPECT_EQ(tgm_masked(kTable[i].netlist, &w), kTable[i].nvn) << std::hex << w.reg;
	}
}

TEST(GM20BNvnGolden, TheGoldenContextTakesThemAfterSwCtxAndADebugKernelAloneLeavesOneOut)
{
	const CodeText main = Block(SourceText("../nouveau/nvkm/engine/gr/ctxgm20b.c"),
				    "gm20b_grctx_generate_main(struct gf100_gr_chan *chan)");
	EXPECT_TRUE(main.before("gf100_gr_mmio(gr, gr->sw_ctx);", "switch_nvn_golden(device);"));
	EXPECT_TRUE(main.before("switch_nvn_golden(device);", "gf100_gr_wait_idle(gr);"));

	const CodeText skipped =
		Block(SourceText("../TegraGM20B.cpp"), "uint32_t switch_nvn_golden_skipped(void)");
	EXPECT_TRUE(skipped.before("#if DEVELOPMENT || DEBUG",
				   "PE_parse_boot_argn(\"switch-no-nvn-golden\""));
}
}  // namespace
