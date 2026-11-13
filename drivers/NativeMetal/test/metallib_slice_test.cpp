// SPDX-License-Identifier: MIT
// MetallibSlice.h: the AIR slice of a universal metallib the selected IPSW's Metal loads, on that IPSW's own fat libraries
// (24B119's CoreImage.metallib: AIR 2.4 and AIR 2.9 slices; QuartzCore's: AIR beside the AGX binary) and over the rules of
// both kinds.
#include "drivers/NativeMetal/MetallibEntries.h"
#include "drivers/NativeMetal/MetallibSlice.h"
#include <gtest/gtest.h>
#include <cstdlib>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <string>
#include <vector>

namespace {

using switch_metal::AirSliceRule;
using switch_metal::kAirSliceRules;

// What this IPSW's Metal loads of a library: its AIR subtype (0: a thin file) and its entries
struct Expected {
    std::string_view kind;
    const char *library;
    uint32_t subtype;
    size_t entries;
};

constexpr Expected kExpected[] = {
    {"21H461", "CoreImage.framework/CoreImage.metallib", 0, 73},
    {"21H461", "QuartzCore.framework/default.metallib", 0xa, 0},
    {"24B119", "CoreImage.framework/CoreImage.metallib", 0x8, 75},
    {"24B119", "QuartzCore.framework/default.metallib", 0xd, 0},
};

std::vector<uint8_t> frameworkFile(const char *library)
{
    const std::string quartzCore = getenv("SWITCH_QUARTZCORE") ?: "";
    const std::string mine = "QuartzCore.framework/default.metallib";
    if (quartzCore.size() < mine.size())
        return {};
    std::ifstream in(quartzCore.substr(0, quartzCore.size() - mine.size()) + library, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

uint32_t subtypeAt(const std::vector<uint8_t> &file, size_t offset)
{
    const uint32_t count = uint32_t(file[4]) << 24 | uint32_t(file[5]) << 16 | uint32_t(file[6]) << 8 | file[7];
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *a = file.data() + 8 + 20 * i;
        const uint32_t at = uint32_t(a[8]) << 24 | uint32_t(a[9]) << 16 | uint32_t(a[10]) << 8 | a[11];
        if (at == offset)
            return uint32_t(a[4]) << 24 | uint32_t(a[5]) << 16 | uint32_t(a[6]) << 8 | a[7];
    }
    return ~0u;
}

TEST(MetallibSlice, ThisIpswsLibrariesLoadTheSliceItsMetalTakes)
{
    size_t checked = 0;
    for (const Expected &e : kExpected) {
        if (e.kind != SWITCH_METAL_KIND)
            continue;
        const std::vector<uint8_t> file = frameworkFile(e.library);
        ASSERT_FALSE(file.empty()) << e.library;
        size_t offset = 0, size = 0;
        ASSERT_TRUE(switch_metal::airSlice(file.data(), file.size(), offset, size)) << e.library;
        EXPECT_EQ(e.subtype ? subtypeAt(file, offset) : 0u, e.subtype) << e.library;
        if (!e.subtype)
            EXPECT_EQ(size, file.size()) << e.library;
        if (e.entries) {
            size_t entries = 0;
            EXPECT_TRUE(switch_metal::forEachMetallibEntry(file.data() + offset, size, [&](const auto &) {
                entries++;
                return true;
            }));
            EXPECT_EQ(entries, e.entries) << e.library;
        }
        checked++;
    }
    EXPECT_EQ(checked, 2u) << SWITCH_METAL_KIND;
}

std::vector<uint8_t> universal(std::initializer_list<uint32_t> subtypes)
{
    std::vector<uint8_t> file;
    const auto word = [&](uint32_t w) {
        for (int shift = 24; shift >= 0; shift -= 8)
            file.push_back(uint8_t(w >> shift));
    };
    word(0xcafebabe);
    word(uint32_t(subtypes.size()));
    uint32_t at = 8 + 20 * uint32_t(subtypes.size());
    for (uint32_t subtype : subtypes) {
        word(switch_metal::kAir64CpuType);
        word(subtype);
        word(at);
        word(16);
        word(4);
        at += 16;
    }
    file.resize(at, 0);
    return file;
}

uint32_t picked(const AirSliceRule &rule, std::initializer_list<uint32_t> subtypes)
{
    const std::vector<uint8_t> file = universal(subtypes);
    size_t offset = 0, size = 0;
    if (!switch_metal::airSlice(file.data(), file.size(), offset, size, rule))
        return 0;
    return *(subtypes.begin() + (offset - (8 + 20 * subtypes.size())) / 16);
}

TEST(MetallibSlice, IosSeventeenTakesTheFirstAirSlice)
{
    const AirSliceRule &rule = kAirSliceRules[0];
    ASSERT_EQ(rule.kind, "21H461");
    EXPECT_EQ(picked(rule, {0xd, 0x8}), 0xdu);
    EXPECT_EQ(picked(rule, {0x8, 0xd}), 0x8u);
}

TEST(MetallibSlice, IosTwentySevenBelowProfileEightTakesTheNewestBeforeAir29)
{
    const AirSliceRule &rule = kAirSliceRules[1];
    ASSERT_EQ(rule.kind, "24B119");
    EXPECT_EQ(picked(rule, {0x8, 0xd}), 0x8u);
    EXPECT_EQ(picked(rule, {0xd, 0x8}), 0x8u);
    EXPECT_EQ(picked(rule, {0x6, 0x8, 0xd}), 0x8u);
    EXPECT_EQ(picked(rule, {0xd}), 0xdu);
    EXPECT_EQ(picked(rule, {0xe}), 0u) << "no slice newer than AIR 2.9";
    AirSliceRule newer = rule;
    newer.belowProfile8 = false;
    EXPECT_EQ(picked(newer, {0x8, 0xd}), 0xdu);
}

}  // namespace
