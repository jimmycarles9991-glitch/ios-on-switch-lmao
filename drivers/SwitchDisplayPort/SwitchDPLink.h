// SPDX-License-Identifier: GPL-2.0-only
// The link for a mode, SOR1's transfer-unit configuration for it, and PLL_D2's dividers for its pixel clock. Plain C
// for the host tests.
//
// switch_dp_tu_config is Linux drivers/gpu/drm/tegra/sor.c tegra_sor_compute_params/_config (Copyright (C) 2013
// NVIDIA Corporation, GPL-2.0-only). PLL_D2: drivers/clk/tegra/clk-tegra210.c (pll_d2_params: PLLSS, VCO 750-1500 MHz,
// the qlin post divider, SDM) and clk-pll.c (Fvco = Fref * (N + 1/2 + SDM_DIN / 8192) / M).
#ifndef SWITCH_DP_LINK_H
#define SWITCH_DP_LINK_H

#include <stdbool.h>
#include <stdint.h>

#define SWITCH_DP_RATE_RBR  0x06u
#define SWITCH_DP_RATE_HBR  0x0Au
#define SWITCH_DP_RATE_HBR2 0x14u

// The link symbol clock (kHz) of a DPCD link rate code: 27 MHz units.
static inline uint32_t switch_dp_rate_khz(uint8_t rate)
{
    return rate * 27000u;
}

// Video payload of a link (kbit/s): one byte per symbol per lane after 8b/10b.
static inline uint64_t switch_dp_payload_kbps(uint8_t rate, uint8_t lanes)
{
    return (uint64_t)switch_dp_rate_khz(rate) * 8u * lanes;
}

// The fastest rate SOR1 and the sink share: Tegra drives RBR, HBR and HBR2.
static inline uint8_t switch_dp_rate_cap(uint8_t sink_max_rate)
{
    if (sink_max_rate >= SWITCH_DP_RATE_HBR2) {
        return SWITCH_DP_RATE_HBR2;
    }
    if (sink_max_rate >= SWITCH_DP_RATE_HBR)
        return SWITCH_DP_RATE_HBR;
    return sink_max_rate >= SWITCH_DP_RATE_RBR ? SWITCH_DP_RATE_RBR : 0;
}

struct switch_dp_link {
    uint8_t rate;
    uint8_t lanes;
};

struct switch_dp_tu {
    uint32_t tu_size, active_count, active_frac, active_polarity, watermark;
    uint32_t hblank_symbols, vblank_symbols;
};

static inline bool switch_dp_tu_config(uint32_t pixel_khz, uint32_t hactive, uint32_t htotal, uint8_t rate,
                                       uint8_t lanes, bool enhanced_framing, struct switch_dp_tu *tu)
{
    const uint64_t symbol_khz = switch_dp_rate_khz(rate);
    const uint64_t input = (uint64_t)pixel_khz * 3, output = symbol_khz * lanes;
    if (!input || input >= output || !hactive || htotal <= hactive ||
        (lanes != 1 && lanes != 2 && lanes != 4))
        return false;

    // TRM 27.8.41: the fractional symbol is sent once per F units (polarity 0), or on all but one (1).
    uint64_t best_excess = 0, best_period = 0;
    for (uint32_t size = 64; size >= 32; --size) {
        const uint64_t target = input * size, rem = target % output;
        uint32_t count = (uint32_t)(target / output), frac = 1, polarity = 0;
        if (!rem) {
            --count;
        } else if (rem * 2 <= output) {
            const uint64_t denominator = output / rem;
            frac = (uint32_t)(denominator > 15 ? 15 : denominator);
        } else {
            const uint64_t complement = output - rem;
            const uint64_t denominator = (output + complement - 1) / complement;
            if (denominator <= 15) {
                frac = (uint32_t)denominator;
                polarity = 1;
            }
        }
        const uint64_t emitted = (uint64_t)count * frac + (polarity ? frac - 1 : 1);
        const uint64_t excess = emitted * output - target * frac, period = (uint64_t)size * frac;
        if (best_period && excess * best_period >= best_excess * period)
            continue;
        best_excess = excess;
        best_period = period;
        tu->tu_size = size;
        tu->active_count = count;
        tu->active_frac = frac;
        tu->active_polarity = polarity;
        if (!excess)
            break;
    }
    const uint64_t precision = 100000, ratio = input * precision / output;
    const uint64_t clocks = symbol_khz * hactive / pixel_khz;
    const uint64_t error = (best_excess * precision / (output * best_period)) * clocks;
    tu->watermark = (uint32_t)((ratio * tu->tu_size * (precision - ratio) / precision + error) / precision) + 5;
    if (tu->watermark > 30 || tu->watermark > (uint64_t)hactive * 3 / lanes)
        return false;
    const int64_t hblank = ((int64_t)htotal - hactive - 7) * (int64_t)symbol_khz / pixel_khz -
                           (enhanced_framing ? 3 : 0) - 12 / lanes;
    const int64_t vblank = ((int64_t)hactive - 25) * (int64_t)symbol_khz / pixel_khz - (36 / lanes + 4);
    tu->hblank_symbols = hblank > 0 ? (uint32_t)hblank : 0;
    tu->vblank_symbols = vblank > 0 ? (uint32_t)vblank : 0;
    return true;
}

static inline bool switch_dp_next_link(uint32_t pixel_khz, uint32_t hactive, uint32_t htotal, uint8_t max_rate,
                                       uint8_t max_lanes, bool enhanced, bool downspread, unsigned *cursor,
                                       struct switch_dp_link *link, struct switch_dp_tu *tu)
{
    static const struct switch_dp_link choices[] = {
        {SWITCH_DP_RATE_RBR, 1}, {SWITCH_DP_RATE_RBR, 2}, {SWITCH_DP_RATE_HBR, 1},
        {SWITCH_DP_RATE_HBR, 2}, {SWITCH_DP_RATE_RBR, 4}, {SWITCH_DP_RATE_HBR, 4},
        {SWITCH_DP_RATE_HBR2, 1}, {SWITCH_DP_RATE_HBR2, 2}, {SWITCH_DP_RATE_HBR2, 4}
    };
    while (*cursor < sizeof(choices) / sizeof(choices[0])) {
        const struct switch_dp_link candidate = choices[(*cursor)++];
        if (candidate.rate > max_rate || candidate.lanes > max_lanes)
            continue;
        const uint64_t capacity = switch_dp_payload_kbps(candidate.rate, candidate.lanes);
        if ((uint64_t)pixel_khz * 24 >= (downspread ? capacity * 995 / 1000 : capacity))
            continue;
        if (switch_dp_tu_config(pixel_khz, hactive, htotal, candidate.rate, candidate.lanes, enhanced, tu)) {
            *link = candidate;
            return true;
        }
    }
    return false;
}

struct switch_dp_plld2 {
    uint32_t n, m, pdiv, pdiv_hw;
    uint16_t sdm_din;
    uint64_t out_hz;
};

// Switchroot clocks pll_d2_out0 at PLL_D2's rate; DISP2 and both DC dividers are 1:1.
static inline bool switch_dp_plld2_for(uint32_t pixel_khz, uint32_t ref_khz, bool b01, struct switch_dp_plld2 *pll)
{
    static const uint8_t pdivs[17][2] = {{1, 0},   {2, 1},   {3, 2},   {4, 3},   {5, 4},   {6, 5},
                                         {8, 6},   {9, 7},   {10, 8},  {12, 9},  {15, 10}, {16, 11},
                                         {18, 12}, {20, 13}, {24, 14}, {30, 15}, {32, 16}};
    const uint64_t target_hz = (uint64_t)pixel_khz * 1000u, ref_hz = (uint64_t)ref_khz * 1000u;
    if (target_hz == 0 || ref_hz == 0) {
        return false;
    }
    for (unsigned i = 0; i < 17; ++i) {
        // PDIV code 16 is /31 on B01 and /32 on Erista.
        const uint32_t pdiv = b01 && pdivs[i][0] == 32 ? 31u : pdivs[i][0];
        const uint64_t vco = target_hz * pdiv;
        if (vco < (b01 ? 780000000u : 750000000u)) {
            continue;
        }
        if (vco > (b01 ? 1620000000u : 1500000000u)) {
            return false;
        }
        const uint64_t scaled = (vco * 8192u + ref_hz / 2) / ref_hz;  // (N + 1/2 + SDM / 8192) * 8192
        const uint64_t n = scaled / 8192u;
        const int64_t din = (int64_t)(scaled - n * 8192u) - 4096;
        const int64_t effective = (int64_t)(n * 8192u + 4096u) + din;
        pll->n = (uint32_t)n;
        pll->m = 1;
        pll->pdiv = pdiv;
        pll->pdiv_hw = pdivs[i][1];
        pll->sdm_din = (uint16_t)din;
        pll->out_hz = (uint64_t)effective * ref_hz / 8192u / pdiv;
        return n > 0 && n < 256;
    }
    return false;
}

#endif
