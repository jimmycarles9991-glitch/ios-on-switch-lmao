// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>
#include <array>
#include <string>
#include "drivers/TegraGM20BMetal/precompile/ca-selectors/ca_selector_layout.h"

TEST(CaSelectorIds, ImmutableBindingBytesResolveOnceWithoutTreatingPaddingAsASelector)
{
    const std::string hex =
        "0000000000000000000000000101ff000000000000000000000000000000ff000001030001000100000000000000000000003c003c";
    std::array<uint8_t, 52> bytes = {};
    for (size_t i = 0; i < bytes.size(); i++)
        bytes[i] = uint8_t(std::stoul(hex.substr(2 * i, 2), nullptr, 16));
    EXPECT_EQ(gm20b::caSelectorId(nullptr, 0), 0u);
    EXPECT_EQ(gm20b::caSelectorId(bytes.data(), bytes.size() - 1), 0u);
    const uint16_t id = gm20b::caSelectorId(bytes.data(), bytes.size());
    if (!gm20b::kCaSelectorConfigs) {
        EXPECT_EQ(id, 0u);
        return;
    }
    ASSERT_NE(id, 0u);
    bytes[49] ^= 0xff;
    bytes[50] ^= 0xff;
    bytes[51] ^= 0xff;
    EXPECT_EQ(gm20b::caSelectorId(bytes.data(), bytes.size()), id);
    bytes[48] = 0xff;
    EXPECT_EQ(gm20b::caSelectorId(bytes.data(), bytes.size()), 0u);
}
