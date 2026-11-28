// SPDX-License-Identifier: BSD-2-Clause-Patent
// TegraSPI's FIFO words for a block of 8-bit packets: one byte a word unpacked, or four packed with byte 0 in bits 7:0
// (Tegra X1 TRM 37.2.1 "packet 0 in byte 0 of the FIFO"; spi-tegra114.c tegra_spi_fill_tx_fifo_from_client_txbuf and
// tegra_spi_read_rx_fifo_to_client_rxbuf). Packed from four bytes on, as tegra_spi_calculate_curr_xfer_param packs
// (len > 3). Plain C++, so the host test checks it.
#pragma once
#include <stddef.h>
#include <stdint.h>

inline bool tegra_spi_packed(size_t len)
{
    return len > 3;
}

inline size_t tegra_spi_words(size_t len, bool packed)
{
    return packed ? (len + 3) / 4 : len;
}

// TX FIFO word `word` of the block (tx null: zeros).
inline uint32_t tegra_spi_tx_word(const uint8_t *tx, size_t len, bool packed, size_t word)
{
    if (!packed)
        return tx ? tx[word] : 0;
    uint32_t value = 0;
    for (size_t i = 0; i < 4 && 4 * word + i < len; ++i)
        value |= static_cast<uint32_t>(tx ? tx[4 * word + i] : 0) << (8 * i);
    return value;
}

// An RX FIFO word's bytes into rx[done...] (rx null: dropped), at most up to `len`; returns the new count.
inline size_t tegra_spi_rx_word(uint32_t value, uint8_t *rx, size_t done, size_t len, bool packed)
{
    const unsigned perWord = packed ? 4 : 1;
    for (unsigned i = 0; i < perWord && done < len; ++i, ++done) {
        if (rx)
            rx[done] = static_cast<uint8_t>(value >> (8 * i));
    }
    return done;
}
