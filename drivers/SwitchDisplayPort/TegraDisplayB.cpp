// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2018 naehrwert
// Copyright (c) 2018-2025 CTCaer
// Copyright (C) 2012 Avionic Design GmbH
// Copyright (C) 2012 NVIDIA CORPORATION
// Display controller B; see TegraDisplayB.h. kInit is display controller A's base configuration from hekate
// (github.com/CTCaer/hekate, GPL-2.0-only): bdk/display/di.inl _di_dc_init_config, as TegraDisplayPower.cpp has it,
// without the DSI parts. The rest is Linux drivers/gpu/drm/tegra/dc.c and dc.h: tegra_crtc_atomic_enable,
// tegra_dc_set_timings (reference-to-sync 1/1), tegra_dc_commit_state (shift clock 1:1), tegra_dc_setup_window for a
// 1:1 pitch BGRA window, SOR_ENABLE(1) = DC_DISP_DISP_WIN_OPTIONS bit 26. Power: the DISB partition (19), DISP2 clock
// and reset 26, MC client hot reset DCB (MC 0x200 bit 3, Linux memory/tegra/tegra210.c), the MBIST workaround
// (clk-tegra210.c tegra210_generic_mbist_war, LVL2_CLK_GATE_OVRA bit 2).
#include "TegraDisplayB.h"

#include <IOKit/IOLib.h>
#include "../TegraDisplayFlip/SwitchDisplayPriority.h"

#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint64_t kDisplayBPA = 0x54240000, kMcPA = 0x70019000;
constexpr unsigned kClockDisp2 = 26, kPartitionDisb = 19;
constexpr uint32_t kMcHotResetControl = 0x200, kMcHotResetDcb = 1u << 3;
constexpr uint32_t kLvl2ClockGateOverrideA = 0xf8, kOverrideDisb = 1u << 2;

enum : uint32_t {
    DC_CMD_DISPLAY_COMMAND_OPTION0 = 0x031,
    DC_CMD_DISPLAY_COMMAND = 0x032,
    DC_CMD_DISPLAY_POWER_CONTROL = 0x036,
    DC_CMD_INT_STATUS = 0x037,
    DC_CMD_INT_MASK = 0x038,
    DC_CMD_INT_ENABLE = 0x039,
    DC_CMD_STATE_ACCESS = 0x040,
    DC_CMD_STATE_CONTROL = 0x041,
    DC_CMD_DISPLAY_WINDOW_HEADER = 0x042,
    DC_CMD_REG_ACT_CONTROL = 0x043,
    DC_COM_CRC_CONTROL = 0x300,
    DC_COM_PIN_OUTPUT_POLARITY1 = 0x307,
    DC_COM_PIN_OUTPUT_POLARITY3 = 0x309,
    DC_DISP_DISP_WIN_OPTIONS = 0x402,
    DC_DISP_DISP_MEM_HIGH_PRIORITY = 0x403,
    DC_DISP_DISP_MEM_HIGH_PRIORITY_TIMER = 0x404,
    DC_DISP_REF_TO_SYNC = 0x406,
    DC_DISP_SYNC_WIDTH = 0x407,
    DC_DISP_BACK_PORCH = 0x408,
    DC_DISP_ACTIVE = 0x409,
    DC_DISP_FRONT_PORCH = 0x40a,
    DC_DISP_DISP_CLOCK_CONTROL = 0x42e,
    DC_DISP_DISP_INTERFACE_CONTROL = 0x42f,
    DC_DISP_DISP_COLOR_CONTROL = 0x430,
    DC_DISP_DC_MCCIF_FIFOCTRL = 0x480,
    DC_DISP_BLEND_BACKGROUND_COLOR = 0x4e4,
    DC_WIN_WIN_OPTIONS = 0x700,
    DC_WIN_BYTE_SWAP = 0x701,
    DC_WIN_COLOR_DEPTH = 0x703,
    DC_WIN_POSITION = 0x704,
    DC_WIN_SIZE = 0x705,
    DC_WIN_PRESCALED_SIZE = 0x706,
    DC_WIN_H_INITIAL_DDA = 0x707,
    DC_WIN_V_INITIAL_DDA = 0x708,
    DC_WIN_DDA_INC = 0x709,
    DC_WIN_LINE_STRIDE = 0x70a,
    DC_WIN_BUFFER_ADDR_MODE = 0x70d,
    DC_WIN_DV_CONTROL = 0x70e,
    DC_WINBUF_BLEND_LAYER_CONTROL = 0x716,
    DC_WINBUF_START_ADDR = 0x800,
    DC_WINBUF_ADDR_H_OFFSET = 0x806,
    DC_WINBUF_ADDR_V_OFFSET = 0x808,
    DC_WINBUF_SURFACE_KIND = 0x80b,
};

constexpr uint32_t kGeneralActReq = 1u << 0, kWinAActReq = 1u << 1, kGeneralUpdate = 1u << 8;
constexpr uint32_t kFrameEnd = 1u << 1, kContinuousDisplay = 1u << 5, kSor1Enable = 1u << 26;
constexpr uint32_t kPowerAll = 0x50155;  // PW0..PW4, PM0, PM1
constexpr uint32_t kWindowA = 1u << 4, kWinEnable = 1u << 30, kB8G8R8A8 = 12, kDdaOne = 0x1000;

struct Reg {
    uint32_t index, value;
};

constexpr Reg kInit[] = {
    {DC_CMD_STATE_ACCESS, 0},
    {DC_CMD_REG_ACT_CONTROL, 0x454},
    {DC_CMD_STATE_CONTROL, kGeneralUpdate},
    {DC_CMD_STATE_CONTROL, kGeneralActReq},
    {DC_DISP_DC_MCCIF_FIFOCTRL, 0},
    {DC_DISP_DISP_MEM_HIGH_PRIORITY, SwitchDisplayPriority::kThresholds},
    {DC_DISP_DISP_MEM_HIGH_PRIORITY_TIMER, SwitchDisplayPriority::kDelays},
    {DC_CMD_DISPLAY_WINDOW_HEADER, 0xf0},
    {DC_WIN_WIN_OPTIONS, 0},
    {DC_WIN_DV_CONTROL, 0},
    {DC_DISP_DISP_COLOR_CONTROL, 8},
    {DC_DISP_DISP_INTERFACE_CONTROL, 0},
    {DC_COM_PIN_OUTPUT_POLARITY1, 0},
    {DC_COM_PIN_OUTPUT_POLARITY3, 0},
    {DC_DISP_BLEND_BACKGROUND_COLOR, 0},
    {DC_COM_CRC_CONTROL, 0},
    {DC_WINBUF_BLEND_LAYER_CONTROL, (1u << 24) | 255},
    {DC_CMD_DISPLAY_COMMAND_OPTION0, 0},
    {DC_DISP_DISP_WIN_OPTIONS, 0},
    {DC_CMD_DISPLAY_COMMAND, 0},
    {DC_CMD_STATE_CONTROL, 0x1f00},
    {DC_CMD_STATE_CONTROL, 0x1f},
};
}  // namespace

uint32_t TegraDisplayB::read(uint32_t word) const
{
    return switch_mmio_read32(base, word * 4u);
}

void TegraDisplayB::write(uint32_t word, uint32_t value)
{
    switch_mmio_write32(base, word * 4u, value);
}

void TegraDisplayB::commit(uint32_t update)
{
    write(DC_CMD_STATE_CONTROL, update << 8);
    write(DC_CMD_STATE_CONTROL, update);
}

bool TegraDisplayB::powerUp(char *why, size_t size)
{
    if (base == nullptr) {
        base = switch_mmio_map(kDisplayBPA, 0x40000);
    }
    if (!switch_pmc_power_up(kPartitionDisb, kClockDisp2, kClockDisp2)) {
        snprintf(why, size, "the DISB power partition did not come up");
        return false;
    }
    // tegra210_generic_mbist_war for DISB: the partition's RAMs initialise only with their second-level clock gate
    // overridden for a moment after the partition comes up.
    switch_car_rmw(kLvl2ClockGateOverrideA, 0, kOverrideDisb);
    IODelay(1);
    switch_car_rmw(kLvl2ClockGateOverrideA, kOverrideDisb, 0);
    IODelay(1);
    volatile uint8_t *mc = switch_mmio_map(kMcPA, 0x1000);
    switch_mmio_rmw32(mc, kMcHotResetControl, kMcHotResetDcb, 0);
    for (const Reg &reg : kInit) {
        write(reg.index, reg.value);
    }
    return true;
}

void TegraDisplayB::setMode(const struct switch_dp_mode &mode)
{
    write(DC_DISP_DISP_CLOCK_CONTROL, 0);
    write(DC_DISP_REF_TO_SYNC, (1u << 16) | 1u);
    write(DC_DISP_SYNC_WIDTH, ((uint32_t)mode.vsync << 16) | mode.hsync);
    write(DC_DISP_BACK_PORCH, ((uint32_t)mode.vback << 16) | mode.hback);
    write(DC_DISP_FRONT_PORCH, ((uint32_t)mode.vfront << 16) | mode.hfront);
    write(DC_DISP_ACTIVE, ((uint32_t)mode.vactive << 16) | mode.hactive);
    write(DC_CMD_INT_STATUS, ~0u);
    write(DC_CMD_INT_ENABLE, 0);
    write(DC_CMD_INT_MASK, kFrameEnd);
    write(DC_CMD_DISPLAY_COMMAND, kContinuousDisplay);
    write(DC_CMD_DISPLAY_POWER_CONTROL, kPowerAll);
    commit(kGeneralActReq);
}

void TegraDisplayB::enableSor(bool on)
{
    const uint32_t options = read(DC_DISP_DISP_WIN_OPTIONS);
    write(DC_DISP_DISP_WIN_OPTIONS, on ? (options | kSor1Enable) : (options & ~kSor1Enable));
    commit(kGeneralActReq);
}

void TegraDisplayB::scanout(uint32_t iova, uint32_t width, uint32_t height, uint32_t stride)
{
    write(DC_CMD_DISPLAY_WINDOW_HEADER, kWindowA);
    write(DC_WIN_COLOR_DEPTH, kB8G8R8A8);
    write(DC_WIN_BYTE_SWAP, 0);
    write(DC_WIN_POSITION, 0);
    write(DC_WIN_SIZE, (height << 16) | width);
    write(DC_WIN_PRESCALED_SIZE, (height << 16) | (width * 4u));
    write(DC_WIN_H_INITIAL_DDA, 0);
    write(DC_WIN_V_INITIAL_DDA, 0);
    write(DC_WIN_DDA_INC, (kDdaOne << 16) | kDdaOne);
    write(DC_WIN_LINE_STRIDE, stride);
    write(DC_WIN_BUFFER_ADDR_MODE, 0);
    write(DC_WINBUF_SURFACE_KIND, 0);
    write(DC_WINBUF_START_ADDR, iova);
    write(DC_WINBUF_ADDR_H_OFFSET, 0);
    write(DC_WINBUF_ADDR_V_OFFSET, 0);
    write(DC_WIN_WIN_OPTIONS, kWinEnable);
    write(DC_CMD_INT_ENABLE, kFrameEnd);
    commit(kWinAActReq);
}

void TegraDisplayB::windowOff()
{
    write(DC_CMD_DISPLAY_WINDOW_HEADER, kWindowA);
    write(DC_WIN_WIN_OPTIONS, 0);
    commit(kWinAActReq);
}

uint32_t TegraDisplayB::takeInterrupt()
{
    const uint32_t cause = read(DC_CMD_INT_STATUS);
    write(DC_CMD_INT_STATUS, cause);
    write(DC_CMD_INT_ENABLE, 0);
    return cause;
}

void TegraDisplayB::powerDown()
{
    if (base == nullptr || !switch_car_clock_enabled(kClockDisp2)) {
        return;
    }
    write(DC_CMD_INT_ENABLE, 0);
    write(DC_CMD_INT_MASK, 0);
    write(DC_CMD_DISPLAY_COMMAND, 0);
    write(DC_CMD_DISPLAY_POWER_CONTROL, 0);
    commit(kGeneralActReq);
    switch_car_reset_assert(kClockDisp2);
    switch_car_clock_disable(kClockDisp2);
    (void)switch_pmc_partition_off(kPartitionDisb);
}
