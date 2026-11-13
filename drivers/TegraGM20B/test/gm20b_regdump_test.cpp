// TegraGM20BRegDump.h against the capture's register list (regs_gm20b.txt, the same file L4T's capture.sh reads):
// the table is that list's readable offsets in its order, under its id, and the dump's lines are the capture's
// "0xOFFSET 0xVALUE" form, one value per offset.
#include <cstdio>
#include <cstring>

#include "drivers/TegraGM20B/TegraGM20BRegDump.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace
{

struct List {
	std::string id;
	std::vector<uint32_t> read;
	std::set<uint32_t> skipped;
};

List Load()
{
	std::ifstream file(REGS_TXT);
	List list;
	for (std::string line; std::getline(file, line);) {
		std::smatch m;
		if (std::regex_search(
			    line, m,
			    std::regex(R"(^# GM20B register list v1 list=([0-9a-f]{16}))"))) {
			list.id = m[1].str();
			continue;
		}
		if (line.empty() || line[0] == '#')
			continue;
		const uint32_t offset = std::stoul(line.substr(0, line.find(' ')), nullptr, 16);
		if (line.find(" skip=") != std::string::npos)
			list.skipped.insert(offset);
		else
			list.read.push_back(offset);
	}
	return list;
}

std::vector<uint32_t> Values()
{
	std::vector<uint32_t> values;
	for (size_t i = 0; i < TGM_REGDUMP_COUNT; i++)
		values.push_back(tgm_regdump_offsets[i] ^ 0xa5a5a5a5u);
	return values;
}

TEST(Gm20bRegdump, TheTableIsTheCapturesListInItsOrder)
{
	const List list = Load();
	char id[17];
	snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(TGM_REGDUMP_LIST_ID));
	EXPECT_EQ(list.id, id);
	ASSERT_EQ(list.read.size(), TGM_REGDUMP_COUNT);
	for (size_t i = 0; i < TGM_REGDUMP_COUNT; i++) {
		EXPECT_EQ(list.read[i], tgm_regdump_offsets[i]) << i;
		EXPECT_EQ(list.skipped.count(tgm_regdump_offsets[i]), 0u) << i;
	}
}

TEST(Gm20bRegdump, EachLineIsOneRegisterInListOrder)
{
	std::vector<char> buf(TGM_REGDUMP_BYTES);
	const std::vector<uint32_t> values = Values();
	const tgm_regdump_how how = {2,          0x142028,   0x80002000, 0x80002000,
				     0x40000123, 0x40000123, 3456};
	const size_t len = tgm_regdump_format(buf.data(), buf.size(), values.data(), &how);
	ASSERT_GT(len, 0u);
	ASSERT_LT(len, buf.size());

	std::istringstream text(std::string(buf.data(), len));
	std::string line;
	ASSERT_TRUE(std::getline(text, line));
	char header[96];
	snprintf(header, sizeof(header), "# regdump v1 list=%016llx count=%u",
		 static_cast<unsigned long long>(TGM_REGDUMP_LIST_ID),
		 static_cast<unsigned>(TGM_REGDUMP_COUNT));
	EXPECT_EQ(line, header);
	ASSERT_TRUE(std::getline(text, line));
	EXPECT_EQ(line, "# read badf=2 bus_fault=142028 engine0=80002000/80002000 "
			"fecs_ctx=40000123/40000123 "
			"read_us=3456");
	const std::regex form(R"(^0x([0-9a-f]{6}) 0x([0-9a-f]{8})$)");
	size_t i = 0;
	for (; std::getline(text, line); i++) {
		std::smatch m;
		ASSERT_TRUE(std::regex_match(line, m, form)) << line;
		EXPECT_EQ(std::stoul(m[1].str(), nullptr, 16), tgm_regdump_offsets[i]);
		EXPECT_EQ(std::stoul(m[2].str(), nullptr, 16), values[i]);
	}
	EXPECT_EQ(i, TGM_REGDUMP_COUNT);
}

TEST(Gm20bRegdump, AShortBufferGetsNothing)
{
	std::vector<char> buf(TGM_REGDUMP_BYTES - 1);
	const std::vector<uint32_t> values = Values();
	const tgm_regdump_how how = {};
	EXPECT_EQ(tgm_regdump_format(buf.data(), buf.size(), values.data(), &how), 0u);
}

uint32_t Fnv1a(const std::vector<uint32_t> &values)
{
	uint32_t h = 0x811c9dc5u;
	for (const uint32_t value : values) {
		unsigned char bytes[4];
		memcpy(bytes, &value, sizeof(bytes));
		for (const unsigned char byte : bytes)
			h = (h ^ byte) * 0x01000193u;
	}
	return h;
}

TEST(Gm20bRegdump, TheLogLinesCarryEveryValueOnceInListOrder)
{
	const std::vector<uint32_t> values = Values();
	const std::regex form(
		R"(^tegra-gm20b: regdump v1 list=([0-9a-f]{16}) tag=hot_states chunk=(\d+)/(\d+) )"
		R"(first=(\d+) n=(\d+)((?: [0-9a-f]{8})*)$)");
	char id[17];
	snprintf(id, sizeof(id), "%016llx", static_cast<unsigned long long>(TGM_REGDUMP_LIST_ID));
	std::vector<uint32_t> logged;
	for (size_t chunk = 0; chunk < TGM_REGDUMP_CHUNKS; chunk++) {
		char line[TGM_REGDUMP_LOG_LINE];
		const size_t len = tgm_regdump_chunk_line(line, sizeof(line), values.data(),
							  "hot_states", chunk);
		ASSERT_LT(len, sizeof(line)) << chunk;
		std::cmatch m;
		ASSERT_TRUE(std::regex_match(line, m, form)) << line;
		EXPECT_EQ(m[1].str(), id);
		EXPECT_EQ(std::stoul(m[2].str()), chunk);
		EXPECT_EQ(std::stoul(m[3].str()), TGM_REGDUMP_CHUNKS);
		const size_t before = logged.size();
		EXPECT_EQ(std::stoul(m[4].str()), before);
		std::istringstream words(m[6].str());
		size_t n = 0;
		for (std::string word; words >> word; n++)
			logged.push_back(static_cast<uint32_t>(std::stoul(word, nullptr, 16)));
		EXPECT_EQ(std::stoul(m[5].str()), n);
		EXPECT_EQ(n, std::min<size_t>(TGM_REGDUMP_CHUNK, TGM_REGDUMP_COUNT - before));
	}
	EXPECT_EQ(logged, values);
}

TEST(Gm20bRegdump, TheDoneLineCarriesTheCountAndTheSum)
{
	std::vector<uint32_t> values = Values();
	const tgm_regdump_how how = {1, 0, 0x00002000, 0x80002000, 0x40000123, 0x40000124, 812};
	char line[TGM_REGDUMP_LOG_LINE];
	ASSERT_LT(tgm_regdump_done_line(line, sizeof(line), values.data(), "quiet", &how),
		  sizeof(line));
	char want[TGM_REGDUMP_LOG_LINE];
	snprintf(
		want, sizeof(want),
		"tegra-gm20b: regdump v1 list=%016llx tag=quiet done count=%u chunks=%u sum=%08x "
		"badf=1 "
		"bus_fault=000000 engine0=00002000/80002000 fecs_ctx=40000123/40000124 read_us=812",
		static_cast<unsigned long long>(TGM_REGDUMP_LIST_ID),
		static_cast<unsigned>(TGM_REGDUMP_COUNT), static_cast<unsigned>(TGM_REGDUMP_CHUNKS),
		Fnv1a(values));
	EXPECT_STREQ(line, want);

	const uint32_t sum = tgm_regdump_sum(values.data());
	values[TGM_REGDUMP_COUNT / 2] ^= 0x00000100u;
	EXPECT_NE(tgm_regdump_sum(values.data()), sum);
}

TEST(Gm20bRegdump, AShortLineBufferStaysTerminated)
{
	const std::vector<uint32_t> values = Values();
	char line[48];
	memset(line, 'x', sizeof(line));
	EXPECT_GE(tgm_regdump_chunk_line(line, sizeof(line), values.data(), "quiet", 0),
		  sizeof(line));
	EXPECT_EQ(strlen(line), sizeof(line) - 1);
}

TEST(Gm20bRegdump, PriErrorsAreCounted)
{
	std::vector<uint32_t> values(TGM_REGDUMP_COUNT, 0);
	values[3] = 0xbadf1100u;
	values[TGM_REGDUMP_COUNT - 1] = 0xbadf5040u;
	values[7] = 0x0badf000u;
	EXPECT_EQ(tgm_regdump_badf(values.data()), 2u);
}

}  // namespace
