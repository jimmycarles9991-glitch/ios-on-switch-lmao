#include <gtest/gtest.h>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "../SwitchIrqSources.h"
using namespace switch_power;

namespace {

std::unique_ptr<IrqSources> window(const std::vector<std::pair<uint32_t, uint32_t>> &counts,
                                   std::unique_ptr<IrqSources> s = std::make_unique<IrqSources>())
{
    startIrqWindow(*s);
    for (const auto &[intid, count] : counts)
        noteIrq(*s, intid, count);
    return s;
}

TEST(IrqSources, AWindowCountsWhatEachIntidTookSinceTheLast)
{
    auto s = window({{119, 5000}, {60, 300}});
    s = window({{119, 15900}, {60, 300}, {67, 40}}, std::move(s));
    EXPECT_EQ(s->all, 10940u);
    EXPECT_EQ(s->sources, 2u) << "INTID 60 took none in this window";
    EXPECT_EQ(s->top[0].intid, 119u);
    EXPECT_EQ(s->top[0].count, 10900u);
    EXPECT_EQ(s->top[1].intid, 67u);
}

TEST(IrqSources, TheTopKeepsTheBusiestInOrder)
{
    std::vector<std::pair<uint32_t, uint32_t>> counts;
    for (uint32_t i = 0; i < 10; ++i)
        counts.push_back({kFirstSpi + i, (i * 7) % 10 + 1});
    const auto s = window(counts);
    for (uint32_t i = 1; i < kTopSources; ++i)
        EXPECT_GE(s->top[i - 1].count, s->top[i].count);
    EXPECT_EQ(s->top[0].count, 10u);
    EXPECT_EQ(s->top[kTopSources - 1].count, 5u);
}

TEST(IrqSources, ACountThatWrappedStillGivesItsWindow)
{
    auto s = window({{119, 0xfffffff0u}});
    s = window({{119, 0x10u}}, std::move(s));
    EXPECT_EQ(s->top[0].count, 0x20u);
}

TEST(IrqSources, TheLineNamesTheSourcesWithTheirRates)
{
    const auto s = window({{119, 8000}, {67, 2900}});
    char line[200];
    formatIrqSources(*s, 10000, line, sizeof(line));
    EXPECT_EQ(std::string(line),
              "10900 SPIs from 2 INTIDs (1090/s): 119 (spi 87) 8000 (800/s), 67 (spi 35) 2900 (290/s)");
}

TEST(IrqSources, ALineCutShortStaysInItsBuffer)
{
    const auto s = window({{119, 8000}, {67, 2900}, {68, 1}, {69, 2}});
    char line[40];
    EXPECT_EQ(formatIrqSources(*s, 10000, line, sizeof(line)), sizeof(line) - 1);
    EXPECT_EQ(std::string(line).size(), sizeof(line) - 1);
}

}  // namespace
