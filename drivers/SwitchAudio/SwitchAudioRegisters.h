// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio's hardware: T210 APE clocks, pads, AHUB, I2S1, ADMA and AGIC registers and timing constants.
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
#include "SwitchAudioAhub.h"
#include "SwitchAudioFormat.h"
#include "SwitchRT5639.h"
#include "../ArmGIC/SwitchInterrupts.h"
#include "../TegraGPIO/TegraGPIO.h"
#include "../TegraSMMU/TegraSMMU.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchDMA/SwitchDcache.h"
#include "../../kits/SwitchEngineGate/SwitchEngineGateKernel.h"
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
constexpr mach_vm_address_t kDmaMask = 0x00000000fffff000ULL;

constexpr unsigned kOutPeriods = 4, kInPeriods = 8;
constexpr uint32_t kOutPeriodFrames = SWITCH_AUDIO_RING_FRAMES / kOutPeriods;   // 16384
constexpr uint32_t kInPeriodFrames = SWITCH_AUDIO_IN_RING_FRAMES / kInPeriods;  // 2048
constexpr uint32_t kOutPeriodBytes = kOutBytes / kOutPeriods, kInPeriodBytes = kInBytes / kInPeriods;
static_assert(kOutPeriodFrames * kOutPeriods == SWITCH_AUDIO_RING_FRAMES, "a ring is whole ADMA periods");
static_assert(kInPeriodFrames * kInPeriods == SWITCH_AUDIO_IN_RING_FRAMES, "a ring is whole ADMA periods");
constexpr uint32_t kGraceMS = 50;
constexpr uint32_t kIdleMS = 5000;
constexpr uint32_t kJackSettleMS = 250;
#if DEVELOPMENT || DEBUG
constexpr uint32_t kStatsMS = 5000;
constexpr uint64_t kSettlePeriods = 2;
#endif
constexpr unsigned kChOut = 0, kChIn = 1;
constexpr uint32_t kAgicAdma = 32 + 24;
constexpr uint32_t kCascadeIntid = 32 + 102;

// Register blocks (T210 TRM Ch. 2.1-2.2).
constexpr vm_offset_t kPinmuxPA = 0x70003000, kAhubPA = 0x702d0000, kAdmaPA = 0x702e2000, kAgicPA = 0x702f9000;
constexpr vm_size_t kAhubSize = 0xd000;  // ADMAIF (0x0000) through MIXER1 (0xC2FF)

// CAR clock/reset IDs (include/dt-bindings/clock/tegra210-car.h) and source offsets.
enum : unsigned {
    kClkIdI2s0 = 30,
    kClkIdDAudio = 106,
    kClkIdApb2Ape = 107,
    kClkIdExtern1 = 120,
    kClkIdApe = 198,
};

enum : uint32_t {
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
};

enum : uint32_t {
    PLL_BYPASS = 1u << 31,
    PLL_ENABLE = 1u << 30,
    PLL_LOCK = 1u << 27,
    PLLA_IDDQ = 1u << 25,
    PLLA_MNP_MASK = 0x01f0ffff,
    PLLA_LOCK_ENABLE = 0x10000000,
    PLLA_EN_SDM = 1u << 26,
};

// Supported nominal client rates (advertised to CoreAudio); link clocks natively at 48 kHz or 44.1 kHz,
// with AHUB SFC converting any other rate in hardware.
constexpr switch_audio_clock kClocks[2] = {SWITCH_AUDIO_CLOCK_48000, SWITCH_AUDIO_CLOCK_44100};
constexpr uint32_t kSupportedRates[SWITCH_AUDIO_MAX_RATES] = {48000u, 44100u, 96000u, 88200u, 32000u,
                                                              24000u, 22050u, 16000u, 11025u, 8000u};
static_assert(kClocks[0].rate == SWITCH_AUDIO_RATE, "a service starts at SWITCH_AUDIO_RATE");
constexpr uint32_t kPllaPHw = 1, kApeSource = (4u << 29) | 30;
static_assert(kClocks[0].p == 2 && kClocks[1].p == 2, "p 2 is hardware 1");
static_assert(switch_audio_i2s_divider(kClocks[0].out0_per_fs) == 30, "I2S0 = PLLA_OUT0 / 16 (50% duty)");
static_assert(switch_audio_extern1_divider(kClocks[0].out0_per_fs) == 6, "EXTERN1 = PLLA_OUT0 / 4 (50% duty)");

constexpr uint32_t pllaMnp(const switch_audio_clock &c)
{
    return (kPllaPHw << 20) | (c.n << 8) | c.m;
}

// APBDEV_PMC offsets (accessed exclusively via SwitchPMC).
enum : uint32_t {
    CLAMP_STATUS = 0x2c,
    PMC_REMOVE_CLAMPING = 0x34,
    PWRGATE_STATUS = 0x38,
    NO_IOPOWER = 0x44,
    PWR_DET = 0x48,
    PWR_DET_VAL = 0xe4,
    CLK_OUT_CNTRL = 0x1a8,
    IO_DPD_STATUS = 0x1bc,
    IO_DPD2_STATUS = 0x1c4,
};

// MC hot-reset for APE power gating (TRM Ch. 18.11.1.114-115, written exclusively via tegra_mc_hot_reset).
constexpr uint32_t kMcHotResetCtrl1 = 0x970, kMcHotResetStatus1 = 0x974, kMcHotResetApe = 1u << 6;

constexpr uint32_t kPartitionAud = 1u << 27;
constexpr unsigned kPartitionAudId = 27;
constexpr uint32_t kPad33V = (1u << 5) | (1u << 18);
constexpr uint32_t kNoIoPowerAudioHv = 1u << 18;
constexpr uint32_t kClkOut1Mask = (3u << 6) | (1u << 2), kClkOut1Extern1 = (3u << 6) | (1u << 2);

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
    {0x214, 0x50},  // touch_rst_pv6: headphone-detect net, no pull, tristate, input
};
constexpr uint32_t kPinJackDetect = 0x214;
constexpr uint32_t kPinFields = 0x7f;

// AHUB core offsets: ADMAIF (+0x000), I2S1 (+0x1000). Other AHUB blocks live in SwitchAudioAhub.h.
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

constexpr uint32_t kI2sCtrl = switch_audio_i2s_ctrl(), kI2sTiming = switch_audio_i2s_timing(), kI2sDataOffset = 1u << 8;
static_assert(kI2sCtrl == 0x1f000407 && kI2sTiming == 31, "I2S1: 32-bit slots, 64 fs");

// ADMA (0x702E2000).
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
constexpr unsigned kJackDetect = TEGRA_GPIO(V, 6);
}  // namespace switch_audio_hw
