// SPDX-License-Identifier: GPL-2.0-only
// DPAUX1, the AUX channel and hot-plug input of SOR1 (0x54040000, SPI 11 level-high), for SwitchDisplayPort's work
// loop: one AUX transaction at a time, finished by AUX_DONE; HPD plug, unplug and IRQ_HPD as interrupts. The BM92T36
// drives the HPD line (PCC1, DPAUX1's HPD input) from the dock's Attention messages.
#ifndef TEGRA_DPAUX1_H
#define TEGRA_DPAUX1_H

#include <stddef.h>
#include <stdint.h>

#include "SwitchDPAux.h"

#define TEGRA_DPAUX1_INTID (32u + 11u)

class TegraDpaux1 {
public:
    // Clocks (DPAUX1, SOR_SAFE, PLL_DP at 270 MHz), reset, pads in AUX mode, HPD debounce, every interrupt but none
    // pending. False with one line in `why` when a clock or PLL does not come up.
    bool powerUp(char *why, size_t size);
    void powerDown();
    void setDownspread(bool enabled);

    bool hpdHigh() const;

    // Starts `request`; AUX_DONE ends it (finish()). The caller keeps one in flight.
    void start(const struct switch_dp_aux_request &request);
    // After AUX_DONE: the reply, and a read's bytes into request.data.
    enum switch_dp_aux_result finish(struct switch_dp_aux_request &request);
    void resetAux();

    // The interrupt's cause, cleared (the line is level: reading and clearing INTR_AUX drops it).
    uint32_t takeInterrupt();

    uint32_t auxstat() const;

private:
    volatile uint8_t *base = nullptr, *pinmux = nullptr;
    uint32_t read(uint32_t word) const;
    void write(uint32_t word, uint32_t value);
    bool startPllDp(char *why, size_t size);
};

#endif
