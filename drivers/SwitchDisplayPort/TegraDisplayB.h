// SPDX-License-Identifier: GPL-2.0-only
// Display controller B (0x54240000, SPI 74 level-high) driving SOR1: its power partition and clock, the mode's timing,
// and windows A, B and C scanning surfaces by DMA through the SMMU (swgroup dcb), switched together at a frame end.
#ifndef TEGRA_DISPLAY_B_H
#define TEGRA_DISPLAY_B_H

#include <stddef.h>
#include <stdint.h>

#include "SwitchDPModes.h"
#include "SwitchExternalWindows.h"
#include "../SwitchPower/SwitchDisplayMemory.h"

#define TEGRA_DISPLAY_B_INTID (32u + 74u)

class TegraDisplayB {
public:
    using Frame = switch_external::Frame;
    // The DISB partition with DISP2 (already sourced from pll_d2_out0) and the controller's base configuration.
    bool powerUp(char *why, size_t size);
    void setMode(const struct switch_dp_mode &mode);
    void enableSor(bool on);
    // Every window's registers for the next frame boundary, in one request; a CSC written under a window that may be
    // scanning panics.
    void present(const Frame &frame);
    // At a frame end after present: false while the request is out, then true with every window it named scanning what it
    // was given (its START_ADDR, or off); anything else panics.
    bool latch();
    switch_external::WindowState windowState(unsigned window) const;

    uint32_t pixelClockHz() const
    {
        return memory.pixel_clock_hz;
    }

    void windowsOff();
    void frameInterrupt(bool enable);
    uint32_t takeInterrupt();
    void powerDown();

private:
    struct WindowCache {
        switch_external::WindowRegisters written;  // the assembly copy, while valid
        bool valid;
        bool live;  // may be scanning
        bool expectOn;
        uint32_t expectStart;
    };

    struct FrameMemory {
        switch_display_memory_window windows[switch_external::kWindows];
        uint64_t peakBytes;
    };

    volatile uint8_t *base = nullptr;
    switch_display_memory_head memory = {};  // the timing, and the windows' plan as last asked of the EMC
    FrameMemory latchedMemory = {}, requestedMemory = {};
    WindowCache windows[switch_external::kWindows] = {};
    int csc[switch_external::kWindows][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
    uint32_t requested = 0;
    bool frameInterruptEnabled = false, emcRefusedLogged = false, memoryPending = false;
    void reserveMemory(const FrameMemory &next);
    void releaseBandwidth();
    void forgetWindows();
    void select(unsigned window);
    void writeWindow(unsigned window, const switch_external::WindowRegisters &next);
    uint32_t read(uint32_t word) const;
    void write(uint32_t word, uint32_t value);
    void commit(uint32_t update);
};

#endif
