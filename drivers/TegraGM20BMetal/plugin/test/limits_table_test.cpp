// SPDX-License-Identifier: MIT
// LimitsTable.h: the device's answers in the base class's limits table, over a model of 21H461's _MTLDevice, whose
// minConstantBufferAlignmentBytes is not read from the table: every other field the device answers is filled, the 2D texture
// size PhotosUICore reads at 0x84/0x88 among them.
#include "../LimitsTable.h"

#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

struct BaseModel {
    uint8_t table[kMTLDeviceLimitsBytes] = {};

    uint32_t at(uint16_t offset) const
    {
        uint32_t v;
        memcpy(&v, table + offset, 4);
        return v;
    }

    uint64_t read(const LimitField &field) const
    {
        if (std::string(field.selector) == "minConstantBufferAlignmentBytes")
            return 4;
        if (field.bytes == 8) {
            uint64_t v;
            memcpy(&v, table + field.offset, 8);
            return v;
        }
        return at(field.offset);
    }
};

const std::map<std::string, uint64_t> kAnswers = {
    {"maxColorAttachments", 8},
    {"minConstantBufferAlignmentBytes", 256},
    {"minBufferNoCopyAlignmentBytes", 16384},
    {"maxTextureWidth1D", 16384},
    {"maxTextureWidth2D", 16384},
    {"maxTextureHeight2D", 16384},
    {"maxTextureLayers", 2048},
    {"deviceLinearReadOnlyTextureAlignmentBytes", 32},
    {"maxTextureBufferWidth", 1ull << 27},
    {"maxConstantBufferArguments", 31},
};

struct Fill {
    BaseModel base;
    std::vector<std::pair<std::string, LimitFill>> reports;

    explicit Fill(const std::map<std::string, uint64_t> &answers)
    {
        fillLimitsTable(
            base.table,
            [&](const LimitField &field, uint64_t &value) {
                auto it = answers.find(field.selector);
                if (it == answers.end())
                    return false;
                value = it->second;
                return true;
            },
            [&](const LimitField &field) {
                return base.read(field);
            },
            [&](const LimitField &field, LimitFill how) {
                reports.emplace_back(field.selector, how);
            });
    }
};

TEST(LimitsTable, FieldsPastOneTheBaseClassReadsElsewhereAreFilled)
{
    Fill f(kAnswers);
    EXPECT_EQ(f.base.at(0x20), 8u);
    EXPECT_EQ(f.base.at(0x78), 0u);
    EXPECT_EQ(f.base.at(0x7c), 16384u);
    EXPECT_EQ(f.base.at(0x80), 16384u);
    EXPECT_EQ(f.base.at(0x84), 16384u);
    EXPECT_EQ(f.base.at(0x88), 16384u);
    EXPECT_EQ(f.base.at(0x9c), 2048u);
    EXPECT_EQ(f.base.at(0xb0), 32u);
    EXPECT_EQ(f.base.at(0xe8), 1u << 27);
    EXPECT_EQ(f.base.at(0xfc), 31u);
    for (const auto &[selector, how] : f.reports)
        EXPECT_EQ(how, selector == "minConstantBufferAlignmentBytes" ? LimitFill::NotRead : LimitFill::Filled)
            << selector;
    EXPECT_EQ(f.reports.size(), kAnswers.size());
}

TEST(LimitsTable, AFieldWithoutAnAnswerKeepsTheBaseClasssValue)
{
    BaseModel base;
    const uint32_t mine = 77;
    memcpy(base.table + 0x98, &mine, 4);
    fillLimitsTable(
        base.table,
        [](const LimitField &, uint64_t &) {
            return false;
        },
        [&](const LimitField &field) {
            return base.read(field);
        },
        [](const LimitField &, LimitFill) {});
    EXPECT_EQ(base.at(0x98), 77u);
}

TEST(LimitsTable, AnAnswerPastThirtyTwoBitsIsLeftOut)
{
    Fill f({{"maxTextureWidth2D", 1ull << 32}, {"maxTextureHeight2D", 16384}});
    EXPECT_EQ(f.base.at(0x84), 0u);
    EXPECT_EQ(f.base.at(0x88), 16384u);
    ASSERT_EQ(f.reports.size(), 2u);
    EXPECT_EQ(f.reports[0].second, LimitFill::Overflow);
}

TEST(LimitsTable, OffsetsAreDistinctAlignedFieldsInsideTheTable)
{
    std::map<uint16_t, std::string> seen;
    for (const LimitField &field : kLimitFields) {
        EXPECT_EQ(field.offset % field.bytes, 0) << field.selector;
        EXPECT_LE(field.offset + field.bytes, kMTLDeviceLimitsBytes) << field.selector;
        EXPECT_TRUE(seen.emplace(field.offset, field.selector).second) << field.selector;
    }
    EXPECT_EQ(seen.size(), 62u);
}

// _MTLDevice's limits are 264 bytes in 21H461 and 24B119: maxBufferLength, a 64-bit field, at 0x100
TEST(LimitsTable, TheTableRunsThroughMaxBufferLength)
{
    EXPECT_EQ(kMTLDeviceLimitsBytes, 0x108u);
    Fill f({{"maxBufferLength", 3ull << 31}});
    uint64_t v;
    memcpy(&v, f.base.table + 0x100, 8);
    EXPECT_EQ(v, 3ull << 31);
    ASSERT_EQ(f.reports.size(), 1u);
    EXPECT_EQ(f.reports[0].second, LimitFill::Filled);
}

}  // namespace
