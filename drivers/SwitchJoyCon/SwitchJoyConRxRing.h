// SPDX-License-Identifier: BSD-2-Clause-Patent
// A rail's RX ring as its APB DMA channel fills it (SwitchJoyConRail.cpp, header "RX by APB DMA"): one word per byte,
// round and round (TegraAPBDMA.h tegra_apbdma_start_rx_words_ring). What a take finds there: the words since the last
// take, and whether the channel went round the ring past words nobody took. Plain C, so
// test/switch_joycon_rx_ring_test.cpp drives it on the host.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct switch_joycon_rx_take {
    uint32_t from;   // the first new word
    uint32_t count;  // new words, from `from` on, wrapping at the ring's end
    bool overrun;    // the channel lapped words not taken yet: data lost
};

// words: the ring's length in words. taken: the word the last take stopped at. moved: what tegra_apbdma_moved shows
// in bytes (the channel's place in its current pass; 0 at a wrap, whose end of count may not be counted yet). laps:
// ends of count since the ring started. total: words taken since then. A channel that moved exactly one ring since
// the last take shows no new words, and its extra lap is the overrun.
static inline struct switch_joycon_rx_take switch_joycon_rx_ring_take(uint32_t words, uint32_t taken, uint32_t moved,
                                                                      uint32_t laps, uint64_t total)
{
    struct switch_joycon_rx_take take;
    const uint32_t at = (moved / 4) % words;
    take.from = taken;
    take.count = (at + words - taken) % words;
    take.overrun = laps > (total + take.count) / words;
    return take;
}

#ifdef __cplusplus
}
#endif
