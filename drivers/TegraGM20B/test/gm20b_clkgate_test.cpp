// TegraGM20BClkgate.h against nvgpu's own gm20b_gating_reglist.c (kept verbatim in nvidia/): the registers and values
// the driver writes are nvgpu's "prod" lists, in nvgpu's load order, and gpu_bringup.c and gpu_power.c apply them at bring-up and after SC7.
// GR's reset in a recovery runs with gating off and gets the lists back after it, as nvgpu resets GR.
#include "drivers/TegraGM20B/TegraGM20BClkgate.h"
#include "drivers/TegraGM20B/TegraGM20BTherm.h"

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
	const std::string gpu = Text(GPU_BRINGUP_C) + Text(GPU_POWER_C);
	const size_t load = gpu.find("\t\t\tret = m2(line, len);\n\t\tif (ret)\n\t\t\treturn ret;\n"
				     "\t\ttgm_clkgate_load(tgm_gpu.device);\n");
	const size_t golden = gpu.find("\t\tret = m3(line, len);\n", load);
	const size_t enable = gpu.find("\t\ttgm_clkgate_enable(tgm_gpu.device);\n", golden);
	EXPECT_NE(load, std::string::npos);
	EXPECT_NE(golden, std::string::npos);
	EXPECT_NE(enable, std::string::npos);
	EXPECT_NE(
		gpu.find(
			"\twake_fence();\n\ttgm_clkgate_enable(tgm_gpu.device);\n\twake_open();\n"),
		std::string::npos);
}

// The C definition starting with `head`, through `close` at column 0 (a function's brace, a table's "};")
std::string Body(const std::string &source, const std::string &head, const char *close = "\n}\n")
{
	const size_t at = source.find(head);
	if (at == std::string::npos)
		return "";
	const size_t end = source.find(close, at);
	return source.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

TEST(Gm20bClkgate, GrResetRunsWithGatingOffAndGetsTheListsBack)
{
	const std::string reset =
		Body(Text(CLKGATE_C), "static int gated_reset(struct nvkm_engine *engine)");
	const size_t off = reset.find("tgm_clkgate_disable(device);");
	const size_t nouveau = reset.find("ret = gr_reset(engine);");
	const size_t on = reset.find("tgm_clkgate_enable(device);");
	ASSERT_NE(off, std::string::npos);
	ASSERT_NE(nouveau, std::string::npos);
	ASSERT_NE(on, std::string::npos);
	EXPECT_LT(off, nouveau);
	EXPECT_LT(nouveau, on);

	const std::string guard =
		Body(Text(CLKGATE_C), "void tgm_clkgate_guard_reset(struct nvkm_device *device)");
	EXPECT_NE(guard.find("nvkm_device_engine(device, NVKM_ENGINE_GR, 0)"), std::string::npos);
	EXPECT_NE(guard.find("gr_func.reset = gated_reset;"), std::string::npos);
	EXPECT_NE(guard.find("gr->func == &gr_func"), std::string::npos) << "installed once";
}

TEST(Gm20bClkgate, TheGuardIsInPlaceBeforeAnyChannel)
{
	const std::string gpu = Text(GPU_BRINGUP_C);
	const size_t load = gpu.find("\t\ttgm_clkgate_load(tgm_gpu.device);\n\t\ttgm_clkgate_guard_"
				     "reset(tgm_gpu.device);\n");
	const size_t golden = gpu.find("\t\tret = m3(line, len);\n");
	ASSERT_NE(load, std::string::npos);
	ASSERT_NE(golden, std::string::npos);
	EXPECT_LT(load, golden);
}

// The guard replaces GR's engine table by a copy: a whole one only while nouveau's table names its classes through
// fifo.sclass and leaves the trailing sclass list (a flexible array the copy drops) empty.
TEST(Gm20bClkgate, NouveausGrEngineTableCopiesWhole)
{
	const std::string table =
		Body(Text(GR_BASE_C), "static const struct nvkm_engine_func\nnvkm_gr = {", "\n};");
	ASSERT_FALSE(table.empty());
	EXPECT_NE(table.find(".reset = nvkm_gr_reset,"), std::string::npos);
	EXPECT_NE(table.find(".fifo.sclass = nvkm_gr_oclass_get,"), std::string::npos);
	EXPECT_EQ(table.find("\t.sclass"), std::string::npos);
}

std::map<uint32_t, uint32_t> ArmedTherm(std::map<uint32_t, uint32_t> regs)
{
	for (const auto &w : tgm_therm_setup)
		regs[w.addr] = tgm_therm_value(&w, regs[w.addr]);
	return regs;
}

// L4T's THERM registers on this unit (the 11 Nov capture's regs_pg_idle.txt, nvgpu R32.3.1), from our reset values
// (CONFIG2 0x00001300, every other one 0, in our dumps).
TEST(Gm20bTherm, ArmedAsL4tReadsThem)
{
	const std::map<uint32_t, uint32_t> l4t = {
		{0x00020798u, 0x00000007u}, {0x00020700u, 0x00000200u}, {0x00020704u, 0x00000600u},
		{0x00020708u, 0x00000e00u}, {0x000202c8u, 0x0e386081u}, {0x000202ccu, 0x0e38e38eu},
		{0x000203c0u, 0x00010000u}, {0x00020130u, 0x81001300u}, {0x000202c4u, 0x00000020u},
		{0x000202c0u, 0x00000001u},
	};
	EXPECT_EQ(ArmedTherm({{0x00020130u, 0x00001300u}}), l4t);
	EXPECT_EQ(ArmedTherm(ArmedTherm({{0x00020130u, 0x00001300u}})), l4t)
		<< "a second power-up arms the same";
}

TEST(Gm20bTherm, NvgpusWriteOrderAndItsReadModifyWrites)
{
	const uint32_t order[] = {0x00020798u, 0x00020700u, 0x00020704u, 0x00020708u, 0x000202c8u,
				  0x000202ccu, 0x000203c0u, 0x00020130u, 0x000202c4u, 0x000202c0u};
	ASSERT_EQ(sizeof(order) / sizeof(order[0]),
		  sizeof(tgm_therm_setup) / sizeof(tgm_therm_setup[0]));
	for (unsigned i = 0; i < sizeof(order) / sizeof(order[0]); ++i)
		EXPECT_EQ(tgm_therm_setup[i].addr, order[i]) << i;
	const auto armed = ArmedTherm({{0x000203c0u, 0x12340000u}, {0x000202c0u, 0x00000100u}});
	EXPECT_EQ(armed.at(0x000203c0u), 0x12350000u) << "CLK_TIMING keeps its other fields";
	EXPECT_EQ(armed.at(0x000202c0u), 0x00000101u) << "GRAD_STEPPING0 keeps its other fields";
}

TEST(Gm20bTherm, ArmedWheneverTheListsLoad)
{
	const std::string load =
		Body(Text(CLKGATE_C), "void tgm_clkgate_load(struct nvkm_device *device)");
	EXPECT_NE(load.find("tgm_therm_arm(device);"), std::string::npos)
		<< "M2, and every tgm_clkgate_enable: M3, the SC7 wake, a rail-up, GR's reset";
	EXPECT_NE(Body(Text(CLKGATE_C), "void tgm_clkgate_enable(struct nvkm_device *device)")
			  .find("tgm_clkgate_load(device);"),
		  std::string::npos);
}

}  // namespace
