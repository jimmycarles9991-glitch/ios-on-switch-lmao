// SPDX-License-Identifier: BSD-2-Clause-Patent
// The rate NVDEC, NVENC and NVJPG run at on T210B01, and the PLL that gives it. L4T clocks them from shared buses with a
// PLL each (clk-tegra210.c tegra210_clk_register_buses: c3bus on PLLC3 for NVENC and NVDEC, c2bus on PLLC2 for NVJPG,
// VIC, SE and TSECB); PLLC4 is not in their mux (clk-tegra-periph.c mux_pllc2_c_c3_pllp_plla1_clkm: PLLC2 1, PLLC 2,
// PLLC3 3, PLLP 4, PLLA1 6, CLK_M 7). The rate is the one L4T's core DVFS table allows at VDD_CORE's minimum, 637 mV
// (tegra210-dvfs.c get_coreb01_sku_min_mv; tegra210b01-core-dvfs.c coreb01_dvfs_table, speedo 0, by SoC process), so
// it needs no core voltage at all beyond the floor. Where that rate is below PLLP's 408 MHz (process 0) the engines
// keep PLLP: 408 MHz is above process 0's 650 mV point (403.2 MHz for NVDEC), which the rail hekate leaves (1050 mV)
// covers.
#ifndef SWITCH_ENGINE_CLOCK_PLAN_H
#define SWITCH_ENGINE_CLOCK_PLAN_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SWITCH_ENGINE_BUS_C2 = 2,  // NVJPG
    SWITCH_ENGINE_BUS_C3 = 3,  // NVDEC, NVENC
};

#define SWITCH_ENGINE_SOURCE_PLLC2 (1u << 29)
#define SWITCH_ENGINE_SOURCE_PLLC3 (3u << 29)
#define SWITCH_ENGINE_SOURCE_PLLP  (4u << 29)
#define SWITCH_ENGINE_PLLP_KHZ     408000u

// speedo-tegra210.c soc_process_speedos, T210B01 row; -1 for an unfused speedo.
static inline int switch_engine_soc_process(int32_t soc_speedo)
{
    if (soc_speedo <= 0)
        return -1;
    return soc_speedo < 1598 ? 0 : soc_speedo < 1709 ? 1 : 2;
}

// coreb01_dvfs_table's 637 mV column: "c3bus", "nvdec", "nvenc" and "c2bus", "nvjpg" rows, processes 0-2.
static inline uint32_t switch_engine_vmin_khz(unsigned bus, int process)
{
    static const uint32_t kC3[3] = {364800, 460800, 556800}, kC2[3] = {307200, 422400, 499200};
    if (process < 0 || process > 2 || (bus != SWITCH_ENGINE_BUS_C2 && bus != SWITCH_ENGINE_BUS_C3))
        return 0;
    return bus == SWITCH_ENGINE_BUS_C3 ? kC3[process] : kC2[process];
}

// A PLLC2/PLLC3 BASE (M 7:0, N 17:10, P 24:20 as the QLIN code) for `khz` from the 38.4 MHz reference, as L4T's
// T210B01 tegra210b01_pll_fixed_mdiv_cfg picks it: M 2 (clk-tegra210b01.c pll_c2/c3_params mdiv_default, a 19.2 MHz
// comparison at its cf_min; T210's M 3 would be below it), the smallest P putting the VCO at 614.4 MHz or more (vco_min
// 600 MHz rounded up to the reference, _clip_vco_min), N = VCO / 19.2 MHz. 0 when the rate is not an exact multiple or
// the VCO passes 1200 MHz.
static inline uint32_t switch_engine_pllcx_base(uint32_t khz)
{
    static const struct {
        uint32_t divider, code;
    } kQlin[] = {{1, 0}, {2, 1}, {3, 2}, {4, 3}, {5, 4}, {6, 5}, {8, 6}, {9, 7}, {10, 8}, {12, 9}, {15, 10}, {16, 11}};

    const uint32_t kComparisonKHz = 19200;
    if (!khz)
        return 0;
    for (unsigned i = 0; i < sizeof(kQlin) / sizeof(kQlin[0]); i++) {
        const uint64_t vco = (uint64_t)khz * kQlin[i].divider;
        if (vco < 614400)
            continue;
        if (vco > 1200000 || vco % kComparisonKHz || vco / kComparisonKHz > 255)
            return 0;
        return (kQlin[i].code << 20) | ((uint32_t)(vco / kComparisonKHz) << 10) | 2u;
    }
    return 0;
}

struct switch_engine_clock_plan {
    uint32_t khz;       // the engine's rate
    uint32_t pll_base;  // 0: PLLP
};

static inline struct switch_engine_clock_plan switch_engine_clock_plan_for(unsigned bus, int32_t soc_speedo)
{
    const uint32_t khz = switch_engine_vmin_khz(bus, switch_engine_soc_process(soc_speedo));
    const uint32_t base = khz > SWITCH_ENGINE_PLLP_KHZ ? switch_engine_pllcx_base(khz) : 0;
    struct switch_engine_clock_plan plan = {base ? khz : SWITCH_ENGINE_PLLP_KHZ, base};
    return plan;
}

#ifdef __cplusplus
}
#endif

#endif
