// SPDX-License-Identifier: BSD-2-Clause-Patent
// SwitchAudio: the kernel half of the Switch's iPadOS audio device. iPadOS 17's audio server (audiomxd) loads our
// AudioServerPlugIn (hal/SwitchAudioPlugIn.c); the plug-in opens this service, maps its control page and two sample
// rings (SwitchAudioAbi.h) and asks it to start and stop each direction. Design after UniXNU-sheng's
// package/ShengAudioService.cpp, the iPadOS 17.7.11 audio path that is known to play on real hardware.
//
// Backends:
//   Tegra I2S playback (speaker and headphone jack) and capture (the headset microphone on the jack). The jack's state,
//      headphones and whether the plug has a microphone, is in the control page for the plug-in's Codec device. A
//      capture channel that delivers nothing in its first periods is turned off for the boot with one line (it had
//      never run on hardware before 11-07); the microphone then reads as absent.
//   null, with boot-arg switch-no-audio: a 10 ms timer advances the play position on the host clock and clears what it
//      passed; capture is silence; no Tegra hardware is touched. The timer runs only while a stream does.
// The Tegra backend logs one line every 5 s while streams run: streams started, frames played, underruns, the loudest sample, the
// DMA, I2S and jack state and the codec's power and volume registers. The register fields are read at the moment of the
// line: between streams the I2S is off (en 0) and, 5 s after the last one, the codec's power registers are 0.
// The Tegra backend is interrupt driven throughout: the ADMA period interrupt moves the positions, the codec's IRQ
// line reports the jack, and an interrupt that does not arrive, a DMA that stalls or runs fast, or hardware that does
// not answer, panics with one line.
//
// The OLED's audio path. Sources: the shipped DTB (oled-sku3.dtb in bl33.bin = CTCaer
// switch-l4t-platform-t210-nx tegra210b01-fric.dts over tegra210b01-nx-common.dtsi:864-950), switchroot Android
// android_device_nintendo_nx media/nvaudio_conf.xml (the mixer paths), CTCaer switch-l4t-kernel-nvidia
// sound/soc/codecs/rt5640.c and sound/soc/tegra-alt (tegra_machine_driver_mobile.c, tegra_asoc_utils_alt.c,
// tegra210_i2s_alt.c, tegra210-adma.c), CTCaer switch-l4t-kernel-4.9 drivers/clk/tegra, mainline sound/soc/tegra
// (tegra210_ahub.c, tegra210_admaif.c, tegra210_i2s.c) and pinctrl-tegra210.c.
//  - Codec: Realtek RT5639 (rt5640 family, VENDOR_ID2 0x6231) on I2C1 at 0x1c. Its own class-D amplifier drives the
//    speakers (SPOLP/N, SPORP/N; spkvdd = vdd-ac-bat) and its HP amplifier the jack (HPOL/R); there is no external
//    amplifier. The only microphone is a headset's, on IN1P (single ended). LDO1_EN = PZ4 (high = on, then 400 ms);
//    the codec's GPIO1 is its IRQ output, on PBB4; jack detect is the codec's own (realtek,sel-jd-source 4 = its
//    GPIO2, jack-detect-is-jd1, inverted: JD_STATUS 0 = plugged). The same headphone-detect net reaches Tegra PV6
//    (the DT's jack-detect-gpios, active low; tegra210b01-fric-pinmux.dtsi touch_rst_pv6 "Audio Codec (ALC5639)
//    Headphone Detect": rsvd0, no pull, tristate, input). rt5640.c never reads PV6, but its pad is on the net, so it is
//    muxed as L4T leaves it before the codec is read; its level only goes into the diagnostics here.
//    Microphone vs plain headphones: MICBIAS1 over-current at 600 uA.
//  - Link: Tegra I2S1 (AHUB 0x702d1000; CAR clock "i2s0") is bit and frame clock master, the codec slave
//    (dai-link-1: format i2s, no bitclock-master), stereo, 48 kHz. Samples are S32_LE from memory to the I2S
//    (SwitchAudioFormat.h): 32-bit slots, BCLK 64 fs, and the codec reads 24 bits of each (I2S1 data length 24, BCLK
//    64 fs), as rt5640_hw_params sets it for 24-bit samples in a 64-bit frame. (The DTB's dai-link says s16_le, BCLK
//    32 fs, as L4T runs it.) MCLK 256 fs = 12.288 MHz (mclk-fs 256) on AUD_MCLK = PMC
//    clk_out_1 <- extern1 <- PLLA_OUT0 (clk-tegra210.c init table); the codec runs from MCLK directly (sysclk source
//    MCLK, pre-divider 1). Pins DAP1_FS/DIN/DOUT/SCLK (function i2s1), AUD_MCLK.
//  - Clocks for 48 kHz (tegra_asoc_utils_alt.c): PLLA 368.64 MHz (T210B01 table, clk-tegra210b01.c:1789-1793:
//    38.4 MHz ref, m 2, n 38, p 2, SDM 0xfccc, really 368639062.5 Hz), PLLA_OUT0 49.152 MHz, AHUB (d_audio)
//    = PLLA_OUT0, I2S 3.072 MHz (64 fs), extern1 12.288 MHz, APE from PLLP at 25.5 MHz. The real frame rate is
//    therefore 47999.878 Hz, and ticks_per_frame_q32 says so.
//  - The 44.1 kHz family (SwitchAudioFormat.h's clock table, chosen by the plug-in through SwitchAudioSetRate between
//    streams): PLLA 282.24 MHz (m 2, n 29, p 2, SDM 0xfccc, the T210B01 table's other entry), PLLA_OUT0 / 5 = 56.448
//    MHz (1280 fs), I2S / 20, extern1 / 5, so 44099.854 Hz. The codec needs nothing new: MCLK stays 256 fs.
//  - Routing: XBAR "I2S1 Mux" = ADMAIF1 (playback), "ADMAIF1 Mux" = I2S1 (capture), as nvaudio_conf.xml sets them;
//    ADMA request line 1 = ADMAIF1 (DTB admaif dmas). ADSP and the AHUB effect blocks are not used.
//  - Interrupts: ADMA channel n raises SPI 24+n on the AGIC, the APE's own GICv2 (distributor 0x702f9000, CPU
//    interface 0x702fa000; DTB adma@702e2000 interrupts), whose host output is main-GIC SPI 102 = INTID 134 (DTB
//    agic@702f9000 interrupts <0 0x66 ..>). This driver claims switch_interrupt_nub(134, false) and runs the
//    cascade: the channel's AGIC SPI targeted at the interface the CPU reads in its banked ITARGETSR0, IAR/EOIR on the
//    AGIC, ADMA_CH_INT_STATUS/CLEAR. Channels 0 (playback) and 1 (capture) are host channels.
//
// The ADMA plays the playback ring and fills the capture ring in place: one cycle of 8 periods (the most an ADMA
// cycle holds) is exactly one ring, so a period is 2048 frames (42.7 ms, 16 KiB of S32 stereo). Each period interrupt
// clears the playback period just consumed (so the next turn mixes onto silence) and invalidates the capture period
// just written, then moves out_frames/in_frames on. The ADMA does not snoop the caches: the plug-in cleans what it
// mixes, this driver cleans what it clears and invalidates what the ADMA wrote. Sample time 0 is the host time the
// playback DMA started.
//
// Started by switch_audio_publish() from the board thread; the classes register through the normal static
// initializer (they derive only from XNU's own IOService and IOUserClient).
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
extern task_t kernel_task;
vm_offset_t phystokv(uint64_t pa);
uint64_t pmap_find_pa(pmap_t pmap, uint64_t va);
}

namespace {
constexpr uint32_t kOutBytes = SWITCH_AUDIO_RING_FRAMES * SWITCH_AUDIO_OUT_CHANNELS * sizeof(switch_audio_sample_t);
constexpr uint32_t kInBytes = SWITCH_AUDIO_IN_RING_FRAMES * SWITCH_AUDIO_IN_CHANNELS * sizeof(switch_audio_sample_t);
constexpr uint32_t kTickMS = SWITCH_AUDIO_PERIOD_FRAMES * 1000 / SWITCH_AUDIO_RATE;
// Below 4 GiB and physically contiguous, so the ADMA plays the rings in place (32-bit addresses).
constexpr mach_vm_address_t kDmaMask = 0x00000000fffff000ULL;

uint64_t timerHz()
{
    mach_timebase_info_data_t tb;
    clock_timebase_info(&tb);
    return 1000000000ULL * tb.denom / tb.numer;
}

// CoreAudio assumes a 24 MHz host clock (Apple's timer): AudioGetHostClockFrequency and the audio server's IO thread
// read one double constant in the dyld shared cache. This board's timer runs at 19.2 MHz (ADT /cpus/cpu0
// timebase-frequency), so without this the audio server's IO cycles run 25 % long (UniXNU-sheng, sheng.md). The
// constant sits on a read-only shared page, so the caller cannot write it; this rewrites that physical page once for
// every process. Only a value of exactly 24 MHz (or ours already) at an address inside the shared region is touched.
IOReturn setHostClock(task_t task, uint64_t address)
{
    constexpr uint64_t kApple = 0x4176e36000000000ULL;  // 24000000.0
    if ((address & 7) || address < 0x180000000ULL || address >= 0x400000000ULL)
        return kIOReturnBadArgument;
    IOMemoryDescriptor *memory = IOMemoryDescriptor::withAddressRange(address, 8, kIODirectionOut, task);
    if (!memory)
        return kIOReturnNoMemory;
    IOReturn result = memory->prepare();  // wires the page while we write it
    if (result != kIOReturnSuccess) {
        memory->release();
        return result;
    }
    uint64_t seen = 0;
    uint64_t pa = copyin(address, &seen, sizeof(seen)) ? 0 : pmap_find_pa(get_task_pmap(task), address);
    // The timer frequency as an IEEE double, from the integer (no floating point in the kernel).
    const uint64_t hz = timerHz();
    const unsigned top = 63 - (unsigned)__builtin_clzll(hz);
    const uint64_t ours = (uint64_t)(1023 + top) << 52 | ((hz << (52 - top)) & ((1ULL << 52) - 1));
    result = pa ? kIOReturnUnsupported : kIOReturnNotReadable;
    if (pa) {
        volatile uint64_t *value = reinterpret_cast<volatile uint64_t *>(phystokv(pa));
        if (*value == seen && (seen == kApple || seen == ours)) {
            *value = ours;
            __asm__ volatile("dsb sy" ::: "memory");
            result = kIOReturnSuccess;
        }
    }
    memory->complete();
    memory->release();
    return result;
}

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
}  // namespace

class SwitchAudio : public IOService {
    OSDeclareDefaultStructors(SwitchAudio) IOWorkLoop *loop = nullptr;
    IOCommandGate *gate = nullptr;
    IOTimerEventSource *timer = nullptr;  // null: the play clock; Tegra: the period interrupt's deadline
    IOBufferMemoryDescriptor *memory[3] = {};
    SwitchAudioControl *control = nullptr;

    uint64_t hz = 0;
    const switch_audio_clock *clock = &kClocks[0];  // what the I2S runs (or runs next)

    // Tegra backend: hardware on `loop`, the codec (I2C transfers sleep) on its own work loop.
    bool tegra = false, tegraCapture = false;
    volatile uint8_t *car = nullptr, *pmc = nullptr, *pinmux = nullptr, *ahub = nullptr, *adma = nullptr,
                     *agic = nullptr;
    uint32_t ringPA[3] = {};
    uint32_t jackPinFound = 0;  // PV6's pinmux as the boot chain left it (logged with the first jack reading)
    IOInterruptEventSource *cascade = nullptr;
    uint64_t periods[2] = {};   // periods done since the DMA started
    uint16_t xferSeen[2] = {};  // ADMA_CH_XFER_STATUS (completed-period counter) at the last interrupt
    bool graced = false;
    IOWorkLoop *codecLoop = nullptr;
    IOCommandGate *codecGate = nullptr;
    IOInterruptEventSource *jackSource = nullptr;
    IOTimerEventSource *jackTimer = nullptr, *idleTimer = nullptr;
    bool codecOn = false, jackKnown = false, jackIn = false, headset = false, speakerOnly = false;
    bool captureProven = false;      // a capture period has arrived since start
    bool captureRunning = false;     // ADMA ch1 and I2S TX run: input is requested (captureStart)
    bool captureCodecOn = false;     // the codec's ADC path and microphone bias are up (codec work loop)
    uint64_t captureStartFrame = 0;  // the sample time of capture ring frame 0
    unsigned headphoneSteps = 0;     // the plug-in's headphone level, in the DAC's 0.375 dB steps (setHeadphoneLevel)
    bool hardwareLost = false;       // the APE did not come back after sleep, or shut down: never touched again
    bool codecOff = false;           // shut down: the codec is unpowered
    IONotifier *shutdownNotifier = nullptr;
    SwitchRT5639 codec;
#if DEVELOPMENT || DEBUG
    // The stats: counted on the audio work loop, printed (and the peak reset) on the codec work loop.
    IOTimerEventSource *statsTimer = nullptr;
    uint64_t statFrames = 0, statFramesShown = 0, statIoCyclesShown = 0, statIoFramesShown = 0;
    uint32_t statStarts = 0, statStartsShown = 0, statUnderruns = 0;
    bool statsArmed = false, statsShown = false;  // statsTimer: armed / a line written (statsFired)
    int statPeak = 0;
    int64_t statLagMin = INT64_MAX, statLagMax = INT64_MIN, statLeadMin = INT64_MAX;
    uint64_t periodTicksQ16 =
        0;  // host ticks per playback period in 48.16 (kOutPeriodFrames * ticks_per_frame_q32 >> 16)
#endif

    uint64_t nowFrame() const
    {
        const uint64_t now = mach_absolute_time();
        if (now <= control->anchor_host)
            return 0;
        const uint64_t ticks = now - control->anchor_host;  // split so nothing overflows (no 128-bit helpers here)
        return ticks / hz * clock->rate + ticks % hz * clock->rate / hz;
    }

    switch_audio_sample_t *ring(uint32_t type) const
    {
        return static_cast<switch_audio_sample_t *>(memory[type]->getBytesNoCopy());
    }

    // Playback up to now is "played"; clear it so the next turn of the ring mixes onto silence.
    void advance()
    {
        const uint64_t now = nowFrame();
        if (control->state & SwitchAudioOutputRunning) {
            switch_audio_sample_t *out = ring(SwitchAudioMemoryOutput);
            uint64_t from = control->out_frames;
            if (now > from + SWITCH_AUDIO_RING_FRAMES)
                from = now - SWITCH_AUDIO_RING_FRAMES;
            while (from < now) {
                const uint64_t at = from % SWITCH_AUDIO_RING_FRAMES;
                const uint64_t run =
                    now - from < SWITCH_AUDIO_RING_FRAMES - at ? now - from : SWITCH_AUDIO_RING_FRAMES - at;
                bzero(out + at * SWITCH_AUDIO_OUT_CHANNELS, run * SWITCH_AUDIO_OUT_CHANNELS * sizeof(*out));
                from += run;
            }
            control->out_frames = now;
        }
        if (control->state & SwitchAudioInputRunning)
            control->in_frames = now;  // the capture ring stays silent
    }

    static void tick(OSObject *owner, IOTimerEventSource *source)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (!self->control->state)
            return;
        self->advance();
        source->setTimeoutMS(kTickMS);
    }

    static volatile uint8_t *map(vm_offset_t pa, vm_size_t size, const char *what)
    {
        volatile uint8_t *base = switch_mmio_map(pa, size);
        if (!base)
            panic("SwitchAudio: cannot map %s at %#lx", what, (unsigned long)pa);
        return base;
    }

    // The rate the I2S really runs at, as host ticks per frame in 32.32 (47999.878 Hz for 48 kHz, 44099.854 Hz for
    // 44.1 kHz).
    uint64_t tegraTicksPerFrame(uint64_t refHz) const
    {
        return switch_audio_clock_ticks_per_frame_q32(hz, refHz, *clock);
    }

    uint32_t watchdogMS() const  // 4 periods: 170 ms at 48 kHz
    {
        return 4 * kOutPeriodFrames * 1000 / clock->rate;
    }

    // `clock`'s PLLA and PLLA_OUT0, in the order clk-pll.c programs a disabled SDM PLL (set_defaults,
    // _update_pll_mnp, _clk_pll_enable, wait for lock). Left as it is if it already runs this configuration.
    void startPlla()
    {
        const uint32_t mnp = pllaMnp(*clock), sdm = clock->sdm;
        const uint32_t base = switch_mmio_read32(car, PLLA_BASE);
        const bool running = (base & (PLL_ENABLE | PLL_BYPASS | PLLA_IDDQ)) == PLL_ENABLE &&
                             (base & PLLA_MNP_MASK) == mnp && (switch_mmio_read32(car, PLLA_MISC2) & PLLA_EN_SDM) &&
                             (switch_mmio_read32(car, PLLA_MISC1) & 0xffff) == sdm;
        if (!running) {
            switch_mmio_write32(car, PLLA_OUT,
                                switch_mmio_read32(car, PLLA_OUT) & ~3u);  // PLLA_OUT0 off while the PLL changes
            switch_mmio_write32(car, PLLA_BASE, (base & ~PLL_ENABLE) | PLLA_IDDQ);
            switch_mmio_write32(car, PLLA_MISC0, PLLA_LOCK_ENABLE);
            switch_mmio_write32(car, PLLA_MISC2, 0);
            switch_mmio_write32(car, PLLA_BASE,
                                (switch_mmio_read32(car, PLLA_BASE) & ~(PLL_BYPASS | PLLA_MNP_MASK)) | mnp);
            switch_mmio_write32(car, PLLA_MISC1, (switch_mmio_read32(car, PLLA_MISC1) & ~0xffffu) | sdm);
            switch_mmio_write32(car, PLLA_MISC2, PLLA_EN_SDM);
            IODelay(1);
            switch_mmio_write32(car, PLLA_BASE, switch_mmio_read32(car, PLLA_BASE) & ~PLLA_IDDQ);
            IODelay(5);
            switch_mmio_write32(car, PLLA_BASE, switch_mmio_read32(car, PLLA_BASE) | PLL_ENABLE);
            if (!switch_mmio_wait32(car, PLLA_BASE, PLL_LOCK, PLL_LOCK, 600)) {
                panic("SwitchAudio: PLLA did not lock: BASE %#x MISC0 %#x MISC1 %#x MISC2 %#x",
                      switch_mmio_read32(car, PLLA_BASE), switch_mmio_read32(car, PLLA_MISC0),
                      switch_mmio_read32(car, PLLA_MISC1), switch_mmio_read32(car, PLLA_MISC2));
            }
            IODelay(50);
        }
        // PLLA_OUT0: ratio at 15:8, out of reset (bit 0) and enabled (bit 1); the vreg field 31:28 stays 0.
        const uint32_t out0Div = clock->out0_x2 - 2;
        switch_mmio_write32(car, PLLA_OUT, (switch_mmio_read32(car, PLLA_OUT) & ~0xf000ff03u) | (out0Div << 8));
        switch_mmio_write32(car, PLLA_OUT, switch_mmio_read32(car, PLLA_OUT) | 3u);
    }

    // PLLA and the I2S and MCLK dividers under it for `clock`; nothing on them may run.
    void startClocks()
    {
        startPlla();
        switch_mmio_write32(car, CLK_SOURCE_I2S0, switch_audio_i2s_divider(clock->out0_per_fs));         // PLLA_OUT0
        switch_mmio_write32(car, CLK_SOURCE_EXTERN1, switch_audio_extern1_divider(clock->out0_per_fs));  // PLLA_OUT0
    }

    void tegraClocksAndPads()
    {
        // Every rate below assumes the 38.4 MHz oscillator as PLL reference (the T210B01 PLLA table has only that).
        const uint32_t osc = switch_mmio_read32(car, OSC_CTRL);
        if ((osc >> 28) != 5 || ((osc >> 26) & 3))
            panic("SwitchAudio: unexpected OSC_CTRL %#x (want 38.4 MHz, ref /1)", osc);
        // The APE partition (AHUB, ADMA, AGIC) is powered at boot and nothing here gates it; un-gating it would need the
        // MC flush and SLCG sequence of powergate-t21x.c, so a gated partition is reported, not handled.
        if (!(switch_mmio_read32(pmc, PWRGATE_STATUS) & kPartitionAud)) {
            panic("SwitchAudio: APE power partition is off (PMC PWRGATE_STATUS %#x)",
                  switch_mmio_read32(pmc, PWRGATE_STATUS));
        }
        if ((switch_mmio_read32(pmc, IO_DPD_STATUS) & (1u << 17)) ||
            (switch_mmio_read32(pmc, IO_DPD2_STATUS) & (1u << 29))) {
            panic("SwitchAudio: audio pads in deep power-down (IO_DPD_STATUS %#x IO_DPD2_STATUS %#x)",
                  switch_mmio_read32(pmc, IO_DPD_STATUS), switch_mmio_read32(pmc, IO_DPD2_STATUS));
        }
        // DAP1 is on the audio-hv rail, AUD_MCLK on audio; both are vdd-1v8 (MAX77620 SD3), DTB iopad-defaults 1.8 V.
        // mainline tegra_io_pad_set_voltage: write-enable the bit in PWR_DET, then clear it in PWR_DET_VALUE.
        if (switch_mmio_read32(pmc, PWR_DET_VAL) & kPad33V) {
            switch_mmio_write32(pmc, PWR_DET, switch_mmio_read32(pmc, PWR_DET) | kPad33V);
            switch_mmio_write32(pmc, PWR_DET_VAL, switch_mmio_read32(pmc, PWR_DET_VAL) & ~kPad33V);
            IODelay(250);
        }
        if (switch_mmio_read32(pmc, NO_IOPOWER) & kNoIoPowerAudioHv)
            switch_mmio_write32(pmc, NO_IOPOWER, switch_mmio_read32(pmc, NO_IOPOWER) & ~kNoIoPowerAudioHv);

        startClocks();
        switch_mmio_write32(car, CLK_SOURCE_APE, kApeSource);
        switch_mmio_write32(car, CLK_SOURCE_D_AUDIO, 0);  // PLLA_OUT0 (mux_d_audio_clk index 0), 1:1
        switch_mmio_write32(car, CLK_ENB_Y_SET, kClkY);
        switch_mmio_write32(car, CLK_ENB_V_SET, kClkV);
        switch_mmio_write32(car, CLK_ENB_L_SET, kClkL);
        IODelay(2);
        switch_mmio_write32(car, RST_DEV_Y_CLR, kClkY);  // hekate hw_init leaves APE in reset
        switch_mmio_write32(car, RST_DEV_V_CLR, kClkV);
        switch_mmio_write32(car, RST_DEV_L_CLR, kClkL);
        (void)switch_mmio_read32(car, RST_DEV_L_CLR);
        IODelay(2);
        // MCLK: clk_out_1 <- extern1, forced on (clk-tegra-pmc.c PMC_CLK(1, 6, 2)).
        switch_mmio_write32(pmc, CLK_OUT_CNTRL,
                            (switch_mmio_read32(pmc, CLK_OUT_CNTRL) & ~kClkOut1Mask) | kClkOut1Extern1);
        jackPinFound = switch_mmio_read32(pinmux, kPinJackDetect);
        for (const Pin &pin : kPins) {
            switch_mmio_write32(pinmux, pin.offset, (switch_mmio_read32(pinmux, pin.offset) & ~kPinFields) | pin.value);
        }
    }

    void tegraAhubAndDma()
    {
        switch_mmio_write32(ahub, ADMAIF_GLOBAL_ENABLE, 1);
        for (uint32_t part = 0; part < 3; ++part) {
            switch_mmio_write32(ahub, XBAR_I2S1 + part * XBAR_PART, part ? 0 : kXbarFromAdmaif1);
            switch_mmio_write32(ahub, XBAR_ADMAIF1 + part * XBAR_PART, part ? 0 : kXbarFromI2s1);
        }
        // tegra_adma_init: clear interrupts, soft reset, enable.
        switch_mmio_write32(adma, GLOBAL_INT_CLEAR, 1);
        switch_mmio_write32(adma, GLOBAL_SOFT_RESET, 1);
        if (!switch_mmio_wait32(adma, GLOBAL_SOFT_RESET, 1, 0, 10000))
            panic("SwitchAudio: ADMA soft reset did not finish");
        switch_mmio_write32(adma, GLOBAL_CMD, 1);
    }

    // The two ADMA channel SPIs on the AGIC, as Linux irq-gic.c sets up a distributor and CPU interface (priority
    // 0xa0, level, the reading CPU interface as target, PMR 0xf0, bypass bits kept). A refused enable panics.
    void tegraAgic()
    {
        const uint32_t mask =
            switch_mmio_read32(agic, GICD_ITARGETSR) & 0xff;  // banked: the interface our accesses come from
        if (!mask)
            panic("SwitchAudio: AGIC CPU interface mask reads 0 (GICD_CTLR %#x)", switch_mmio_read32(agic, GICD_CTLR));
        const unsigned channels = tegraCapture ? 2 : 1;
        for (unsigned ch = 0; ch < channels; ++ch) {
            const uint32_t id = kAgicAdma + ch, bit = 1u << (id % 32), word = 4 * (id / 32), shift = 8 * (id % 4);
            switch_mmio_write32(agic, GICD_ICENABLER + word, bit);
            switch_mmio_write32(agic, GICD_ICPENDR + word, bit);
            const uint32_t byteReg = id & ~3u;
            switch_mmio_write32(agic, GICD_IPRIORITYR + byteReg,
                                (switch_mmio_read32(agic, GICD_IPRIORITYR + byteReg) & ~(0xffu << shift)) |
                                    0xa0u << shift);
            switch_mmio_write32(agic, GICD_ITARGETSR + byteReg,
                                (switch_mmio_read32(agic, GICD_ITARGETSR + byteReg) & ~(0xffu << shift)) |
                                    mask << shift);
            const uint32_t cfg = GICD_ICFGR + 4 * (id / 16);
            switch_mmio_write32(agic, cfg, switch_mmio_read32(agic, cfg) & ~(2u << (2 * (id % 16))));  // level
        }
        switch_mmio_write32(agic, GICD_CTLR, switch_mmio_read32(agic, GICD_CTLR) | 1);
        switch_mmio_write32(agic, GICC_PMR, 0xf0);
        switch_mmio_write32(agic, GICC_CTLR, (switch_mmio_read32(agic, GICC_CTLR) & 0x1e0) | 1);
        for (unsigned ch = 0; ch < channels; ++ch) {
            const uint32_t id = kAgicAdma + ch, bit = 1u << (id % 32), word = 4 * (id / 32);
            switch_mmio_write32(agic, GICD_ISENABLER + word, bit);
            if (!(switch_mmio_read32(agic, GICD_ISENABLER + word) & bit)) {
                panic("SwitchAudio: AGIC refused SPI %u (GICD_CTLR %#x GICC_CTLR %#x)", id - 32,
                      switch_mmio_read32(agic, GICD_CTLR), switch_mmio_read32(agic, GICC_CTLR));
            }
        }
    }

    void tegraInit()
    {
        car = map(kCarPA, 0x1000, "CAR");
        pmc = map(kPmcPagePA, 0x1000, "PMC") + kPmcOffset;
        pinmux = map(kPinmuxPA, 0x1000, "pinmux");
        ahub = map(kAhubPA, 0x2000, "AHUB");
        adma = map(kAdmaPA, 0x1000, "ADMA");
        agic = map(kAgicPA, 0x2000, "AGIC");
        for (uint32_t type = SwitchAudioMemoryOutput; type <= SwitchAudioMemoryInput; ++type) {
            IOByteCount length = 0;
            const addr64_t pa = memory[type]->getPhysicalSegment(0, &length, kIOMemoryMapperNone);
            const uint32_t size = type == SwitchAudioMemoryOutput ? kOutBytes : kInBytes;
            if (!pa || length < size || pa + size > 0x100000000ULL)
                panic("SwitchAudio: ring %u at %#llx is not DMA-able", type, pa);
            ringPA[type] = (uint32_t)pa;
        }
        tegraClocksAndPads();
        tegraAhubAndDma();
        tegraAgic();
        IOService *nub = switch_interrupt_nub(kCascadeIntid, false);  // sleeps until the GIC is up
        cascade = IOInterruptEventSource::interruptEventSource(this, &SwitchAudio::cascadeFired, nub, 0);
        if (!cascade || loop->addEventSource(cascade) != kIOReturnSuccess) {
            panic("SwitchAudio: cannot attach the APE interrupt (INTID %u)", kCascadeIntid);
        }
        cascade->enable();
    }

    void softReset(uint32_t reg, const char *what)
    {
        switch_mmio_write32(ahub, reg, 1);
        if (!switch_mmio_wait32(ahub, reg, 1, 0, 10000))
            panic("SwitchAudio: %s soft reset did not finish", what);
    }

    // tegra210_i2s_alt.c tegra210_i2s_sw_reset: the reset clears the CIF and stream controls and I2S_CTRL, so they are
    // saved around it. It completes on the I2S's interface clock, so I2S_CTRL must already say master (the bit clock then
    // comes from the CAR i2s0 clock; as a slave the I2S would wait for one from the codec, which is a slave too).
    void i2sSoftReset(uint32_t reg, const char *what)
    {
        const uint32_t saved[] = {I2S_TX_CIF, I2S_RX_CIF, I2S_TX_CTRL, I2S_RX_CTRL, I2S_CTRL};
        uint32_t values[5];
        for (unsigned i = 0; i < 5; ++i)
            values[i] = switch_mmio_read32(ahub, saved[i]);
        switch_mmio_write32(ahub, reg, 1);
        if (!switch_mmio_wait32(ahub, reg, 1, 0, 10000)) {
            panic("SwitchAudio: %s soft reset did not finish (I2S CTRL %#x EN %#x TIMING %#x RX %#x TX %#x; CAR I2S0 "
                  "src %#x "
                  "enb %u rst %u PLLA %#x OUT %#x)",
                  what, switch_mmio_read32(ahub, I2S_CTRL), switch_mmio_read32(ahub, I2S_ENABLE),
                  switch_mmio_read32(ahub, I2S_TIMING), switch_mmio_read32(ahub, I2S_RX_STATUS),
                  switch_mmio_read32(ahub, I2S_TX_STATUS), switch_mmio_read32(car, CLK_SOURCE_I2S0),
                  !!(switch_mmio_read32(car, CLK_OUT_ENB_L) & kClkL),
                  !!(switch_mmio_read32(car, RST_DEVICES_L) & kClkL), switch_mmio_read32(car, PLLA_BASE),
                  switch_mmio_read32(car, PLLA_OUT));
        }
        for (unsigned i = 0; i < 5; ++i)
            switch_mmio_write32(ahub, saved[i], values[i]);
    }

    void stopUnit(uint32_t enable, uint32_t status, const char *what)
    {
        switch_mmio_write32(ahub, enable, 0);
        if (!switch_mmio_wait32(ahub, status, 1, 0, 10000))
            panic("SwitchAudio: %s did not stop (status %#x)", what, switch_mmio_read32(ahub, status));
    }

    void tegraStart()
    {
        if (hardwareLost)
            return;  // the play position stands still: the plug-in sees no progress, never noise
        switch_audio_sample_t *out = ring(SwitchAudioMemoryOutput);
        bzero(out, kOutBytes);
        switch_dcache_clean(out, kOutBytes);
        control->out_frames = 0;
        control->in_frames = 0;
        control->in_anchor = SWITCH_AUDIO_NO_CAPTURE;
        control->out_written = 0;
        // I2S1 and ADMAIF1, each session in L4T's order (tegra210_i2s_alt.c): set_fmt (CTRL with master mode, I2S_EN),
        // hw_params (timing, data offset, CIF), then the DAPM PRE_PMU sw reset of each direction it powers up
        // (tegra210_i2s_rx_stop/tx_stop), which keeps that configuration and needs master mode (i2sSoftReset). Then
        // the ADMAIF resets and tegra_admaif_hw_params.
        switch_mmio_write32(ahub, I2S_CTRL, kI2sCtrl);
        switch_mmio_write32(ahub, I2S_ENABLE, 1);
        switch_mmio_write32(ahub, I2S_TIMING, kI2sTiming);
        switch_mmio_write32(ahub, I2S_RX_CTRL, kI2sDataOffset);
        switch_mmio_write32(ahub, I2S_TX_CTRL, kI2sDataOffset);
        switch_mmio_write32(ahub, I2S_RX_CIF, switch_audio_i2s_rx_cif());  // playback: XBAR stereo -> DAP stereo
        // capture: DAP stereo -> XBAR mono (left: IN1 is on both)
        switch_mmio_write32(ahub, I2S_TX_CIF, switch_audio_i2s_tx_cif());
        // Both directions reset here, before either runs: a TX reset later would also clear I2S_CTRL under running
        // playback (i2sSoftReset), so captureStart and captureStop leave it out.
        i2sSoftReset(I2S_RX_SOFT_RESET, "I2S1 RX");
        if (tegraCapture)
            i2sSoftReset(I2S_TX_SOFT_RESET, "I2S1 TX");
        softReset(ADMAIF_TX_SOFT_RESET, "ADMAIF1 TX");
        softReset(ADMAIF_RX_SOFT_RESET, "ADMAIF1 RX");
        switch_mmio_write32(ahub, ADMAIF_TX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_OUT_CHANNELS));
        programChannel(kChOut);
        switch_mmio_write32(ahub, I2S_RX_ENABLE, 1);
        switch_mmio_write32(ahub, ADMAIF_TX_ENABLE, 1);
        // Sample time 0 is ring frame 0 leaving memory. tegra_adma_start brackets the start with the global clock gate.
        control->anchor_host = mach_absolute_time();
        switch_mmio_write32(adma, GLOBAL_CG, 0);
        switch_mmio_write32(adma, CH_CMD, 1);
        switch_mmio_write32(adma, GLOBAL_CG, 7);
        periods[kChOut] = 0;
        xferSeen[kChOut] = (uint16_t)switch_mmio_read32(adma, CH_XFER_STATUS);
        graced = false;
        timer->setTimeoutMS(watchdogMS());
#if DEVELOPMENT || DEBUG
        ++statStarts;
#endif
    }

    void programChannel(unsigned ch)
    {
        const uint32_t base = ch * CH_STRIDE;
        switch_mmio_write32(adma, base + CH_CMD, 0);
        switch_mmio_write32(adma, base + CH_INT_CLEAR, 1);
        switch_mmio_write32(adma, base + CH_TC, ch == kChOut ? kOutPeriodBytes : kInPeriodBytes);
        switch_mmio_write32(adma, base + CH_CTRL, kChCtrl[ch]);
        switch_mmio_write32(adma, base + CH_SRC, ch == kChOut ? ringPA[SwitchAudioMemoryOutput] : 0);
        switch_mmio_write32(adma, base + CH_TRG, ch == kChOut ? 0 : ringPA[SwitchAudioMemoryInput]);
        switch_mmio_write32(adma, base + CH_FIFO_CTRL, kChFifo[ch]);
        switch_mmio_write32(adma, base + CH_CONFIG, kChConfig[ch]);
    }

    // Input requested with the I2S running (tegraStart, or a stream already playing): ADMAIF1 RX, I2S TX and ADMA
    // ch1 from capture ring frame 0, which holds the sample time captureStartFrame (in_anchor for the plug-in).
    void captureStart()
    {
        if (!tegraCapture || captureRunning || hardwareLost)
            return;
        switch_audio_sample_t *in = ring(SwitchAudioMemoryInput);
        bzero(in, kInBytes);
        switch_dcache_clean_invalidate(in, kInBytes);
        softReset(ADMAIF_RX_SOFT_RESET, "ADMAIF1 RX");
        switch_mmio_write32(ahub, ADMAIF_RX_CIF, switch_audio_admaif_cif(SWITCH_AUDIO_IN_CHANNELS));
        programChannel(kChIn);
        switch_mmio_write32(ahub, I2S_TX_ENABLE, 1);
        switch_mmio_write32(ahub, ADMAIF_RX_ENABLE, 1);
        captureStartFrame = nowFrame();
        control->in_frames = captureStartFrame;
        __atomic_store_n(&control->in_anchor, captureStartFrame, __ATOMIC_RELEASE);
        switch_mmio_write32(adma, kChIn * CH_STRIDE + CH_CMD, 1);
        periods[kChIn] = 0;
        xferSeen[kChIn] = (uint16_t)switch_mmio_read32(adma, kChIn * CH_STRIDE + CH_XFER_STATUS);
        captureRunning = true;
    }

    // tegraStop's capture half: DMA, ADMAIF, I2S TX; playback, if it runs, goes on.
    void captureStop()
    {
        if (!captureRunning)
            return;
        captureRunning = false;
        __atomic_store_n(&control->in_anchor, SWITCH_AUDIO_NO_CAPTURE, __ATOMIC_RELEASE);
        const uint32_t base = kChIn * CH_STRIDE;
        switch_mmio_write32(adma, base + CH_CMD, 0);
        if (!switch_mmio_wait32(adma, base + CH_STATUS, 1, 0, 10000))
            panic("SwitchAudio: ADMA ch1 did not stop (STATUS %#x)", switch_mmio_read32(adma, base + CH_STATUS));
        switch_mmio_write32(adma, base + CH_INT_CLEAR, 1);
        stopUnit(ADMAIF_RX_ENABLE, ADMAIF_RX_STATUS, "ADMAIF1 RX");
        softReset(ADMAIF_RX_SOFT_RESET, "ADMAIF1 RX");
        stopUnit(I2S_TX_ENABLE, I2S_TX_STATUS, "I2S1 TX");
    }

    // tegra_adma_stop, tegra_admaif_stop, the I2S widgets' power-down: DMA, then ADMAIF, then I2S.
    void tegraStop()
    {
        timer->cancelTimeout();
        if (hardwareLost)
            return;  // tegraStart started nothing; a powered-off APE must not be accessed
        captureStop();
        switch_mmio_write32(adma, CH_CMD, 0);
        if (!switch_mmio_wait32(adma, CH_STATUS, 1, 0, 10000))
            panic("SwitchAudio: ADMA ch0 did not stop (STATUS %#x)", switch_mmio_read32(adma, CH_STATUS));
        switch_mmio_write32(adma, CH_INT_CLEAR, 1);
        stopUnit(ADMAIF_TX_ENABLE, ADMAIF_TX_STATUS, "ADMAIF1 TX");
        softReset(ADMAIF_TX_SOFT_RESET, "ADMAIF1 TX");
        stopUnit(I2S_RX_ENABLE, I2S_RX_STATUS, "I2S1 RX");
        switch_mmio_write32(ahub, I2S_ENABLE,
                            0);  // the bit and frame clocks stop until the next session (runtime suspend in Linux)
    }

    // Period n of the playback ring has left memory: clear it for the next turn and move out_frames past it. A DMA
    // that finishes periods sooner than the 48 kHz clock allows is not paced by the I2S: panic rather than play noise.
    void playedPeriod()
    {
        const uint64_t n = periods[kChOut]++;
        switch_audio_sample_t *slot =
            ring(SwitchAudioMemoryOutput) + (n % kOutPeriods) * kOutPeriodFrames * SWITCH_AUDIO_OUT_CHANNELS;
        const uint64_t done = (n + 1) * kOutPeriodFrames, now = nowFrame();
#if DEVELOPMENT || DEBUG
        if (statsTimer)
            notePlayed(slot, done);
#endif
        bzero(slot, kOutPeriodBytes);
        switch_dcache_clean(slot, kOutPeriodBytes);
        control->out_frames = done;
        if (done > now + kOutPeriodFrames) {
            panic(
                "SwitchAudio: ADMA ch0 runs fast: %llu frames played at clock frame %llu (I2S %#x CTRL %#x TIMING %#x)",
                done, now, switch_mmio_read32(ahub, I2S_RX_STATUS), switch_mmio_read32(ahub, I2S_CTRL),
                switch_mmio_read32(ahub, I2S_TIMING));
        }
    }

#if DEVELOPMENT || DEBUG
    // The stats: the period that just played, before it is cleared: its loudest sample (in 16-bit units:
    // 32767 is full scale), an underrun when the plug-in had not written up to its end, how far past it the plug-in
    // had written (the write-ahead margin: the safety offset in use, less the IO thread's lateness), and how late this
    // interrupt runs after the period's end on the I2S clock model the plug-in's zero time stamps use
    // (anchor_host + frames * ticks_per_frame_q32). The model and the host timer both run from the
    // 38.4 MHz oscillator, so the lag only holds the DMA's start offset and the interrupt latency: one that drifts
    // over a session means the time stamps do. The first kSettlePeriods of a start stay out of lead and lag (the audio
    // server's first IO cycle comes after them, so "under" counts one per start). (The plug-in's writes are already in
    // this CPU-coherent mapping.)
    void notePlayed(const switch_audio_sample_t *slot, uint64_t done)
    {
        statFrames += kOutPeriodFrames;
        const bool settled = done > kSettlePeriods * kOutPeriodFrames;
        if (settled) {
            const int64_t lagTicks = (int64_t)(mach_absolute_time() - control->anchor_host) -
                                     (int64_t)((done / kOutPeriodFrames) * periodTicksQ16 >> 16);
            const int64_t lagUs = lagTicks * 1000000 / (int64_t)hz;
            if (lagUs < statLagMin)
                statLagMin = lagUs;
            if (lagUs > statLagMax)
                statLagMax = lagUs;
        }
        if (!(control->state & SwitchAudioOutputRunning))
            return;
        const int64_t lead = (int64_t)control->out_written - (int64_t)done;
        if (settled && lead < statLeadMin)
            statLeadMin = lead;
        if (lead < 0)
            ++statUnderruns;
        int peak = statPeak;
        for (uint32_t i = 0; i < kOutPeriodFrames * SWITCH_AUDIO_OUT_CHANNELS; ++i) {
            const int sample = slot[i] >> 16;  // -32768..32767
            const int magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak)
                peak = magnitude;
        }
        statPeak = peak;
    }
#endif

    // Audio work loop: the capture channel delivered no period in two playback periods, on its first run since
    // start. Capture stops for the boot, playback goes on, and the microphone reads as absent (publishJack).
    void captureOff()
    {
        const uint32_t in = kChIn * CH_STRIDE, id = kAgicAdma + kChIn;
        IOLog("SwitchAudio: headset microphone off for this boot: ADMA ch1 delivered no period in %llu frames "
              "(XFER %u STATUS %#x INT_STATUS %#x, ADMAIF RX %#x, I2S TX %#x)\n",
              nowFrame() - captureStartFrame, switch_mmio_read32(adma, in + CH_XFER_STATUS) & 0xffff,
              switch_mmio_read32(adma, in + CH_STATUS), switch_mmio_read32(adma, in + CH_INT_STATUS),
              switch_mmio_read32(ahub, ADMAIF_RX_STATUS), switch_mmio_read32(ahub, I2S_TX_STATUS));
        switch_mmio_write32(adma, in + CH_CMD, 0);
        switch_mmio_write32(adma, in + CH_INT_CLEAR, 1);
        switch_mmio_write32(agic, GICD_ICENABLER + 4 * (id / 32), 1u << (id % 32));
        switch_mmio_write32(ahub, ADMAIF_RX_ENABLE, 0);  // not waited for: nothing here starts them again this boot
        switch_mmio_write32(ahub, I2S_TX_ENABLE, 0);
        tegraCapture = false;
        captureRunning = false;
        __atomic_store_n(&control->in_anchor, SWITCH_AUDIO_NO_CAPTURE, __ATOMIC_RELEASE);
        codecStep(kCodecNoMicrophone);
    }

    // Period n of the capture ring is in memory: drop what the CPUs cached of it, then publish it.
    void capturedPeriod()
    {
        captureProven = true;
        const uint64_t n = periods[kChIn]++;
        switch_dcache_clean_invalidate(ring(SwitchAudioMemoryInput) +
                                           (n % kInPeriods) * kInPeriodFrames * SWITCH_AUDIO_IN_CHANNELS,
                                       kInPeriodBytes);
        control->in_frames = captureStartFrame + (n + 1) * kInPeriodFrames;
    }

    // One ADMA channel's interrupt: acknowledge it and account every period its completed-period counter
    // (ADMA_CH_XFER_STATUS, as tegra_adma_get_residue reads it) has moved past, so a late interrupt loses nothing.
    void channelInterrupt(unsigned ch)
    {
        const uint32_t base = ch * CH_STRIDE;
        const uint32_t status = switch_mmio_read32(adma, base + CH_INT_STATUS);
        if (status)
            switch_mmio_write32(adma, base + CH_INT_CLEAR, status);
        if (!control->state || (ch == kChIn && !captureRunning))
            return;  // the period that ended as the stream stopped
        const uint16_t xfer = (uint16_t)switch_mmio_read32(adma, base + CH_XFER_STATUS);
        uint32_t done = (uint16_t)(xfer - xferSeen[ch]);
        xferSeen[ch] = xfer;
        if (done > (ch == kChOut ? kOutPeriods : kInPeriods)) {
            panic("SwitchAudio: ADMA ch%u completed %u periods in one interrupt (XFER_STATUS %#x): lost track", ch,
                  done, xfer);
        }
        for (; done; --done)
            ch == kChOut ? playedPeriod() : capturedPeriod();
        if (ch != kChOut)
            return;
        graced = false;
        timer->setTimeoutMS(watchdogMS());
        // Capture runs on the same clock from its own start: two playback periods behind the clock, its interrupt is lost
        // or its DMA stalled.
        if (!captureRunning)
            return;
        const uint64_t now = nowFrame(), captured = periods[kChIn] * kInPeriodFrames;
        const uint64_t played = now > captureStartFrame ? now - captureStartFrame : 0;
        if (!captureProven && played > 2 * kOutPeriodFrames) {
            captureOff();
            return;
        }
        if (played > captured + 2 * kOutPeriodFrames) {
            const uint32_t in = kChIn * CH_STRIDE, id = kAgicAdma + kChIn, bit = 1u << (id % 32), word = 4 * (id / 32);
            panic("SwitchAudio: ADMA ch1 behind: %llu periods vs %llu, XFER %u seen %u INT_STATUS %#x STATUS %#x, AGIC "
                  "pend %u, "
                  "ADMAIF RX %#x I2S TX %#x",
                  periods[kChIn], periods[kChOut], switch_mmio_read32(adma, in + CH_XFER_STATUS) & 0xffff,
                  xferSeen[kChIn], switch_mmio_read32(adma, in + CH_INT_STATUS),
                  switch_mmio_read32(adma, in + CH_STATUS), !!(switch_mmio_read32(agic, GICD_ISPENDR + word) & bit),
                  switch_mmio_read32(ahub, ADMAIF_RX_STATUS), switch_mmio_read32(ahub, I2S_TX_STATUS));
        }
    }

    // Work loop action for INTID 134 (level; the GIC masks it while this runs): drain the AGIC.
    static void cascadeFired(OSObject *owner, IOInterruptEventSource *, int)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        for (;;) {
            const uint32_t iar = switch_mmio_read32(self->agic, GICC_IAR), id = iar & 0x3ff;
            if (id >= 1020)
                break;
            if (id == kAgicAdma + kChOut || (self->tegraCapture && id == kAgicAdma + kChIn)) {
                self->channelInterrupt(id - kAgicAdma);
            } else {
                switch_mmio_write32(self->agic, GICD_ICENABLER + 4 * (id / 32),
                                    1u << (id % 32));  // not ours: keep it off
                IOLog("SwitchAudio: AGIC interrupt %u is not ours, disabled\n", id);
            }
            switch_mmio_write32(self->agic, GICC_EOIR, iar);
        }
    }

    // No playback period interrupt for 4 periods. If the ADMA moved on, its interrupt is not reaching us (after a grace
    // period for an action already queued); if it did not, the DMA is stalled (no I2S clock pulling data). Either way,
    // one line with the state.
    static void watchdogFired(OSObject *owner, IOTimerEventSource *sender)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (!self->control->state)
            return;
        volatile uint8_t *dma = self->adma, *gic = self->agic;
        const uint32_t xfer = switch_mmio_read32(dma, CH_XFER_STATUS) & 0xffff;
        if (xfer != self->xferSeen[kChOut] && !self->graced) {
            self->graced = true;
            sender->setTimeoutMS(kGraceMS);
            return;
        }
        if (xfer != self->xferSeen[kChOut]) {
            char state[160];
            switch_interrupt_describe(kCascadeIntid, state, sizeof(state));
            const uint32_t id = kAgicAdma + kChOut, bit = 1u << (id % 32), word = 4 * (id / 32);
            panic("SwitchAudio: ADMA ch0 period interrupt lost: XFER %u seen %u INT_STATUS %#x, AGIC en %u pend %u act "
                  "%u, %s",
                  xfer, self->xferSeen[kChOut], switch_mmio_read32(dma, CH_INT_STATUS),
                  !!(switch_mmio_read32(gic, GICD_ISENABLER + word) & bit),
                  !!(switch_mmio_read32(gic, GICD_ISPENDR + word) & bit),
                  !!(switch_mmio_read32(gic, GICD_ISACTIVER + word) & bit), state);
        }
        panic(
            "SwitchAudio: ADMA ch0 stalled at period %llu: STATUS %#x TC_STATUS %#x, ADMAIF TX %#x, I2S RX %#x EN %#x",
            self->periods[kChOut], switch_mmio_read32(dma, CH_STATUS), switch_mmio_read32(dma, CH_TC_STATUS),
            switch_mmio_read32(self->ahub, ADMAIF_TX_STATUS), switch_mmio_read32(self->ahub, I2S_RX_STATUS),
            switch_mmio_read32(self->ahub, I2S_ENABLE));
    }

    rt5639::Route wantedRoute() const
    {
        return jackIn && !speakerOnly ? rt5639::kRouteHeadphones : rt5639::kRouteSpeaker;
    }

    // A stream starts: the codec powered (bias, DAC, capture path) and routed to where the jack says, still muted.
    void codecPowerUp()
    {
        idleTimer->cancelTimeout();
#if DEVELOPMENT || DEBUG
        statsResume();
#endif
        tegra_gpio_check_delivery(kCodecIrq, "SwitchAudio");  // before trusting the jack state
        if (!codecOn) {
            codec.biasStandby();
            codec.powerDac();
            codecOn = true;
        }
        const rt5639::Route want = wantedRoute();
        if (codec.route() != want) {
            codec.routeDown();
            codec.routeUp(want);
        }
    }

    // Route, DAC and references off, outputs muted first (rt5640.c's PRE_PMD steps, then bias OFF).
    // The headset microphone's path (BST1, the REC mixers, the ADC, MICBIAS1 for a headset) only while input runs.
    void captureCodec(bool on)
    {
        if (on && tegraCapture && codecOn && !captureCodecOn) {
            codec.micBias(headset);
            codec.powerCapture();
            captureCodecOn = true;
        } else if (!on && captureCodecOn) {
            codec.powerCaptureOff();
            codec.micBias(false);
            captureCodecOn = false;
        }
    }

    void codecPowerDown()
    {
        if (codecOn)
            codec.routeDown();
        if (codec.biasIsOn())
            codec.biasOff();
        codecOn = false;
        captureCodecOn = false;
    }

    enum CodecStep : uintptr_t {
        kCodecPowerUp,
        kCodecUnmute,
        kCodecMute,
        kCodecPause,           // the last stream stopped: muted now, powered down after kIdleMS unless one starts again
        kCodecIdleNow,         // no stream: powered down now (before its MCLK changes), not kIdleMS later
        kCodecHeadphoneLevel,  // headphoneSteps
        kCodecNoMicrophone,    // capture turned off for the boot
        kCodecCaptureOn,
        kCodecCaptureOff,
    };

    static IOReturn codecGated(OSObject *owner, void *step, void *, void *, void *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (self->codecOff)
            return kIOReturnSuccess;
        switch ((uintptr_t)step) {
        case kCodecPowerUp:
            self->codecPowerUp();
            break;
        case kCodecUnmute:
            if (self->control->state)
                self->codec.mute(false);
            break;
        case kCodecMute:
            self->codec.mute(true);
            break;
        case kCodecPause:
            self->codec.mute(true);
            self->idleTimer->setTimeoutMS(kIdleMS);
            break;
        case kCodecIdleNow:
            self->idleTimer->cancelTimeout();
            codecIdle(self, self->idleTimer);
            break;
        case kCodecHeadphoneLevel:
            self->codec.setHeadphoneAttenuation(__atomic_load_n(&self->headphoneSteps, __ATOMIC_RELAXED));
            if (self->control->state)
                self->codec.mute(false);
            break;
        case kCodecNoMicrophone:
            self->captureCodec(false);
            self->codec.micBias(false);
            self->publishJack();
            break;
        case kCodecCaptureOn:
            self->captureCodec(true);
            break;
        case kCodecCaptureOff:
            self->captureCodec(false);
            break;
        default:
            break;
        }
        return kIOReturnSuccess;
    }

    void codecStep(CodecStep step)
    {
        codecGate->runAction(&SwitchAudio::codecGated, reinterpret_cast<void *>(step));
    }

    static void codecIdle(OSObject *owner, IOTimerEventSource *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (self->control->state || self->codecOff || !self->codec.biasIsOn())
            return;
        tegra_gpio_check_delivery(kCodecIrq, "SwitchAudio");
        self->codecPowerDown();
    }

    // PBB4, both edges (rt5640_irq: IRQF_TRIGGER_RISING | FALLING): wait for the contacts to settle.
    static void jackEdge(OSObject *owner, IOInterruptEventSource *, int)
    {
        static_cast<SwitchAudio *>(owner)->jackTimer->setTimeoutMS(kJackSettleMS);
    }

    // Codec work loop, after the jack settled: whether the plug carries a microphone (rt5640_detect_headset needs the
    // references, which come up for it and go down kIdleMS later), then the jack's state to the plug-in.
    void noteJack()
    {
        if (tegraCapture && jackIn && !codec.biasIsOn()) {
            codec.biasStandby();
            if (!control->state)
                idleTimer->setTimeoutMS(kIdleMS);
        }
        headset = tegraCapture && codec.detectHeadset(jackIn);
        if (!captureCodecOn || !headset)
            codec.micBias(false);
        publishJack();
    }

    void publishJack()
    {
        const uint32_t jack =
            (jackIn ? SwitchAudioJackHeadphones : 0u) | (headset && tegraCapture ? SwitchAudioJackMicrophone : 0u);
        if (control->jack == jack)
            return;
        control->jack = jack;
        messageClients(kIOMessageServicePropertyChange);
    }

    // Also the first reading, kJackSettleMS after codecStart; until then the jack counts as empty (the speaker).
    static void jackSettled(OSObject *owner, IOTimerEventSource *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        const uint16_t status = self->codec.read(rt5639::INT_IRQ_ST);
        const bool in = SwitchRT5639::jackPlugged(status);
        const bool first = !self->jackKnown;
        if (first) {
            self->jackKnown = true;
            IOLog("SwitchAudio: headphones %s at start\n", in ? "in" : "out");
        } else if (in != self->jackIn) {
            IOLog("SwitchAudio: headphones %s\n", in ? "plugged in" : "unplugged");
        } else {
            return;
        }
        self->jackIn = in;
        // The references come up here once with every output off, and go down kIdleMS later as after a stream. At
        // boot the VREF capacitor starts from 0 V: charged by the first stream instead, its 15 ms of fast charge left
        // the reference still rising when the class-D amplifier came on, the loud click of the first sound after
        // boot. Later power-ups start from a charged capacitor.
        if (first && !self->codec.biasIsOn()) {
            self->codec.biasStandby();
            self->idleTimer->setTimeoutMS(kIdleMS);
        }
        self->noteJack();
        if (!self->codecOn)
            return;  // the next stream routes
        if (self->codec.route() == self->wantedRoute())
            return;
        self->codec.routeDown();
        self->codec.routeUp(self->wantedRoute());
        if (self->control->state)
            self->codec.mute(false);
    }

#if DEVELOPMENT || DEBUG
    // The stats line, every kStatsMS on the codec work loop (it reads the codec over I2C). "starts" counts the
    // streams the hardware started since the last line, "moved" what the playback DMA played (240000 a line while a
    // stream runs), "under" the periods that played before the plug-in had written their end, "peak" the loudest sample
    // since the last line (16-bit units; 0: only silence reached the DMA); "io" the plug-in's playback IO cycles in the
    // window and the frames each wrote (the audio server's wake-ups and IO buffer size), "lead" the least write-ahead
    // the plug-in had at a period's end (frames; under 0 is an underrun), "lag" the period interrupts' least and most
    // lateness against the I2S clock model in us (see notePlayed; "-" without a period); then, as read now, ADMA ch0
    // STATUS and completed periods, I2S1 enable and RX (playback) status, ADMAIF1 TX status, the jack (INT_IRQ_ST,
    // bit 4 set = empty; PV6, 1 = empty) and route, and PWR_DIG1, PWR_ANLG1, PWR_MIXER, PWR_VOL, SPK_VOL, HP_VOL,
    // DAC1_DIG_VOL.
    // The line runs while there is audio to report: the first window after boot, then from a stream's start
    // (codecPowerUp arms it) until a window with no stream, no start, nothing moved and the codec powered down; that
    // window reads nothing, writes no line and leaves the timer off, so a silent console takes no stats wakeups.
    static void statsFired(OSObject *owner, IOTimerEventSource *sender)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        const uint64_t frames = self->statFrames, moved = frames - self->statFramesShown;
        const uint32_t starts = self->statStarts - self->statStartsShown;
        if (self->statsShown && !self->control->state && !self->codecOn && !starts && !moved) {
            self->statsArmed = false;
            return;
        }
        self->statsShown = true;
        self->statFramesShown = frames;
        self->statStartsShown += starts;
        const int peak = self->statPeak;
        self->statPeak = 0;
        const uint64_t ioCycles = self->control->io_cycles, ioFrames = self->control->io_frames;
        const uint64_t cycles = ioCycles - self->statIoCyclesShown, written = ioFrames - self->statIoFramesShown;
        self->statIoCyclesShown = ioCycles;
        self->statIoFramesShown = ioFrames;
        char lead[24] = "-", lag[48] = "-";
        if (self->statLeadMin != INT64_MAX)
            snprintf(lead, sizeof(lead), "%lld", (long long)self->statLeadMin);
        if (self->statLagMax != INT64_MIN) {
            snprintf(lag, sizeof(lag), "%lld..%lld", (long long)self->statLagMin, (long long)self->statLagMax);
        }
        self->statLeadMin = INT64_MAX;
        self->statLagMin = INT64_MAX;
        self->statLagMax = INT64_MIN;
        static const char *const routes[] = {"none", "speaker", "headphones"};
        char line[416];
        snprintf(
            line, sizeof(line),
            "SwitchAudio stats: run %u starts %u moved %llu under %u peak %d io %llu x %llu f lead %s lag %s us | ADMA "
            "%#x xfer %u, I2S en %u rx %#x, ADMAIF tx %#x | jack %s st %#x pv6 %d, %s | codec %04x %04x %04x %04x vol "
            "%04x %04x dac %04x",
            self->control->state, starts, moved, self->statUnderruns, peak, cycles, cycles ? written / cycles : 0, lead,
            lag, switch_mmio_read32(self->adma, CH_STATUS), switch_mmio_read32(self->adma, CH_XFER_STATUS) & 0xffff,
            switch_mmio_read32(self->ahub, I2S_ENABLE), switch_mmio_read32(self->ahub, I2S_RX_STATUS),
            switch_mmio_read32(self->ahub, ADMAIF_TX_STATUS), self->jackIn ? "in" : "out",
            self->codec.read(rt5639::INT_IRQ_ST), tegra_gpio_read(kJackDetect), routes[self->codec.route()],
            self->codec.read(rt5639::PWR_DIG1), self->codec.read(rt5639::PWR_ANLG1),
            self->codec.read(rt5639::PWR_MIXER), self->codec.read(rt5639::PWR_VOL), self->codec.read(rt5639::SPK_VOL),
            self->codec.read(rt5639::HP_VOL), self->codec.read(rt5639::DAC1_DIG_VOL));
        os_log(OS_LOG_DEFAULT, "%s", line);  // log only, never the boot console
        sender->setTimeoutMS(kStatsMS);
    }

    // Codec work loop: a stream starts; the stats line resumes if it had stopped (statsFired).
    void statsResume()
    {
        if (!statsTimer || statsArmed)
            return;
        statsArmed = true;
        statsTimer->setTimeoutMS(kStatsMS);
    }
#endif

    void codecStart()
    {
        codecLoop = IOWorkLoop::workLoop();
        codecGate = codecLoop ? IOCommandGate::commandGate(this) : nullptr;
        jackTimer = codecGate ? IOTimerEventSource::timerEventSource(this, &SwitchAudio::jackSettled) : nullptr;
        idleTimer = jackTimer ? IOTimerEventSource::timerEventSource(this, &SwitchAudio::codecIdle) : nullptr;
        if (!idleTimer || codecLoop->addEventSource(codecGate) != kIOReturnSuccess ||
            codecLoop->addEventSource(jackTimer) != kIOReturnSuccess ||
            codecLoop->addEventSource(idleTimer) != kIOReturnSuccess) {
            panic("SwitchAudio: codec work loop setup failed");
        }
        codec.probe(false, tegraCapture);
        // PV6 as a GPIO input (CNF set, output off), so its IN bit is the pad's level; no event source, so its interrupt
        // stays masked: the codec's IRQ reports the jack.
        if (!tegra_gpio_interrupt_nub(kJackDetect, TEGRA_GPIO_EDGE_BOTH))
            panic("SwitchAudio: cannot make PV6 a GPIO input");
        // The jack is first read once its line has settled, with the IRQ already armed so no change is missed
        // meanwhile: read right after the setup above, JD_STATUS says plugged with nothing in the jack. (Linux reads it
        // from a work item and confirms a plug for a second: rt5640_detect_headset.)
        IOService *nub = tegra_gpio_interrupt_nub(kCodecIrq, TEGRA_GPIO_EDGE_BOTH);
        jackSource = nub ? IOInterruptEventSource::interruptEventSource(this, &SwitchAudio::jackEdge, nub, 0) : nullptr;
        if (!jackSource || codecLoop->addEventSource(jackSource) != kIOReturnSuccess) {
            panic("SwitchAudio: cannot attach the RT5639 IRQ (GPIO %u, PBB4)", kCodecIrq);
        }
        jackSource->enable();
        jackTimer->setTimeoutMS(kJackSettleMS);
#if DEVELOPMENT || DEBUG
        statsTimer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::statsFired);
        if (!statsTimer || codecLoop->addEventSource(statsTimer) != kIOReturnSuccess)
            panic("SwitchAudio: stats timer setup failed");
        statsResume();
#endif
    }

    static IOReturn requestGated(OSObject *owner, void *selector, void *, void *, void *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        SwitchAudioControl *c = self->control;
        const uint32_t was = c->state;
        switch ((uint32_t)(uintptr_t)selector) {
        case SwitchAudioStartOutput:
            if (!(was & SwitchAudioOutputRunning)) {
                if (!self->tegra) {
                    bzero(self->ring(SwitchAudioMemoryOutput), kOutBytes);
                    c->out_frames = self->nowFrame();
                }
                c->state = was | SwitchAudioOutputRunning;
            }
            break;
        case SwitchAudioStopOutput:
            c->state = was & ~(uint32_t)SwitchAudioOutputRunning;
            break;
        case SwitchAudioStartInput:
            if (!(was & SwitchAudioInputRunning)) {
                if (!self->tegra) {
                    c->in_frames = self->nowFrame();
                    __atomic_store_n(&c->in_anchor, c->in_frames, __ATOMIC_RELEASE);
                }
                c->state = was | SwitchAudioInputRunning;
            }
            break;
        case SwitchAudioStopInput:
            if (!self->tegra)
                __atomic_store_n(&c->in_anchor, SWITCH_AUDIO_NO_CAPTURE, __ATOMIC_RELEASE);
            c->state = was & ~(uint32_t)SwitchAudioInputRunning;
            break;
        default:
            return kIOReturnBadArgument;
        }
        // Both directions share one clock: the I2S and playback DMA run while either is requested, the capture path
        // only while input is.
        const uint32_t actions = switch_audio_transition(was, c->state);
        if (!self->tegra) {
            if (actions & SWITCH_AUDIO_HW_START)
                self->timer->setTimeoutMS(kTickMS);
            if (actions & SWITCH_AUDIO_HW_STOP)
                self->timer->cancelTimeout();
            return kIOReturnSuccess;
        }
        if (actions & SWITCH_AUDIO_HW_START)
            self->tegraStart();
        if (actions & SWITCH_AUDIO_CAPTURE_START)
            self->captureStart();
        if (actions & SWITCH_AUDIO_CAPTURE_STOP)
            self->captureStop();
        if (actions & SWITCH_AUDIO_HW_STOP)
            self->tegraStop();
        return kIOReturnSuccess;
    }

    // SC7 powers the APE partition and the codec's link down and resets the CAR, ADMA, AHUB and AGIC; the codec keeps
    // its supply only as LDO1_EN (PZ4) is held, so it is set up again from reset like at boot. The partition comes
    // back as powergate-t21x.c powers a partition on: modules in reset with clocks on, toggle, unclamp.
    bool apePartitionOn()
    {
        if (switch_mmio_read32(pmc, PWRGATE_STATUS) & kPartitionAud)
            return true;
        switch_mmio_write32(car, RST_DEV_Y_SET, kClkY);
        switch_mmio_write32(car, RST_DEV_V_SET, kClkV);
        switch_mmio_write32(car, RST_DEV_L_SET, kClkL);
        switch_mmio_write32(car, CLK_ENB_Y_SET, kClkY);
        switch_mmio_write32(car, CLK_ENB_V_SET, kClkV);
        switch_mmio_write32(car, CLK_ENB_L_SET, kClkL);
        IODelay(10);
        if (!switch_pmc_partition_on(kPartitionAudId))
            return false;
        IODelay(10);
        switch_pmc_remove_clamping(kPartitionAudId);
        return switch_mmio_wait32(pmc, CLAMP_STATUS, kPartitionAud, 0, 5000);
    }

    static IOReturn sleepGated(OSObject *owner, void *entering, void *, void *, void *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (entering) {
            if (self->control->state)
                self->tegraStop();  // iOS holds sleep off while audio plays; nothing runs here
            self->timer->cancelTimeout();
            self->cascade->disable();
            return kIOReturnSuccess;
        }
        if (!self->apePartitionOn()) {
            IOLog("SwitchAudio: APE partition did not power on after sleep (PMC PWRGATE_STATUS %#x CLAMP %#x); audio "
                  "off\n",
                  switch_mmio_read32(self->pmc, PWRGATE_STATUS), switch_mmio_read32(self->pmc, CLAMP_STATUS));
            self->hardwareLost = true;
            return kIOReturnSuccess;
        }
        self->tegraClocksAndPads();
        self->tegraAhubAndDma();
        self->tegraAgic();
        self->cascade->enable();
        if (self->control->state)
            self->tegraStart();
        if (self->control->state & SwitchAudioInputRunning)
            self->captureStart();
        return kIOReturnSuccess;
    }

    // The codec's interrupt and timers off, then the codec powered down.
    void codecQuiesce()
    {
        jackSource->disable();
        jackTimer->cancelTimeout();
        idleTimer->cancelTimeout();
#if DEVELOPMENT || DEBUG
        if (statsTimer)
            statsTimer->cancelTimeout();
#endif
        codecPowerDown();
    }

    static IOReturn codecSleepGated(OSObject *owner, void *entering, void *, void *, void *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (self->codecOff)
            return kIOReturnSuccess;
        if (entering) {
            self->codecQuiesce();
            return kIOReturnSuccess;
        }
        if (self->hardwareLost)
            return kIOReturnSuccess;
        self->codec.probe(true, self->tegraCapture);
        self->jackKnown = false;
        self->jackSource->enable();
        self->jackTimer->setTimeoutMS(kJackSettleMS);
#if DEVELOPMENT || DEBUG
        if (self->statsTimer)
            self->statsResume();
#endif
        if (self->control->state) {  // the I2S already runs again (sleepGated)
            self->codecPowerUp();
            self->codec.mute(false);
        }
        return kIOReturnSuccess;
    }

    // Shut Down and Restart, before the PMIC cuts the rails: the hardware stopped and nothing touched again.
    static IOReturn shutdownGated(OSObject *owner, void *, void *, void *, void *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (self->control->state)
            self->tegraStop();
        self->hardwareLost = true;
        return kIOReturnSuccess;
    }

    static IOReturn codecShutdownGated(OSObject *owner, void *, void *, void *, void *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (self->codecOff)
            return kIOReturnSuccess;
        self->codecQuiesce();
        self->codec.powerOff();
        self->codecOff = true;
        return kIOReturnSuccess;
    }

    static IOReturn powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t)
    {
        if (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart)
            static_cast<SwitchAudio *>(target)->systemShutdown();
        return kIOReturnSuccess;
    }

    // As at sleep, then the codec reset and its LDO1_EN low: the datasheet's power-off order, ahead of its rails.
    void systemShutdown()
    {
        codecStep(kCodecMute);
        gate->runAction(&SwitchAudio::shutdownGated);
        codecGate->runAction(&SwitchAudio::codecShutdownGated);
    }

public:
    // System sleep (switch_audio_suspend/resume, SwitchSuspend's thread), in ASoC's suspend order: the outputs muted
    // while the I2S still clocks, then the hardware stopped, then the codec powered down. On wake the hardware first,
    // then the codec, unmuted only if a stream runs. Null backend: nothing to do.
    void systemSleep(bool entering)
    {
        if (!tegra)
            return;
        if (entering) {
            codecStep(kCodecMute);
            gate->runAction(&SwitchAudio::sleepGated, reinterpret_cast<void *>(1));
            codecGate->runAction(&SwitchAudio::codecSleepGated, reinterpret_cast<void *>(1));
        } else {
            gate->runAction(&SwitchAudio::sleepGated, nullptr);
            codecGate->runAction(&SwitchAudio::codecSleepGated, nullptr);
        }
    }

    bool start(IOService *provider) override
    {
        if (!IOService::start(provider))
            return false;
        uint32_t off = 0;
        tegra = !(PE_parse_boot_argn("switch-no-audio", &off, sizeof(off)) && off);
        tegraCapture = tegra;
        const IOOptionBits options = (IOOptionBits)kIODirectionInOut | (IOOptionBits)kIOMemoryKernelUserShared |
                                     (IOOptionBits)kIOMemoryPhysicallyContiguous;
        const uint32_t sizes[3] = {(uint32_t)PAGE_SIZE, kOutBytes, kInBytes};
        for (int i = 0; i < 3; ++i) {
            memory[i] = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, options, sizes[i], kDmaMask);
            if (!memory[i])
                return false;
            bzero(memory[i]->getBytesNoCopy(), sizes[i]);
        }
        loop = IOWorkLoop::workLoop();
        gate = loop ? IOCommandGate::commandGate(this) : nullptr;
        if (!gate || loop->addEventSource(gate) != kIOReturnSuccess)
            return false;

        control = static_cast<SwitchAudioControl *>(memory[SwitchAudioMemoryControl]->getBytesNoCopy());
        control->magic = SWITCH_AUDIO_MAGIC;
        control->version = SWITCH_AUDIO_VERSION;
        control->rate = clock->rate;
        control->out_channels = SWITCH_AUDIO_OUT_CHANNELS;
        control->in_channels = SWITCH_AUDIO_IN_CHANNELS;
        control->ring_frames = SWITCH_AUDIO_RING_FRAMES;
        // The plug-in writes at least this far ahead of now: past one null tick, or past the ADMA's prefetch.
        control->lead_frames = 2 * SWITCH_AUDIO_PERIOD_FRAMES;
        hz = timerHz();
        if (tegra) {
            // The interrupt source goes on the loop before the deadline timer, so a pending interrupt is served first.
            tegraInit();
            codecStart();
            control->period_frames = kInPeriodFrames;
            control->backend = SwitchAudioBackendTegraI2S;
            for (uint32_t i = 0; i < SWITCH_AUDIO_MAX_RATES; ++i)
                control->rates[i] = kClocks[i].rate;
            noteClock();
            timer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::watchdogFired);
        } else {
            control->period_frames = SWITCH_AUDIO_PERIOD_FRAMES;
            control->backend = SwitchAudioBackendNull;
            control->rates[0] = SWITCH_AUDIO_RATE;
            control->ticks_per_frame_q32 = (hz << 32) / SWITCH_AUDIO_RATE;
            timer = IOTimerEventSource::timerEventSource(this, &SwitchAudio::tick);
        }
        if (!timer || loop->addEventSource(timer) != kIOReturnSuccess)
            return false;
        if (tegra) {
            // registerPrioritySleepWakeInterest dereferences the root domain unchecked.
            shutdownNotifier =
                getPMRootDomain() ? registerPrioritySleepWakeInterest(&SwitchAudio::powerMessage, this) : nullptr;
            if (!shutdownNotifier)
                IOLog("SwitchAudio: no shut down notification; the codec loses power with the PMIC\n");
        }
        control->anchor_host = mach_absolute_time();

        // audiomxd's sandbox names no class of ours; the kernel's NativeSandboxGrantProcess check (IOUserClient.cpp)
        // lets exactly this process open the service. Our check compares the whole string to one process name.
        setProperty("NativeSandboxGrantProcess", "audiomxd");
        setProperty("IOUserClientClass", "SwitchAudioUserClient");
        setProperty("SwitchAudioBackend", tegra ? "tegra-i2s" : "null");
        registerService();
        if (tegra) {
            // The format and rate, once: S32_LE rings and APE path, 32-bit I2S slots at 64 fs, the codec's 24 bits.
            const uint64_t milliHz = switch_audio_clock_millihz(38400000, *clock);
            IOLog("SwitchAudio: Tegra I2S1 + RT5639 ready (%s%s): S32_LE, I2S 32-bit slots at 64 fs (%u kHz BCLK), "
                  "RT5639 %u-bit, %llu.%03llu Hz\n",
                  tegraCapture ? "playback and capture" : "playback", speakerOnly ? ", speaker only" : "",
                  (unsigned)((milliHz * SWITCH_AUDIO_BCLK_FS + 500000) / 1000000), SWITCH_AUDIO_CODEC_BITS,
                  milliHz / 1000, milliHz % 1000);
        }
        return true;
    }

    void free() override
    {
        if (shutdownNotifier)
            shutdownNotifier->remove();
        if (timer) {
            timer->cancelTimeout();
            if (loop)
                loop->removeEventSource(timer);
        }
        if (cascade && loop) {
            cascade->disable();
            loop->removeEventSource(cascade);
        }
        if (gate && loop)
            loop->removeEventSource(gate);
        if (codecLoop) {
            if (jackSource) {
                jackSource->disable();
                codecLoop->removeEventSource(jackSource);
            }
#if DEVELOPMENT || DEBUG
            if (statsTimer)
                statsTimer->cancelTimeout();
            IOEventSource *sources[] = {jackTimer, idleTimer, statsTimer, codecGate};
#else
            IOEventSource *sources[] = {jackTimer, idleTimer, codecGate};
#endif
            for (IOEventSource *source : sources) {
                if (source)
                    codecLoop->removeEventSource(source);
            }
        }
        OSSafeReleaseNULL(timer);
#if DEVELOPMENT || DEBUG
        OSSafeReleaseNULL(statsTimer);
#endif
        OSSafeReleaseNULL(cascade);
        OSSafeReleaseNULL(gate);
        OSSafeReleaseNULL(loop);
        OSSafeReleaseNULL(jackSource);
        OSSafeReleaseNULL(jackTimer);
        OSSafeReleaseNULL(idleTimer);
        OSSafeReleaseNULL(codecGate);
        OSSafeReleaseNULL(codecLoop);
        for (auto &m : memory)
            OSSafeReleaseNULL(m);
        IOService::free();
    }

    // As ASoC orders a stream: the codec powered with its outputs muted, then the I2S clocks and the DMA, then the
    // unmute, so the RT5639 (the I2S slave) has a running frame clock before anything reaches its amplifiers. The
    // last stop mutes before the clocks stop.
    IOReturn request(uint32_t selector)
    {
        if (selector > SwitchAudioStopInput)
            return kIOReturnBadArgument;
        if (!tegra)
            return gate->runAction(&SwitchAudio::requestGated, (void *)(uintptr_t)selector);
        const bool run = selector == SwitchAudioStartOutput || selector == SwitchAudioStartInput;
        const uint32_t bit = (selector == SwitchAudioStartOutput || selector == SwitchAudioStopOutput)
                                 ? SwitchAudioOutputRunning
                                 : SwitchAudioInputRunning;
        if (run)
            codecStep(kCodecPowerUp);
        if (selector == SwitchAudioStartInput)
            codecStep(kCodecCaptureOn);
        else if (selector == SwitchAudioStopInput)
            codecStep(kCodecCaptureOff);
        if (!run && !(control->state & ~bit))
            codecStep(kCodecPause);
        const IOReturn result = gate->runAction(&SwitchAudio::requestGated, (void *)(uintptr_t)selector);
        if (run)
            codecStep(kCodecUnmute);
        return result;
    }

    // ticks_per_frame_q32 (and the stats' period length) for `clock`.
    void noteClock()
    {
        control->rate = clock->rate;
        control->ticks_per_frame_q32 = tegraTicksPerFrame(38400000);
#if DEVELOPMENT || DEBUG
        periodTicksQ16 = (uint64_t)kOutPeriodFrames * control->ticks_per_frame_q32 >> 16;
#endif
    }

    static IOReturn rateGated(OSObject *owner, void *next, void *, void *, void *)
    {
        auto *self = static_cast<SwitchAudio *>(owner);
        if (self->control->state) {
            switch_refusal("audio", "rate-while-running", "%u Hz asked while a stream runs at %u Hz",
                           static_cast<const switch_audio_clock *>(next)->rate, self->clock->rate);
            return kIOReturnBusy;
        }
        self->clock = static_cast<const switch_audio_clock *>(next);
        if (!self->hardwareLost)
            self->startClocks();
        self->noteClock();
        return kIOReturnSuccess;
    }

    // Both directions share the I2S clock, so the rate changes only between streams, and the codec, which runs from
    // MCLK, is powered down first rather than left muted on a clock that stops and comes back at another rate.
    IOReturn setRate(uint32_t rate)
    {
        const switch_audio_clock *next = nullptr;
        for (const switch_audio_clock &c : kClocks) {
            if (c.rate == rate && (tegra || c.rate == SWITCH_AUDIO_RATE))
                next = &c;
        }
        if (!next) {
            switch_refusal_k(switch_refusal_key_with(switch_refusal_key("audio", "rate"), rate), "audio", "rate",
                             "%u Hz asked; the %s backend runs %u%s", rate, tegra ? "Tegra" : "null", SWITCH_AUDIO_RATE,
                             tegra ? " and 44100 Hz" : " Hz");
            return kIOReturnUnsupported;
        }
        if (next == clock)
            return kIOReturnSuccess;
        codecStep(kCodecIdleNow);
        return gate->runAction(&SwitchAudio::rateGated, const_cast<switch_audio_clock *>(next));
    }

    // The headphone volume is the codec's: 0.375 dB DAC steps, rounded to the nearest.
    IOReturn setHeadphoneLevel(uint64_t attenuationMdB)
    {
        if (!tegra)
            return kIOReturnSuccess;
        const uint64_t steps =
            attenuationMdB > SWITCH_AUDIO_HEADPHONE_RANGE_MDB ? 0xffff : (attenuationMdB + 187) / 375;
        __atomic_store_n(&headphoneSteps, steps > 0xffff ? 0xffffu : (unsigned)steps, __ATOMIC_RELAXED);
        codecStep(kCodecHeadphoneLevel);
        return kIOReturnSuccess;
    }

    IOMemoryDescriptor *copyMemory(uint32_t type)
    {
        if (type > SwitchAudioMemoryInput)
            return nullptr;
        memory[type]->retain();
        return memory[type];
    }
};

OSDefineMetaClassAndStructors(SwitchAudio, IOService)

    class SwitchAudioUserClient : public IOUserClient {
    OSDeclareDefaultStructors(SwitchAudioUserClient) task_t owner = nullptr;
    SwitchAudio *audio = nullptr;

public:
    bool initWithTask(task_t task, void *security, UInt32 type, OSDictionary *properties) override
    {
        if (!IOUserClient::initWithTask(task, security, type, properties))
            return false;
        owner = task;
        return true;
    }

    bool start(IOService *provider) override
    {
        audio = OSDynamicCast(SwitchAudio, provider);
        return audio && IOUserClient::start(provider);
    }

    IOReturn clientClose() override
    {
        audio->request(SwitchAudioStopOutput);
        audio->request(SwitchAudioStopInput);
        terminate();
        return kIOReturnSuccess;
    }

    IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override
    {
        IOMemoryDescriptor *m = audio->copyMemory(type);
        if (!m)
            return kIOReturnBadArgument;
        *options = 0;
        *memory = m;  // the caller consumes the reference
        return kIOReturnSuccess;
    }

    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
                            OSObject *target, void *reference) override
    {
        if (args->structureInputSize || args->structureOutputSize || args->scalarOutputCount)
            return kIOReturnBadArgument;
        if (selector == SwitchAudioSetHostClock)
            return args->scalarInputCount == 1 ? setHostClock(owner, args->scalarInput[0]) : kIOReturnBadArgument;
        if (selector == SwitchAudioSetHeadphoneLevel)
            return args->scalarInputCount == 1 ? audio->setHeadphoneLevel(args->scalarInput[0]) : kIOReturnBadArgument;
        if (selector == SwitchAudioSetRate) {
            return args->scalarInputCount == 1 && args->scalarInput[0] <= UINT32_MAX
                       ? audio->setRate((uint32_t)args->scalarInput[0])
                       : kIOReturnBadArgument;
        }
        if (selector > SwitchAudioStopInput || args->scalarInputCount)
            return kIOReturnBadArgument;
        return audio->request(selector);
    }
};

OSDefineMetaClassAndStructors(SwitchAudioUserClient, IOUserClient)

    // Publishes the service under IOResources. Call once from the board thread (the Tegra backend sleeps on the GIC, the
    // GPIO controller and I2C1), any point before launchd starts audiomxd; returns 0 on success.
    static SwitchAudio *gAudio;  // set once published (switch_audio_suspend/resume)

extern "C" int switch_audio_publish(void)
{
    static bool published;
    if (published)
        return 0;
    IOService *resources = IOService::getResourceService();
    SwitchAudio *audio = OSTypeAlloc(SwitchAudio);
    if (!resources || !audio || !audio->init() || !audio->attach(resources)) {
        OSSafeReleaseNULL(audio);
        IOLog("SwitchAudio: publish failed\n");
        return -1;
    }
    if (!audio->start(resources)) {
        audio->detach(resources);
        audio->release();
        IOLog("SwitchAudio: start failed\n");
        return -1;
    }
    gAudio = audio;
    audio->release();  // the attachment keeps it
    published = true;
    IOLog("SwitchAudio: started\n");
    return 0;
}

extern "C" void switch_audio_suspend(void)
{
    if (SwitchAudio *audio = __atomic_load_n(&gAudio, __ATOMIC_ACQUIRE))
        audio->systemSleep(true);
}

extern "C" void switch_audio_resume(void)
{
    if (SwitchAudio *audio = __atomic_load_n(&gAudio, __ATOMIC_ACQUIRE))
        audio->systemSleep(false);
}
