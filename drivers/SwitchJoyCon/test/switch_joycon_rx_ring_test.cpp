// The rail's RX ring takes (SwitchJoyConRxRing.h): new words across the wrap, a channel whose end of count is not
// counted yet at the wrap, and a channel that lapped untaken words.
#include <gtest/gtest.h>

#include <cstdint>

#include "../SwitchJoyConRxRing.h"

namespace {

constexpr uint32_t kWords = 4096;  // a 16 KiB ring

TEST(SwitchJoyConRxRing, TakesTheWordsSinceTheLastTake)
{
    const switch_joycon_rx_take take = switch_joycon_rx_ring_take(kWords, 100, 161 * 4, 0, 100);
    EXPECT_EQ(take.from, 100u);
    EXPECT_EQ(take.count, 61u);
    EXPECT_FALSE(take.overrun);
}

TEST(SwitchJoyConRxRing, TakesAcrossTheWrap)
{
    // 61 words from word 4070: 26 to the end, 35 from the start, the wrap's end of count counted.
    const switch_joycon_rx_take take = switch_joycon_rx_ring_take(kWords, 4070, 35 * 4, 1, 4070);
    EXPECT_EQ(take.from, 4070u);
    EXPECT_EQ(take.count, 61u);
    EXPECT_FALSE(take.overrun);
}

TEST(SwitchJoyConRxRing, ToleratesAnEndOfCountNotCountedYet)
{
    // The counter reloads a little before STATUS shows the end of count, so a take right at the wrap sees 0.
    const switch_joycon_rx_take atWrap = switch_joycon_rx_ring_take(kWords, 4070, 0, 0, 4070);
    EXPECT_EQ(atWrap.count, 26u);
    EXPECT_FALSE(atWrap.overrun);
    const switch_joycon_rx_take after = switch_joycon_rx_ring_take(kWords, 4070, 35 * 4, 0, 4070);
    EXPECT_EQ(after.count, 61u);
    EXPECT_FALSE(after.overrun);
}

TEST(SwitchJoyConRxRing, NothingNewIsNoOverrun)
{
    const switch_joycon_rx_take take = switch_joycon_rx_ring_take(kWords, 500, 500 * 4, 3, 3 * kWords + 500);
    EXPECT_EQ(take.count, 0u);
    EXPECT_FALSE(take.overrun);
}

TEST(SwitchJoyConRxRing, AChannelThatLappedTheTakesIsAnOverrun)
{
    // Exactly one ring since the last take: no new words show, but the extra lap does.
    EXPECT_TRUE(switch_joycon_rx_ring_take(kWords, 500, 500 * 4, 1, 500).overrun);
    // More than a ring: the words that show are the last pass's, the lap before them lost.
    EXPECT_TRUE(switch_joycon_rx_ring_take(kWords, 500, 600 * 4, 1, 500).overrun);
}

}  // namespace
