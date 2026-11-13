// SPDX-License-Identifier: BSD-2-Clause-Patent
// NVDEC's, NVENC's and NVJPG's clock source (SwitchEngineClockPlan.h). The bus PLL is brought up the way L4T's
// tegra_clk_pll_ops bring PLLC2/PLLC3 up from off (tegra210_pllcx_set_defaults, _update_pll_mnp, _clk_pll_enable,
// clk_pll_wait_for_lock): defaults with reset and IDDQ asserted, M/N/P, IDDQ off, reset off, enable, then the lock bit,
// which has no interrupt: a bounded wait of L4T's 300 x 2 us. A PLL found running with other dividers is someone
// else's, and a PLL that does not lock is put back off; either keeps the engines on PLLP with one line. The PLL stays
// on once up. Whether it is up is read from its BASE on every call, so a PLL that SC7 took comes back at the next
// power-up.
#ifndef SWITCH_ENGINE_CLOCK_H
#define SWITCH_ENGINE_CLOCK_H

#include <stdint.h>
#include "SwitchEngineClockPlan.h"

#ifdef __cplusplus
extern "C" {
#endif

// The CLK_SOURCE value (source 31:29, 7.1 divider 7:0 at 1:1) for an engine on `bus`, its PLL locked; *khz its rate.
// Thread context.
uint32_t switch_engine_clock_source(unsigned bus, uint32_t *khz);

#ifdef __cplusplus
}
#endif

#endif
