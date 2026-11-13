// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2018 naehrwert
// Copyright (c) 2018-2025 CTCaer
// The register tables are taken from hekate (github.com/CTCaer/hekate, GPL-2.0-only): bdk/display/di.inl, with the
// register names of bdk/display/di.h.
//
// Cold power-up and power-down of the Switch OLED display pipeline around SC7; see TegraDisplayPower.h.
// Register indices are 32-bit word indices as in hekate bdk/display/di.h (DISPLAY_A, DSI and MIPI_CAL are base + index * 4);
// CAR, PMC and APB_MISC offsets are bytes (hekate bdk/soc/clock.h, pmc_t210.h, t210.h, pinmux.h). Tables are hekate's
// bdk/display/di.inl, T210B01 variants, in the same order.
#include <IOKit/IOLib.h>
#include "../TegraDisplayFlip/SwitchDisplayPriority.h"
#include "TegraDisplayPower.h"
#include "../SwitchSuspend/SwitchSleepSteps.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchMMIO/SwitchMMIOWords.h"

namespace {
constexpr vm_offset_t kCarPA = 0x60006000, kDisplayAPA = 0x54200000, kDsiPA = 0x54300000, kMipiCalPA = 0x700E3000;
constexpr vm_offset_t kApbMiscPA = 0x70000000, kPmcPagePA = 0x7000E000;  // pinmux at +0x3000, PMC at +0x400 of its page

struct Reg {
    uint32_t index, value;
};

// Clock and reset controller (bytes). DISP1 is L27, HOST1X L28, DSI H16, MIPI_CAL H24, UART_FST_MIPI_CAL X17, DSIA_LP W19.
enum : uint32_t {
    RST_DEVICES_L = 0x004,
    CLK_OUT_ENB_L = 0x010,
    PLLD_BASE = 0x0D0,
    PLLD_MISC1 = 0x0D8,
    PLLD_MISC = 0x0DC,
    CLK_SOURCE_DISP1 = 0x138,
    CLK_SOURCE_HOST1X = 0x180,
    CLK_ENB_X_SET = 0x284,
    RST_DEV_L_SET = 0x300,
    RST_DEV_L_CLR = 0x304,
    RST_DEV_H_SET = 0x308,
    RST_DEV_H_CLR = 0x30C,
    CLK_ENB_L_SET = 0x320,
    CLK_ENB_L_CLR = 0x324,
    CLK_ENB_H_SET = 0x328,
    CLK_ENB_H_CLR = 0x32C,
    CLK_ENB_W_SET = 0x448,
    CLK_SOURCE_DSIA_LP = 0x620,
    CLK_SOURCE_UART_FST_MIPI_CAL = 0x66C,
};

constexpr uint32_t kDisp1 = 1u << 27, kHost1x = 1u << 28, kDsi = 1u << 16, kMipiCal = 1u << 24;
constexpr uint32_t kUartFstMipiCal = 1u << 17, kDsiaLp = 1u << 19;
constexpr uint32_t kDiv6 = 10, kDiv2p5 = 3;  // CLK_SRC_DIV(6), CLK_SRC_DIV(2.5)
constexpr uint32_t kPllEnable = 1u << 30, kPllLock = 1u << 27;

// PMC (bytes from 0x7000E400). IO_DPD*_REQ code 1 = DPD off; DSI is DPD bit 2, DSIB DPD2 bit 7 (Linux tegra210_io_pads).
enum : uint32_t {
    IO_DPD_STATUS = 0x1BC,
    IO_DPD2_STATUS = 0x1C4,
    SECURE_SCRATCH113 = 0xB1C
};

constexpr unsigned kDpdDsiPad = 2, kDpdDsibPad = 32 + 7;  // Linux tegra210 io_pads dsi, dsib .dpd

// APB_MISC (bytes): GP_DSI_PAD_CONTROL (T210B01 only), PINMUX_AUX_LCD_RST (pull-down, function 0).
constexpr uint32_t kGpDsiPadControl = 0xAC0, kPinmuxLcdRst = 0x3000 + 0x204, kPinmuxPullDown = 1u << 2;
constexpr unsigned kPanelReset = TEGRA_GPIO(V, 2);

// Display controller A (words).
enum : uint32_t {
    DC_CMD_GENERAL_INCR_SYNCPT_CNTRL = 0x001,
    DC_CMD_CONT_SYNCPT_VSYNC = 0x028,
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
    DC_COM_PIN_OUTPUT_ENABLE1 = 0x303,
    DC_COM_PIN_OUTPUT_POLARITY1 = 0x307,
    DC_COM_PIN_OUTPUT_POLARITY3 = 0x309,
    DC_DISP_DISP_WIN_OPTIONS = 0x402,
    DC_DISP_DISP_MEM_HIGH_PRIORITY = 0x403,
    DC_DISP_DISP_MEM_HIGH_PRIORITY_TIMER = 0x404,
    DC_DISP_DISP_TIMING_OPTIONS = 0x405,
    DC_DISP_REF_TO_SYNC = 0x406,
    DC_DISP_SYNC_WIDTH = 0x407,
    DC_DISP_BACK_PORCH = 0x408,
    DC_DISP_ACTIVE = 0x409,
    DC_DISP_FRONT_PORCH = 0x40A,
    DC_DISP_DISP_CLOCK_CONTROL = 0x42E,
    DC_DISP_DISP_INTERFACE_CONTROL = 0x42F,
    DC_DISP_DISP_COLOR_CONTROL = 0x430,
    DC_DISP_SHIFT_CLOCK_OPTIONS = 0x431,
    DC_DISP_DATA_ENABLE_OPTIONS = 0x432,
    DC_DISP_DC_MCCIF_FIFOCTRL = 0x480,
    DC_DISP_BLEND_BACKGROUND_COLOR = 0x4E4,
    DC_WINC_CSC_YOF = 0x611,
    DC_WIN_WIN_OPTIONS = 0x700,
    DC_WIN_DV_CONTROL = 0x70E,
    DC_WINBUF_BLEND_LAYER_CONTROL = 0x716,
};

constexpr uint32_t kGeneralActReq = 1u << 0, kGeneralUpdate = 1u << 8, kAllActReq = 0x1F, kAllUpdate = 0x1F00;

// DSI A (words).
enum : uint32_t {
    DSI_WR_DATA = 0x0A,
    DSI_POWER_CONTROL = 0x0B,
    DSI_INT_ENABLE = 0x0C,
    DSI_INT_STATUS = 0x0D,
    DSI_INT_MASK = 0x0E,
    DSI_HOST_CONTROL = 0x0F,
    DSI_CONTROL = 0x10,
    DSI_SOL_DELAY = 0x11,
    DSI_MAX_THRESHOLD = 0x12,
    DSI_TRIGGER = 0x13,
    DSI_TX_CRC = 0x14,
    DSI_INIT_SEQ_CONTROL = 0x1A,
    DSI_INIT_SEQ_DATA_0 = 0x1B,
    DSI_PKT_SEQ_0_LO = 0x23,
    DSI_DCS_CMDS = 0x33,
    DSI_PKT_LEN_0_1 = 0x34,
    DSI_PHY_TIMING_0 = 0x3C,
    DSI_PHY_TIMING_1 = 0x3D,
    DSI_PHY_TIMING_2 = 0x3E,
    DSI_BTA_TIMING = 0x3F,
    DSI_TIMEOUT_0 = 0x44,
    DSI_TIMEOUT_1 = 0x45,
    DSI_TO_TALLY = 0x46,
    DSI_PAD_CONTROL_0 = 0x4B,
    DSI_PAD_CONTROL_CD = 0x4C,
    DSI_VIDEO_MODE_CONTROL = 0x4E,
    DSI_PAD_CONTROL_1 = 0x4F,
    DSI_INIT_SEQ_DATA_15_B01 = 0x62,
};

constexpr uint32_t kDsiTriggerHost = 1u << 1;
constexpr uint32_t kDsiPadsDown =
    (1u << 24) | (0xFu << 16) | (1u << 8) | 0xFu;  // VS1_PULLDN_CLK|PULLDN(F)|PDIO_CLK|PDIO(F)

// MIPI pad calibration (words).
enum : uint32_t {
    MIPI_CAL_CTRL = 0x00,
    MIPI_CAL_CIL_STATUS = 0x02,
    MIPI_CAL_BIAS_PAD_CFG0 = 0x16,
    MIPI_CAL_BIAS_PAD_CFG1 = 0x17,
    MIPI_CAL_BIAS_PAD_CFG2 = 0x18,
};

// _di_dc_init_config: assembly access, line-counter latching, pipeline power, syncpoints, all windows off, YUV CSC defaults,
// 24-bit DSI output, black background, display stopped.
constexpr Reg kDcInit[] = {
    {DC_CMD_STATE_ACCESS, 0},
    {DC_CMD_REG_ACT_CONTROL, 0x454},
    {DC_CMD_STATE_CONTROL, kGeneralUpdate},
    {DC_CMD_STATE_CONTROL, kGeneralActReq},
    {DC_DISP_DC_MCCIF_FIFOCTRL, 0},
    {DC_DISP_DISP_MEM_HIGH_PRIORITY, SwitchDisplayPriority::kThresholds},
    {DC_DISP_DISP_MEM_HIGH_PRIORITY_TIMER, SwitchDisplayPriority::kDelays},
    {DC_CMD_DISPLAY_POWER_CONTROL, 0x50155},
    {DC_CMD_GENERAL_INCR_SYNCPT_CNTRL, 1u << 8},
    {DC_CMD_CONT_SYNCPT_VSYNC, (1u << 8) | 9},
    {DC_CMD_STATE_CONTROL, kAllUpdate},
    {DC_CMD_STATE_CONTROL, kAllActReq},
    {DC_CMD_DISPLAY_WINDOW_HEADER, 0xF0},
    {DC_WIN_WIN_OPTIONS, 0},
    {DC_WIN_DV_CONTROL, 0},
    {DC_WINC_CSC_YOF + 0, 0xF0},
    {DC_WINC_CSC_YOF + 1, 0x12A},
    {DC_WINC_CSC_YOF + 2, 0},
    {DC_WINC_CSC_YOF + 3, 0x198},
    {DC_WINC_CSC_YOF + 4, 0x39B},
    {DC_WINC_CSC_YOF + 5, 0x32F},
    {DC_WINC_CSC_YOF + 6, 0x204},
    {DC_WINC_CSC_YOF + 7, 0},
    {DC_DISP_DISP_COLOR_CONTROL, 8},
    {DC_DISP_DISP_INTERFACE_CONTROL, 0},
    {DC_COM_PIN_OUTPUT_POLARITY1, 1u << 24},
    {DC_COM_PIN_OUTPUT_POLARITY3, 0},
    {DC_DISP_BLEND_BACKGROUND_COLOR, 0},
    {DC_COM_CRC_CONTROL, 0},
    {DC_CMD_STATE_CONTROL, kAllUpdate},
    {DC_CMD_STATE_CONTROL, kAllActReq},
    {DC_WINBUF_BLEND_LAYER_CONTROL, (1u << 24) | 255},
    {DC_CMD_DISPLAY_COMMAND_OPTION0, 0},
    {DC_DISP_DISP_WIN_OPTIONS, 0},
    {DC_CMD_DISPLAY_COMMAND, 0},
    {DC_CMD_STATE_CONTROL, kAllUpdate},
    {DC_CMD_STATE_CONTROL, kAllActReq},
};
// _di_dsi_seq_pkt_reset_config0, then DSI_INIT_SEQ_DATA_15_B01 = 0, then _di_dsi_seq_pkt_reset_config1.
constexpr Reg kDsiReset0[] = {
    {DSI_WR_DATA, 0},
    {DSI_INT_ENABLE, 0},
    {DSI_INT_STATUS, 0},
    {DSI_INT_MASK, 0},
    {DSI_INIT_SEQ_DATA_0 + 0, 0},
    {DSI_INIT_SEQ_DATA_0 + 1, 0},
    {DSI_INIT_SEQ_DATA_0 + 2, 0},
    {DSI_INIT_SEQ_DATA_0 + 3, 0},
    {DSI_INIT_SEQ_DATA_15_B01, 0},
};
constexpr Reg kDsiReset1[] = {
    {DSI_DCS_CMDS, 0},
    {DSI_PKT_SEQ_0_LO + 0, 0},
    {DSI_PKT_SEQ_0_LO + 2, 0},
    {DSI_PKT_SEQ_0_LO + 4, 0},
    {DSI_PKT_SEQ_0_LO + 6, 0},
    {DSI_PKT_SEQ_0_LO + 8, 0},
    {DSI_PKT_SEQ_0_LO + 10, 0},
    {DSI_PKT_SEQ_0_LO + 1, 0},
    {DSI_PKT_SEQ_0_LO + 3, 0},
    {DSI_PKT_SEQ_0_LO + 5, 0},
    {DSI_PKT_SEQ_0_LO + 7, 0},
    {DSI_PKT_SEQ_0_LO + 9, 0},
    {DSI_PKT_SEQ_0_LO + 11, 0},
    {DSI_CONTROL, 0},
    // _di_dsi_init_pads_t210b01: DSI_PAD_CONTROL_1..4, 5_B01..7_B01.
    {DSI_PAD_CONTROL_1 + 0, 0},
    {DSI_PAD_CONTROL_1 + 1, 0},
    {DSI_PAD_CONTROL_1 + 2, 0},
    {DSI_PAD_CONTROL_1 + 3, 0},
    {DSI_PAD_CONTROL_1 + 4, 0},
    {DSI_PAD_CONTROL_1 + 5, 0},
    {DSI_PAD_CONTROL_1 + 6, 0},
};
// _di_dsi_init_config: PHY timings at 19.2 MHz, timeouts, pads powered, DSI enabled.
constexpr Reg kDsiInit[] = {
    {DSI_PAD_CONTROL_CD, 0},
    {DSI_SOL_DELAY, 24},
    {DSI_MAX_THRESHOLD, 480},
    {DSI_TRIGGER, 0},
    {DSI_INIT_SEQ_CONTROL, 0},
    {DSI_PKT_LEN_0_1 + 0, 0},
    {DSI_PKT_LEN_0_1 + 1, 0},
    {DSI_PKT_LEN_0_1 + 2, 0},
    {DSI_PKT_LEN_0_1 + 3, 0},
    {DSI_PAD_CONTROL_1, 0},
    {DSI_PHY_TIMING_0, 0x6070603},
    {DSI_PHY_TIMING_1, 0x40A0E05},
    {DSI_PHY_TIMING_2, 0x30109},
    {DSI_BTA_TIMING, 0x190A14},
    {DSI_TIMEOUT_0, 0x2000FFFF},
    {DSI_TIMEOUT_1, 0x07652000},
    {DSI_TO_TALLY, 0},
    {DSI_PAD_CONTROL_0, 0},
    {DSI_POWER_CONTROL, 1},
};
// _di_dsi_timing_lp_config: LP PHY timings, host-triggered transfers on 4 lanes.
constexpr Reg kDsiLp[] = {
    {DSI_PAD_CONTROL_1, 0},
    {DSI_PHY_TIMING_0, 0x6070603},
    {DSI_PHY_TIMING_1, 0x40A0E05},
    {DSI_PHY_TIMING_2, 0x30118},
    {DSI_BTA_TIMING, 0x190A14},
    {DSI_TIMEOUT_0, 0x2000FFFF},
    {DSI_TIMEOUT_1, 0x13432000},
    {DSI_TO_TALLY, 0},
    {DSI_HOST_CONTROL, 0x102003},
    {DSI_CONTROL, 0x31},
    {DSI_POWER_CONTROL, 1},
    {DSI_MAX_THRESHOLD, 64},
    {DSI_TRIGGER, 0},
    {DSI_TX_CRC, 0},
    {DSI_INIT_SEQ_CONTROL, 0},
};
// _di_dsi_seq_pkt_video_non_burst_no_eot_config: HS PHY timings at 234 MHz and the video packet sequence.
constexpr Reg kDsiVideo[] = {
    {DSI_PAD_CONTROL_1, 0},
    {DSI_PHY_TIMING_0, 0x6070603},
    {DSI_PHY_TIMING_1, 0x40A0E05},
    {DSI_PHY_TIMING_2, 0x30172},
    {DSI_BTA_TIMING, 0x190A14},
    {DSI_TIMEOUT_0, 0x20000A40},
    {DSI_TIMEOUT_1, 0x5A2F2000},
    {DSI_TO_TALLY, 0},
    {DSI_PKT_SEQ_0_LO + 0, 0x40000208},
    {DSI_PKT_SEQ_0_LO + 4, 0x40000308},
    {DSI_PKT_SEQ_0_LO + 8, 0x40000308},
    {DSI_PKT_SEQ_0_LO + 2, 0x40000308},
    {DSI_PKT_SEQ_0_LO + 6, 0x3F3B2B08},
    {DSI_PKT_SEQ_0_LO + 7, 0x2CC},
    {DSI_PKT_SEQ_0_LO + 10, 0x3F3B2B08},
    {DSI_PKT_SEQ_0_LO + 11, 0x2CC},
    {DSI_PKT_LEN_0_1 + 0, 206u << 16},
    {DSI_PKT_LEN_0_1 + 1, (2160u << 16) | 418},
    {DSI_PKT_LEN_0_1 + 2, 400},
    {DSI_PKT_LEN_0_1 + 3, 400},
    {DSI_HOST_CONTROL, 0},
};
// _di_dsi_host_mode_config: 24-bit video on 4 lanes, HS clock, start-of-line triggered.
constexpr Reg kDsiHostMode[] = {
    {DSI_TRIGGER, 0},        {DSI_CONTROL, 0},         {DSI_SOL_DELAY, 6},       {DSI_MAX_THRESHOLD, 480},
    {DSI_POWER_CONTROL, 1},  {DSI_CONTROL, 0x103032},  {DSI_HOST_CONTROL, 0x33}, {DSI_CONTROL, 0x103032},
    {DSI_HOST_CONTROL, 0x3}, {DSI_HOST_CONTROL, 0x23},
};
// _di_mipi_pad_cal_config.
constexpr Reg kMipiPadCal[] = {
    {MIPI_CAL_BIAS_PAD_CFG2, 0},
    {MIPI_CAL_CIL_STATUS, 0xF3F10000},
    {MIPI_CAL_BIAS_PAD_CFG0, 0},
    {MIPI_CAL_BIAS_PAD_CFG2, 0},
};
// _di_dsi_pad_cal_config_t210b01 (DSI words).
constexpr Reg kDsiPadCal[] = {
    {DSI_PAD_CONTROL_1 + 0, 0},       {DSI_PAD_CONTROL_1 + 1, 0},       {DSI_PAD_CONTROL_1 + 2, 0},
    {DSI_PAD_CONTROL_1 + 3, 0x77777}, {DSI_PAD_CONTROL_1 + 4, 0x77777}, {DSI_PAD_CONTROL_1 + 5, 0x1111},
    {DSI_PAD_CONTROL_1 + 6, 0},
};
// _di_mipi_dsi_cal_prod_config_t210b01, then _di_mipi_dsi_cal_unused_config (which clears DSIB_CONFIG_2 again, as hekate does).
constexpr Reg kMipiDsiCal[] = {
    {0x38 / 4, 0x200006}, {0x3C / 4, 0x200006}, {0x64 / 4, 0x260000}, {0x68 / 4, 0x260000}, {0x14 / 4, 0},
    {0x18 / 4, 0},        {0x1C / 4, 0},        {0x20 / 4, 0},        {0x24 / 4, 0},        {0x28 / 4, 0},
    {0x40 / 4, 0},        {0x44 / 4, 0},        {0x68 / 4, 0},        {0x70 / 4, 0},        {0x74 / 4, 0},
};
// _di_dc_video_mode_config up to (not including) the start of continuous display: 720x1280 timings, DSI data enable.
constexpr Reg kDcTimings[] = {
    {DC_DISP_DISP_TIMING_OPTIONS, 0},       {DC_DISP_REF_TO_SYNC, 0x10000}, {DC_DISP_SYNC_WIDTH, 0x10048},
    {DC_DISP_BACK_PORCH, 0x90048},          {DC_DISP_FRONT_PORCH, 0xA0088}, {DC_DISP_ACTIVE, (1280u << 16) | 720},
    {DC_DISP_SHIFT_CLOCK_OPTIONS, 0x10001}, {DC_COM_PIN_OUTPUT_ENABLE1, 0}, {DC_DISP_DATA_ENABLE_OPTIONS, 5},
};
// _di_dc_video_disable_config without the syncpoint writes: no interrupts, stopped, window and memory power off.
constexpr Reg kDcDisable[] = {
    {DC_CMD_INT_MASK, 0},
    {DC_CMD_STATE_ACCESS, 0},
    {DC_CMD_INT_ENABLE, 0},
    {DC_CMD_CONT_SYNCPT_VSYNC, 0},
    {DC_CMD_DISPLAY_COMMAND, 0},
    {DC_CMD_STATE_CONTROL, kGeneralUpdate},
    {DC_CMD_STATE_CONTROL, kGeneralActReq},
    {DC_CMD_STATE_CONTROL, kGeneralUpdate},
    {DC_CMD_STATE_CONTROL, kGeneralActReq},
    {DC_CMD_DISPLAY_POWER_CONTROL, 0},
    {DC_CMD_STATE_CONTROL, kGeneralUpdate},
    {DC_CMD_STATE_CONTROL, kGeneralActReq},
    {DC_CMD_INT_STATUS, 0xFFFFFFFF},
};

SwitchMMIOWords gCar, gDc, gDsi, gMipiCal, gApbMisc, gPmc;

template <size_t N> void write(SwitchMMIOWords base, const Reg (&table)[N])
{
    for (const Reg &reg : table)
        base[reg.index] = reg.value;
}

SwitchMMIOWord car(uint32_t offset)
{
    return gCar[offset / 4];
}

SwitchMMIOWord pmc(uint32_t offset)
{
    return gPmc[(0x400 + offset) / 4];
}

void map()
{
    if (gCar)
        return;
    gCar = SwitchMMIOWords(switch_mmio_map(kCarPA, 0x1000));
    gDc = SwitchMMIOWords(switch_mmio_map(kDisplayAPA, 0x4000));
    gDsi = SwitchMMIOWords(switch_mmio_map(kDsiPA, 0x1000));
    gMipiCal = SwitchMMIOWords(switch_mmio_map(kMipiCalPA, 0x1000));
    gApbMisc = SwitchMMIOWords(switch_mmio_map(kApbMiscPA, 0x4000));
    gPmc = SwitchMMIOWords(switch_mmio_map(kPmcPagePA, 0x1000));
    if (!gCar || !gDc || !gDsi || !gMipiCal || !gApbMisc || !gPmc)
        panic("TegraDisplay: cannot map the display registers");
}

// hekate clock_enable_plld (T210B01: no PLLD_MISC1 setup, SDM_DIN -1024): DISP1 from PLLD_OUT0, then wait up to 1 ms for lock.
void setPlld(uint32_t divp, uint32_t divn)
{
    car(CLK_SOURCE_DISP1) = 2u << 29;
    car(PLLD_BASE) = kPllEnable | kPllLock | (divp << 20) | (divn << 11) | 1;
    car(PLLD_MISC1) = 0;
    car(PLLD_MISC) = (1u << 21) | (1u << 19) | (1u << 18) | (1u << 16) | 0xFC00;
    if (!switch_car_wait(PLLD_BASE, kPllLock, kPllLock, 1000)) {
        panic("TegraDisplay: PLLD (P %u N %u) did not lock: BASE %#x MISC %#x", divp, divn,
              static_cast<uint32_t>(car(PLLD_BASE)), static_cast<uint32_t>(car(PLLD_MISC)));
    }
    IODelay(2);
}

// hekate _display_dsi_send_cmd in LP mode: one packet word, host trigger, then the panel's settle time.
void sendLp(uint32_t word, unsigned ms = 0)
{
    gDsi[DSI_WR_DATA] = word;
    gDsi[DSI_TRIGGER] = kDsiTriggerHost;
    if (ms)
        IOSleep(ms);
}

void sendLp(const uint32_t *words, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        gDsi[DSI_WR_DATA] = words[i];
    gDsi[DSI_TRIGGER] = kDsiTriggerHost;
}
}  // namespace

extern "C" uint32_t tegra_display_panel_id(void)
{
    map();
    return pmc(SECURE_SCRATCH113) & 0xFFFF;
}

extern "C" void tegra_display_power_down(void)
{
    map();
    write(gDc, kDcDisable);
    switch_sleep_mark(SWITCH_STEP_DISPLAY_DC_OFF);
    // DSI off (hekate _display_dsi_soft_reset) and its pads powered down, while its clock still runs: a gated block stalls the bus.
    gDsi[DSI_VIDEO_MODE_CONTROL] = 0;
    gDsi[DSI_POWER_CONTROL] = 0;
    IODelay(10);
    gDsi[DSI_PAD_CONTROL_0] = kDsiPadsDown;
    tegra_gpio_output(kPanelReset, 0);
    IOSleep(10);
    switch_sleep_mark(SWITCH_STEP_DISPLAY_DSI_OFF);
    car(RST_DEV_H_SET) = kMipiCal | kDsi;
    car(CLK_ENB_H_CLR) = kMipiCal | kDsi;
    car(RST_DEV_L_SET) = kDisp1;
    car(CLK_ENB_L_CLR) = kDisp1;
    switch_sleep_mark(SWITCH_STEP_DISPLAY_CLOCKS_OFF);
}

extern "C" void tegra_display_power_up(void)
{
    map();
    // hekate hw_init enables HOST1X for every display module (the DC's vsync syncpoint goes to it); SC7 gated it.
    if (!(car(CLK_OUT_ENB_L) & kHost1x) || (car(RST_DEVICES_L) & kHost1x)) {
        car(RST_DEV_L_SET) = kHost1x;
        car(CLK_ENB_L_CLR) = kHost1x;
        car(CLK_SOURCE_HOST1X) = (4u << 29) | kDiv2p5;  // PLLP_OUT0 / 2.5 = 163.2 MHz
        car(CLK_ENB_L_SET) = kHost1x;
        IODelay(2);
        car(RST_DEV_L_CLR) = kHost1x;
        (void)car(RST_DEV_L_SET);
    }
    // display_init: DSI, MIPI-cal and DISP1 out of reset and clocked; MIPI-cal 17 MHz and DSIA LP 68 MHz from PLLP.
    car(RST_DEV_H_CLR) = kMipiCal | kDsi;
    car(CLK_ENB_H_SET) = kMipiCal | kDsi;
    car(RST_DEV_L_CLR) = kDisp1;
    car(CLK_ENB_L_SET) = kDisp1;
    car(CLK_ENB_X_SET) = kUartFstMipiCal;
    car(CLK_SOURCE_UART_FST_MIPI_CAL) = kDiv6;
    car(CLK_ENB_W_SET) = kDsiaLp;
    car(CLK_SOURCE_DSIA_LP) = kDiv6;
    // The DSI A pads (DSI and DSIB, 4 lanes) out of deep power down; a bounded handshake, as Linux tegra_io_pad_power_enable.
    if (!switch_pmc_io_pad_on(kDpdDsiPad) || !switch_pmc_io_pad_on(kDpdDsibPad)) {
        panic("TegraDisplay: DSI pads stay in deep power down: DPD_STATUS %#x DPD2_STATUS %#x",
              static_cast<uint32_t>(pmc(IO_DPD_STATUS)), static_cast<uint32_t>(pmc(IO_DPD2_STATUS)));
    }
    // Panel reset held (the OLED has no LCD AVDD/WLED pins), pad regulators up, T210B01 DSI pad control cleared.
    gApbMisc[kPinmuxLcdRst / 4] = kPinmuxPullDown;
    tegra_gpio_output(kPanelReset, 0);
    gMipiCal[MIPI_CAL_BIAS_PAD_CFG2] = 0;
    gMipiCal[MIPI_CAL_BIAS_PAD_CFG0] = 0;
    gApbMisc[kGpDsiPadControl / 4] = 0;
    // PLLD for command mode (97.8 MHz, DSI bit clock 48.9 MHz), DC A base configuration, DSI init in LP mode.
    setPlld(3, 20);
    write(gDc, kDcInit);
    write(gDsi, kDsiReset0);
    write(gDsi, kDsiReset1);
    write(gDsi, kDsiInit);
    IODelay(10);
    gDsi[DSI_POWER_CONTROL] = 0;
    IODelay(10);
    write(gDsi, kDsiLp);
    IOSleep(10);
    tegra_gpio_output(kPanelReset, 1);
    IOSleep(60);
    gDsi[DSI_BTA_TIMING] = 0x40103;  // Aula device takeover timeout
    // PANEL_SAM_AMS699VC01 (hekate display_init): sleep out, color mode basic, brightness + dimming control, level 2 unlock,
    // PWM transition 6 frames (ELVSS at offset 7), relock, brightness 0. hekate then sends display on; here the panel goes back
    // to sleep with these registers set, as TegraPanel's power-off leaves it, so the same tegra_panel_power_on() wakes it.
    sendLp((0x11u << 8) | 0x05, 180);            // EXIT_SLEEP_MODE
    sendLp(((0xA0u | 0x03u << 8) << 8) | 0x15);  // MIPI_DCS_PRIV_SM_SET_COLOR_MODE basic
    sendLp(((0x53u | 0x28u << 8) << 8) | 0x15);  // SET_CONTROL_DISPLAY brightness control | dimming
    {
        const uint32_t unlock[] = {0x539, 0x5A5A5AE2, 0x5A};
        sendLp(unlock, 3);
    }
    sendLp(((0xB0u | 7u << 8) << 8) | 0x15);  // MIPI_DCS_PRIV_SM_SET_REG_OFFSET 7
    sendLp(((0xB1u | 6u << 8) << 8) | 0x15);  // MIPI_DCS_PRIV_SM_SET_ELVSS 6
    {
        const uint32_t lock[] = {0x539, 0xA55A5AE2, 0xA5};
        sendLp(lock, 3);
    }
    {
        const uint32_t brightness[] = {0x339, 0x000051};
        sendLp(brightness, 2);
    }
    IOSleep(5);
    sendLp((0x10u << 8) | 0x05, 120);  // ENTER_SLEEP_MODE
    // HS: PLLD 468 MHz (DSI bit clock 234 MHz, pixel clock 78 MHz for 60 Hz), video packet sequence, host mode.
    setPlld(1, 24);
    write(gDsi, kDsiVideo);
    gDc[DC_DISP_DISP_CLOCK_CONTROL] = 4;  // PIXEL_CLK_DIVIDER_PCD1 | SHIFT_CLK_DIVIDER(4)
    write(gDsi, kDsiHostMode);
    IOSleep(10);
    // Pad calibration, twice: on T210B01 the clock lane termination code does not load in one pass (hekate's note).
    write(gMipiCal, kMipiPadCal);
    for (unsigned pass = 0; pass < 2; ++pass) {
        gMipiCal[MIPI_CAL_BIAS_PAD_CFG2] = 0x10010;
        gMipiCal[MIPI_CAL_BIAS_PAD_CFG1] = 0;
        write(gDsi, kDsiPadCal);
        write(gMipiCal, kMipiDsiCal);
        gMipiCal[MIPI_CAL_CTRL] = 0x2A000001;
    }
    IOSleep(10);
    write(gDc, kDcTimings);
}
