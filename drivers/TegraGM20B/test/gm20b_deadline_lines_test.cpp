// SPDX-License-Identifier: MIT
// deadline_lines.c: each deadline stats line whole at every count's widest. Boot 22 printed the deadline's counts through
// a 320-byte buffer, so none of its 352 lines reached "owner runs timed"; with its prefix every line fits nvxnu_stats's
// 1 KiB, the kernel's largest os_log message.
#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
extern "C" {
#include "drivers/TegraGM20B/deadline_stats.h"
}

namespace
{
constexpr size_t kLogLine = 1024;
const size_t kPrefix = std::strlen("tegra-gm20b: deadline ");

std::vector<std::string> gPrinted;

void collect(const char *line)
{
	gPrinted.emplace_back(line);
}

bool endsWith(const std::string &text, const std::string &end)
{
	return text.size() >= end.size() &&
	       text.compare(text.size() - end.size(), end.size(), end) == 0;
}

TEST(DeadlineLines, TheBufferAndItsPrefixFitOneLogLine)
{
	EXPECT_LT(kPrefix + TGM_DEADLINE_LINE, kLogLine);
}

TEST(DeadlineLines, TheDeadlineLineIsWholeAtEveryCountsWidest)
{
	struct tgm_deadline_line l = {};
	l.gained = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
		    UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT64_MAX};
	l.mean_us = l.p95_us = l.max_us = l.learned = UINT64_MAX;
	char line[TGM_DEADLINE_LINE];
	tgm_deadline_format(line, sizeof(line), &l);
	const std::string text(line);
	EXPECT_LT(text.size() + 1, sizeof(line)) << text;
	EXPECT_TRUE(endsWith(text, "4294967295 owner runs timed, 4294967295 woken")) << text;
}

TEST(DeadlineLines, TheHoldLineIsWholeAtEveryCountsWidest)
{
	const struct tgm_hold_counts gained = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX,
					       UINT32_MAX, UINT32_MAX, UINT32_MAX};
	char line[TGM_DEADLINE_LINE];
	tgm_hold_format(line, sizeof(line), &gained);
	const std::string text(line);
	EXPECT_LT(text.size() + 1, sizeof(line)) << text;
	EXPECT_TRUE(endsWith(text, "4294967295 samples, 4294967295 lowered")) << text;
}

TEST(DeadlineLines, ManyChannelsContinueOnNewLinesWithNoneCut)
{
	constexpr unsigned kChannels = 64;
	char buf[TGM_DEADLINE_LINE];
	struct tgm_latency_lines lines = {buf, sizeof(buf), 0, 0, collect};
	gPrinted.clear();
	for (unsigned i = 0; i < kChannels; i++)
		tgm_latency_add(&lines, "sixteen-char-own", 4000000000u + i, UINT64_MAX,
				i % 2 ? UINT32_MAX : 0);
	tgm_latency_end(&lines);
	ASSERT_GT(gPrinted.size(), 1u);
	EXPECT_EQ(gPrinted[0].rfind("latency by channel:", 0), 0u);
	size_t entries = 0;
	for (size_t i = 0; i < gPrinted.size(); i++) {
		const std::string &text = gPrinted[i];
		EXPECT_LT(text.size() + 1, sizeof(buf)) << text;
		if (i)
			EXPECT_EQ(text.rfind("latency by channel, continued:", 0), 0u) << text;
		for (size_t at = text.find(" sixteen-char-own ch"); at != std::string::npos;
		     at = text.find(" sixteen-char-own ch", at + 1)) {
			const size_t end = text.find(" sixteen-char-own ch", at + 1);
			const std::string entry =
				text.substr(at, end == std::string::npos ? end : end - at);
			EXPECT_TRUE(endsWith(entry, " us") || endsWith(entry, " turns)")) << entry;
			entries++;
		}
	}
	EXPECT_EQ(entries, kChannels);
}

TEST(DeadlineLines, NoChannelAboveTheMinimumSaysSo)
{
	char buf[TGM_DEADLINE_LINE];
	struct tgm_latency_lines lines = {buf, sizeof(buf), 0, 0, collect};
	gPrinted.clear();
	tgm_latency_end(&lines);
	ASSERT_EQ(gPrinted.size(), 1u);
	EXPECT_EQ(gPrinted[0], "latency by channel: all at the minimum");
}
}  // namespace
