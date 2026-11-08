// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio's hardware: the T210 APE's clocks, pads, AHUB, I2S1 and ADMA registers and the driver's timing
// constants, for the driver's files only (SwitchAudio.cpp says where each comes from).
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <os/log.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "SwitchAudioAbi.h"
#include "SwitchAudioFormat.h"
#include "SwitchRT5639.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"
#include "../../kits/SwitchRefusal/SwitchRefusal.h"

extern "C" {
// NOLINTNEXTLINE(readability-redundant-declaration): not every kernel configuration's headers declare it
extern task_t kernel_task;
vm_offset_t phystokv(uint64_t pa);
uint64_t pmap_find_pa(pmap_t pmap, uint64_t va);
}

namespace switch_audio_hw {
constexpr uint32_t kOutBytes = sizeof(switch_audio_sample_t) * SWITCH_AUDIO_RING_FRAMES * SWITCH_AUDIO_OUT_CHANNELS;
constexpr uint32_t kInBytes = sizeof(switch_audio_sample_t) * SWITCH_AUDIO_IN_RING_FRAMES * SWITCH_AUDIO_IN_CHANNELS;
constexpr uint32_t kTickMS = SWITCH_AUDIO_PERIOD_FRAMES * 1000 / SWITCH_AUDIO_RATE;
// Below 4 GiB and physically contiguous, so the ADMA plays the rings in place (32-bit addresses).
constexpr mach_vm_address_t kDmaMask = 0x00000000fffff000ULL;

// Playback's period interrupt only clears what played and moves out_frames (the plug-in times its IO from the host
// clock), so its periods are long: an interrupt every 341 ms at 48 kHz. Capture's in_frames moves a period at a time,
// so its period is the microphone's latency. ADMA_CH_CONFIG_MAX_BUFS is 8.
constexpr unsigned kOutPeriods = 4, kInPeriods = 8;
constexpr uint32_t kOutPeriodFrames = SWITCH_AUDIO_RING_FRAMES / kOutPeriods;   // 16384
constexpr uint32_t kInPeriodFrames = SWITCH_AUDIO_IN_RING_FRAMES / kInPeriods;  // 2048
constexpr uint32_t kOutPeriodBytes = kOutBytes / kOutPeriods, kInPeriodBytes = kInBytes / kInPeriods;
static_assert(kOutPeriodFrames * kOutPeriods == SWITCH_AUDIO_RING_FRAMES, "a ring is whole ADMA periods");
static_assert(kInPeriodFrames * kInPeriods == SWITCH_AUDIO_IN_RING_FRAMES, "a ring is whole ADMA periods");
constexpr uint32_t kGraceMS = 50;        // after a missed deadline, for an interrupt action already queued
constexpr uint32_t kIdleMS = 5000;       // codec power-down after the last stream stops (ASoC pmdown_time)
constexpr uint32_t kJackSettleMS = 250;  // jack contact bounce
#if DEVELOPMENT || DEBUG
constexpr uint32_t kStatsMS = 5000;     // the stats line
constexpr uint64_t kSettlePeriods = 2;  // the stats: periods after a start left out of lead and lag
#endif
constexpr unsigned kChOut = 0, kChIn = 1;
constexpr uint32_t kAgicAdma = 32 + 24;  // AGIC INTID of ADMA channel 0
constexpr uint32_t kCascadeIntid = 32 + 102;

// Register blocks (T210 TRM map; hekate bdk/soc/t210.h).
constexpr vm_offset_t kCarPA = 0x60006000, kPmcPagePA = 0x7000e000, kPinmuxPA = 0x70003000, kAhubPA = 0x702d0000,
                      kAdmaPA = 0x702e2000, kAgicPA = 0x702f9000;
constexpr uint32_t kPmcOffset = 0x400;  // APBDEV_PMC 0x7000e400

// CLK_RST_CONTROLLER (hekate bdk/soc/clock.h; Linux clk-tegra-periph.c, clk-tegra210b01.c).
enum : uint32_t {
    RST_DEVICES_L = 0x4,
    CLK_OUT_ENB_L = 0x10,
    OSC_CTRL = 0x50,
    PLLA_BASE = 0xb0,
    PLLA_OUT = 0xb4,
    PLLA_MISC1 = 0xb8,
    PLLA_MISC0 = 0xbc,
    PLLA_MISC2 = 0x5d8,
    CLK_SOURCE_I2S0 = 0x1d8,
    CLK_SOURCE_D_AUDIO = 0x3d0,
    CLK_SOURCE_EXTERN1 = 0x3ec,
    CLK_SOURCE_APE = 0x6c0,
    CLK_ENB_L_SET = 0x320,
    RST_DEV_L_SET = 0x300,
    RST_DEV_L_CLR = 0x304,
    CLK_ENB_V_SET = 0x440,
    RST_DEV_V_SET = 0x430,
    RST_DEV_V_CLR = 0x434,
    CLK_ENB_Y_SET = 0x29c,
    RST_DEV_Y_SET = 0x2a8,
    RST_DEV_Y_CLR = 0x2ac,
};

// Clock numbers: i2s0 30 (L), d_audio 106, apb2ape 107, extern1 120 (V), ape 198 (Y).
constexpr uint32_t kClkL = 1u << 30, kClkV = (1u << 10) | (1u << 11) | (1u << 24), kClkY = 1u << 6;

enum : uint32_t {
    PLL_BYPASS = 1u << 31,
    PLL_ENABLE = 1u << 30,
    PLL_LOCK = 1u << 27,
    PLLA_IDDQ = 1u << 25,
    PLLA_MNP_MASK = 0x01f0ffff,     // plla_nmp: m 7:0, n 15:8, p 24:20
    PLLA_LOCK_ENABLE = 0x10000000,  // PLLA_MISC0_DEFAULT_VALUE
    PLLA_EN_SDM = 1u << 26,         // MISC2
};

// The rate families the I2S runs (SwitchAudioFormat.h), the first at start. PLLA = 38.4 MHz / m * (n + 1/2 + sdm /
// 8192) / p (clk-pll.c:653); p 2 is hardware 1 (pll_qlin_pdiv_to_hw). 7.1 dividers (value = 2 * ratio - 2) take
// PLLA_OUT0, the I2S bit clock (64 fs) and MCLK (extern1, 256 fs) from it; APE = PLLP 408 MHz / 16.
constexpr switch_audio_clock kClocks[SWITCH_AUDIO_MAX_RATES] = {SWITCH_AUDIO_CLOCK_48000, SWITCH_AUDIO_CLOCK_44100};
static_assert(kClocks[0].rate == SWITCH_AUDIO_RATE, "a service starts at SWITCH_AUDIO_RATE");
constexpr uint32_t kPllaPHw = 1, kApeSource = (4u << 29) | 30;
static_assert(kClocks[0].p == 2 && kClocks[1].p == 2, "p 2 is hardware 1");
static_assert(switch_audio_i2s_divider(kClocks[0].out0_per_fs) == 30, "I2S0 = PLLA_OUT0 / 16 at 48 kHz");
static_assert(switch_audio_i2s_divider(kClocks[1].out0_per_fs) == 38, "I2S0 = PLLA_OUT0 / 20 at 44.1 kHz");

constexpr uint32_t pllaMnp(const switch_audio_clock &c)
{
    return (kPllaPHw << 20) | (c.n << 8) | c.m;
}

// APBDEV_PMC (hekate bdk/soc/pmc_t210.h; mainline pmc.c tegra210_io_pads/io_pad_vctrls).
enum : uint32_t {
    CLAMP_STATUS = 0x2c,
    PWRGATE_STATUS = 0x38,
    NO_IOPOWER = 0x44,
    PWR_DET = 0x48,
    PWR_DET_VAL = 0xe4,
    CLK_OUT_CNTRL = 0x1a8,
    IO_DPD_STATUS = 0x1bc,
    IO_DPD2_STATUS = 0x1c4,
};

constexpr uint32_t kPartitionAud = 1u << 27;          // POWER_RAIL_AUD
constexpr uint32_t kPartitionAudId = 27;              // TEGRA_POWERGATE_AUD
constexpr uint32_t kPad33V = (1u << 5) | (1u << 18);  // audio, audio-hv: 1 = 3.3 V
constexpr uint32_t kNoIoPowerAudioHv = 1u << 18;
constexpr uint32_t kClkOut1Mask = (3u << 6) | (1u << 2), kClkOut1Extern1 = (3u << 6) | (1u << 2);  // src 3, force on

// Pinmux (PINMUX_AUX at 0x70003000, offsets pinctrl-tegra210.c minus 0x3000): function 1:0, pull 3:2 (1 down),
// tristate 4, park 5, input 6. Values from the DTB pinmux node; the park bit is cleared as Linux does on T210.
struct Pin {
    uint16_t offset;
    uint8_t value;
};

constexpr Pin kPins[] = {
    {0x124, 0x00},  // dap1_fs_pb0: i2s1, driven
    {0x128, 0x54},  // dap1_din_pb1: i2s1, pull-down, tristate, input
    {0x12c, 0x00},  // dap1_dout_pb2: i2s1, driven
    {0x130, 0x00},  // dap1_sclk_pb3: i2s1, driven
    {0x180, 0x00},  // aud_mclk_pbb0: aud (clk_out_1), driven
    {0x190, 0x54},  // gpio_x3_aud_pbb4: codec IRQ, pull-down, input
    {0x214, 0x50},  // touch_rst_pv6: the headphone-detect net, no pull, tristate, input
    {0x28c, 0x05},  // pz4: rsvd1, pull-down, driven (codec LDO1_EN)
};
// Nothing in the boot chain (hekate, U-Boot's pinmux table, TegraGPIO) muxes PV6; L4T sets it as above. The value
// found is logged with the first jack reading.
constexpr uint32_t kPinJackDetect = 0x214;
constexpr uint32_t kPinFields = 0x7f;

// AHUB block 0x702d0000: ADMAIF +0 (mainline tegra210_admaif.h), XBAR +0x800 (one mux per destination, a bit per
// source, three parts 0x200 apart: MUX_REG/MUX_VALUE in tegra210_ahub.h), I2S1 +0x1000 (tegra210_i2s.h).
enum : uint32_t {
    ADMAIF_RX_ENABLE = 0x000,
    ADMAIF_RX_SOFT_RESET = 0x004,
    ADMAIF_RX_STATUS = 0x00c,
    ADMAIF_RX_CIF = 0x020,
    ADMAIF_TX_ENABLE = 0x300,
    ADMAIF_TX_SOFT_RESET = 0x304,
    ADMAIF_TX_STATUS = 0x30c,
    ADMAIF_TX_CIF = 0x320,
    ADMAIF_GLOBAL_ENABLE = 0x700,
    XBAR_ADMAIF1 = 0x800,
    XBAR_I2S1 = 0x840,
    XBAR_PART = 0x200,
    I2S_RX_ENABLE = 0x1000,
    I2S_RX_SOFT_RESET = 0x1004,
    I2S_RX_STATUS = 0x100c,
    I2S_RX_CIF = 0x1020,
    I2S_RX_CTRL = 0x1024,
    I2S_TX_ENABLE = 0x1040,
    I2S_TX_SOFT_RESET = 0x1044,
    I2S_TX_STATUS = 0x104c,
    I2S_TX_CIF = 0x1060,
    I2S_TX_CTRL = 0x1064,
    I2S_ENABLE = 0x1080,
    I2S_CTRL = 0x10a0,
    I2S_TIMING = 0x10a4,
};

constexpr uint32_t kXbarFromAdmaif1 = 1u << 0, kXbarFromI2s1 = 1u << 16;  // MUX_VALUE(0, 0), MUX_VALUE(0, 16)
// The S32 link's words (SwitchAudioFormat.h, as L4T's hw_params build them for S32_LE): audio CIFs 32/32 bits, the
// ADMAIF unpacked (one sample per word), I2S1 master in LRCK frames with 32-bit slots (TIMING 31: half a 64 fs frame),
// data offset 1 (I2S).
constexpr uint32_t kI2sCtrl = switch_audio_i2s_ctrl(), kI2sTiming = switch_audio_i2s_timing(), kI2sDataOffset = 1u << 8;
static_assert(kI2sCtrl == 0x1f000407 && kI2sTiming == 31, "I2S1: 32-bit slots, 64 fs");

// ADMA (tegra210-adma.c, L4T): channel registers at 0x80 per channel, globals at 0xc00.
enum : uint32_t {
    CH_CMD = 0x00,
    CH_STATUS = 0x0c,
    CH_INT_STATUS = 0x10,
    CH_INT_CLEAR = 0x1c,
    CH_CTRL = 0x24,
    CH_CONFIG = 0x28,
    CH_FIFO_CTRL = 0x2c,
    CH_TC_STATUS = 0x30,
    CH_SRC = 0x34,
    CH_TRG = 0x3c,
    CH_TC = 0x44,
    CH_XFER_STATUS = 0x54,
    CH_STRIDE = 0x80,
    GLOBAL_CMD = 0xc00,
    GLOBAL_SOFT_RESET = 0xc04,
    GLOBAL_CG = 0xc08,
    GLOBAL_INT_CLEAR = 0xc20,
};

// CTRL: request 1 (ADMAIF1) at 28 (tx) / 24 (rx), direction MEM2AHUB 4 / AHUB2MEM 2 at 12, continuous mode, flow
// control. CONFIG: buffers - 1 at 28 (src) / 24 (trg), burst fls(16) = 5 at 20, WRR weight 1. FIFO_CTRL: size 3
// (request <= 2) at 8 (tx) / 0 (rx).
constexpr uint32_t kChCtrl[2] = {(1u << 28) | (4u << 12) | (2u << 8) | (1u << 1),
                                 (1u << 24) | (2u << 12) | (2u << 8) | (1u << 1)};
constexpr uint32_t kChConfig[2] = {((kOutPeriods - 1) << 28) | (5u << 20) | 1u,
                                   ((kInPeriods - 1) << 24) | (5u << 20) | 1u};
constexpr uint32_t kChFifo[2] = {3u << 8, 3u};

// AGIC (GICv2: distributor at +0, CPU interface at +0x1000).
enum : uint32_t {
    GICD_CTLR = 0x000,
    GICD_ISENABLER = 0x100,
    GICD_ICENABLER = 0x180,
    GICD_ISPENDR = 0x200,
    GICD_ICPENDR = 0x280,
    GICD_ISACTIVER = 0x300,
    GICD_IPRIORITYR = 0x400,
    GICD_ITARGETSR = 0x800,
    GICD_ICFGR = 0xc00,
    GICC_CTLR = 0x1000,
    GICC_PMR = 0x1004,
    GICC_IAR = 0x100c,
    GICC_EOIR = 0x1010,
};

constexpr unsigned kCodecIrq = TEGRA_GPIO(BB, 4);
constexpr unsigned kJackDetect = TEGRA_GPIO(V, 6);  // the headphone-detect net, 0 = plugged; diagnostics only
}  // namespace switch_audio_hw
