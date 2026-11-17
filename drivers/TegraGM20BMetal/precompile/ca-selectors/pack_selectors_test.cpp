// SPDX-License-Identifier: MIT
#include "../../raw/runtime/raw_pack_selectors.h"
#include "../../raw/pack/selector_configs.h"
#include <gtest/gtest.h>
#include <string>

TEST(PackSelectors, PreparedProgramIdsHaveTheGeneratorsLayout)
{
    const uint32_t section[] = {2, 0, 5, 24, 9, 48, 2, 32, 1, 100, 2, 101, 1, 0, 1, 200};
    const auto *bytes = reinterpret_cast<const uint8_t *>(section);
    const auto *index = reinterpret_cast<const raw::PackSelectorIndex *>(bytes + 8);
    EXPECT_EQ(index[0].record, 5u);
    EXPECT_EQ(index[1].record, 9u);
    const auto &first = *reinterpret_cast<const raw::PackSelectorTable *>(bytes + index[0].at);
    EXPECT_EQ(first.configs, 2u);
    EXPECT_EQ(first.assume, 32u);
    EXPECT_EQ(raw::selectorConfigs(first)[1].id, 2u);
    EXPECT_EQ(raw::selectorConfigs(first)[1].program, 101u);
}

TEST(PackSelectors, GlobalIdsAreSortedByValueAndSharedBetweenStates)
{
    std::map<uint64_t, raw::SelectorConfigs> configs;
    std::string why;
    ASSERT_TRUE(pack::readSelectorConfigs(
        "layout slot=0 length=2 padding=\n0000000000000001 0200\n0000000000000001 0100\n0000000000000002 0100\n",
        configs, why))
        << why;
    ASSERT_EQ(configs.size(), 2u);
    EXPECT_EQ(configs.at(1).values[0].id, 2u);
    EXPECT_EQ(configs.at(1).values[1].id, 1u);
    EXPECT_EQ(configs.at(2).values[0].id, 1u);
}
