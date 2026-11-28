// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSPIWords.h: what the TX FIFO gets and what the RX FIFO gives back for a block, packed and not.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "drivers/TegraSPI/TegraSPIWords.h"

namespace {

std::vector<uint32_t> TxWords(const std::vector<uint8_t> &tx)
{
    const bool packed = tegra_spi_packed(tx.size());
    std::vector<uint32_t> words;
    for (size_t i = 0; i < tegra_spi_words(tx.size(), packed); ++i)
        words.push_back(tegra_spi_tx_word(tx.data(), tx.size(), packed, i));
    return words;
}

TEST(TegraSpiWords, ShortBlocksStayOneByteAWord)
{
    EXPECT_FALSE(tegra_spi_packed(3));
    EXPECT_EQ(TxWords({0x3a, 0x80}), (std::vector<uint32_t>{0x3a, 0x80}));
}

TEST(TegraSpiWords, LongerBlocksPackFourBytesAWordByteZeroLowest)
{
    EXPECT_TRUE(tegra_spi_packed(4));
    EXPECT_EQ(TxWords({0xbf, 1, 2, 3, 4, 5}), (std::vector<uint32_t>{0x030201bf, 0x0504}));
    EXPECT_EQ(tegra_spi_words(256, true), 64u) << "the 64-word FIFO holds the longest block";
    EXPECT_EQ(tegra_spi_tx_word(nullptr, 6, true, 1), 0u) << "no TX bytes: zeros";
}

TEST(TegraSpiWords, RxWordsGiveBackTheBlockAndNoMore)
{
    uint8_t rx[6] = {};
    size_t done = tegra_spi_rx_word(0x44332211, rx, 0, sizeof(rx), true);
    done = tegra_spi_rx_word(0xeeff6655, rx, done, sizeof(rx), true);
    EXPECT_EQ(done, 6u) << "the last word's bytes past the block are not data";
    EXPECT_EQ(std::vector<uint8_t>(rx, rx + 6), (std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44, 0x55, 0x66}));
    EXPECT_EQ(tegra_spi_rx_word(0x1234, rx, 1, 2, false), 2u);
    EXPECT_EQ(rx[1], 0x34) << "unpacked: the word's low byte";
    EXPECT_EQ(tegra_spi_rx_word(0xffffffff, nullptr, 0, 4, true), 4u) << "no RX buffer: counted, dropped";
}

}  // namespace
