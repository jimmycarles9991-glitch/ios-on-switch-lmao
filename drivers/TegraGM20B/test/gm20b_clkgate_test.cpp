// TegraGM20BClkgate.h against nvgpu's own gm20b_gating_reglist.c (kept verbatim in nvidia/): the registers and values
// the driver writes are nvgpu's "prod" lists, in nvgpu's load order, and gpu.c applies them at bring-up and after SC7.
#include "drivers/TegraGM20B/TegraGM20BClkgate.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

namespace
{

using Entries = std::vector<std::tuple<uint32_t, uint32_t, uint32_t>>;

std::string Text(const char *path)
{
	std::ifstream file(path, std::ios::binary);
	std::stringstream text;
	text << file.rdbuf();
	return text.str();
}

// Each `static const struct gating_desc gm20b_<name>[]` list's live (not commented out) entries.
std::map<std::string, Entries> Reference()
{
	const std::string source = Text(REGLIST);
	std::map<std::string, Entries> lists;
	const std::regex list(
		R"(static const struct gating_desc gm20b_(\w+)\[\] = \{([^}]*(?:\}[^;][^}]*)*)\};)");
	const std::regex entry(
		R"(^\s*\{\.addr = (0x[0-9a-fA-F]+)U, \.prod = (0x[0-9a-fA-F]+)U, \.disable = (0x[0-9a-fA-F]+)U)");
	for (auto it = std::sregex_iterator(source.begin(), source.end(), list);
	     it != std::sregex_iterator(); ++it) {
		Entries entries;
		std::istringstream body((*it)[2].str());
		for (std::string line; std::getline(body, line);) {
			std::smatch m;
			if (std::regex_search(line, m, entry))
				entries.emplace_back(std::stoul(m[1].str(), nullptr, 16),
						     std::stoul(m[2].str(), nullptr, 16),
						     std::stoul(m[3].str(), nullptr, 16));
		}
		lists[(*it)[1].str()] = entries;
	}
	return lists;
}

TEST(Gm20bClkgate, TheWritesAreNvgpusProdListsInItsLoadOrder)
{
	const auto lists = Reference();
	ASSERT_GE(lists.size(), 20u);
	const char *order[] = {"slcg_priring", "slcg_fb",   "slcg_ltc", "blcg_fb",      "blcg_ltc",
			       "slcg_fifo",    "blcg_fifo", "slcg_bus", "slcg_chiplet", "slcg_gr",
			       "slcg_perf",    "slcg_xbar", "blcg_bus", "blcg_gr",      "blcg_xbar",
			       "slcg_ce2",     "slcg_pmu",  "blcg_pmu"};
	ASSERT_EQ(sizeof(order) / sizeof(order[0]), sizeof(tgm_cg_lists) / sizeof(tgm_cg_lists[0]));
	Entries expected, written;
	for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); ++i) {
		ASSERT_EQ(lists.count(order[i]), 1u) << order[i];
		EXPECT_EQ(std::string(tgm_cg_lists[i].name), order[i]);
		const Entries &ref = lists.at(order[i]);
		expected.insert(expected.end(), ref.begin(), ref.end());
		for (unsigned j = 0; j < tgm_cg_lists[i].count; ++j)
			written.emplace_back(tgm_cg_lists[i].regs[j].addr,
					     tgm_cg_lists[i].regs[j].prod,
					     tgm_cg_lists[i].regs[j].disable);
		EXPECT_EQ(tgm_cg_lists[i].count, ref.size()) << order[i];
	}
	EXPECT_EQ(written, expected);
	EXPECT_EQ(written.size(), 154u);
}

TEST(Gm20bClkgate, NvgpusDisabledEntryAndUnloadedListsStayOut)
{
	for (const auto &list : tgm_cg_lists)
		for (unsigned j = 0; j < list.count; ++j) {
			EXPECT_NE(list.regs[j].addr, 0x00100d1cu)
				<< "nvgpu comments it out: a priv error";
			EXPECT_NE(list.regs[j].addr, 0x000206b8u)
				<< "slcg_therm is not loaded on GM20B";
			EXPECT_NE(list.regs[j].addr, 0x00000a70u)
				<< "blcg_pwr_csb is not loaded on GM20B";
		}
}

TEST(Gm20bClkgate, ElcgRunAsNvgpuTurnsItOff)
{
	EXPECT_EQ(tgm_elcg_gate_ctrl_run(0x00004901u), 0x00004910u);
	EXPECT_EQ(tgm_elcg_gate_ctrl_run(0xffffffffu), 0xffffffdcu);
}

TEST(Gm20bClkgate, ElcgAutoBehindNvgpusIdleFilter)
{
	EXPECT_EQ(tgm_elcg_gate_ctrl(0), 0x00004901u);
	EXPECT_EQ(tgm_elcg_gate_ctrl(0xffffffffu), 0xffff49fdu);
	EXPECT_EQ(tgm_elcg_gate_ctrl(0x00000006u), 0x00004905u)
		<< "the block clock mode (bits 3:2) is left alone";
	EXPECT_EQ(TGM_THERM_GATE_CTRL(1), 0x00020204u);
}

TEST(Gm20bClkgate, GpuFollowsNvgpusOrderAroundTheGoldenContext)
{
	const std::string gpu = Text(GPU_C);
	const size_t load = gpu.find("\t\t\tret = m2(line, len);\n\t\tif (ret)\n\t\t\treturn ret;\n"
				     "\t\ttgm_clkgate_load(gpu.device);\n");
	const size_t golden = gpu.find("\t\tret = m3(line, len);\n", load);
	const size_t enable = gpu.find("\t\ttgm_clkgate_enable(gpu.device);\n", golden);
	EXPECT_NE(load, std::string::npos);
	EXPECT_NE(golden, std::string::npos);
	EXPECT_NE(enable, std::string::npos);
	EXPECT_NE(gpu.find("\twake_fence();\n\ttgm_clkgate_enable(gpu.device);\n\twake_open();\n"),
		  std::string::npos);
}

}  // namespace
