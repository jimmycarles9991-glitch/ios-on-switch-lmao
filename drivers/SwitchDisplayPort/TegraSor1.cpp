// SPDX-License-Identifier: GPL-2.0-only
// SOR1; see TegraSor1.h. Register sequences: Linux drivers/gpu/drm/tegra/sor.c and sor.h (Copyright (C) 2013 NVIDIA
// Corporation): tegra_sor_dp_enable/_disable, _power_up_lanes/_power_down_lanes, _dp_precharge, _dp_term_calibrate,
// _dp_link_apply_training, _dp_link_configure, _apply_config, _mode_set, _attach/_detach/_wakeup/_power_up/_power_down;
// tegra210_sor1 (crossbar 2 1 0 3 4, identity lane map). Drive tables: the OLED's own (switchroot tegra210b01-fric.dts
// and tegra210-nx-common-disp.dtsi sor1 dp-display lt-data). Clocks: clk-tegra210.c (CLK_SOURCE_SOR1:
// 31:29 sor1 source, 5 = pll_d2_out0; 15:14 sor1_out, 0 sor_safe, 1 the pad clock), PLL_D2 as pll_d2_params. Supplies:
// tegra210b01-fric-power-tree.dtsi (vdd-dp-pad = MAX77620 LDO8, 1.0 V, no FPS slot: software owns it), the pad as
// upstream's DP SOR (TEGRA_IO_PAD_LVDS, IO_DPD2 bit 25).
#include "TegraSor1.h"

#include <IOKit/IOLib.h>

#include "../TegraI2C/TegraI2C.h"
#include "../../kits/SwitchCAR/SwitchCAR.h"
#include "../../kits/SwitchCAR/SwitchPMC.h"
#include "../../kits/SwitchMMIO/SwitchMMIOKernel.h"

namespace {
constexpr uint64_t kSor1PA = 0x54580000;
constexpr unsigned kClockSor1 = 183, kPartitionSor = 17, kPadLvds = 32 + 25, kPmicBus = 5;
constexpr uint8_t kPmic = 0x3c, kLdo8Config = 0x33, kLdo8On1V0 = (3u << 6) | 4u, kLdo8Off = 4u;
constexpr unsigned kHead = 1;  // display controller B
constexpr uint32_t kHandshakeUs = 100000;

enum : uint32_t {
    CLK_SOURCE_DISP2 = 0x13c,
    CLK_SOURCE_SOR1 = 0x410,
    OSC_CTRL = 0x050,
    PLLD2_BASE = 0x4b8,
    PLLD2_MISC0 = 0x4bc,
    PLLD2_MISC1 = 0x570,
    PLLD2_MISC2 = 0x574,
    PLLD2_MISC3 = 0x578,
    PLLD2_MISC4 = 0x76c,
};

constexpr uint32_t kPllEnable = 1u << 30, kPllLock = 1u << 27, kPllIddq = 1u << 18, kSdmEnable = 1u << 31;
constexpr uint32_t kSourcePllD2Out0 = 5u << 29, kSourceMask = 7u << 29, kOutMask = 3u << 14, kOutPad = 1u << 14;

enum : uint32_t {
    SUPER_STATE0 = 0x01,
    SUPER_STATE1 = 0x02,
    STATE0 = 0x03,
    STATE1 = 0x04,
    HEAD_STATE0 = 0x05,
    HEAD_STATE1 = 0x07,
    HEAD_STATE2 = 0x09,
    HEAD_STATE3 = 0x0b,
    HEAD_STATE4 = 0x0d,
    HEAD_STATE5 = 0x0f,
    CLK_CNTRL = 0x13,
    PWR = 0x15,
    TEST = 0x16,
    PLL0 = 0x17,
    PLL1 = 0x18,
    PLL2 = 0x19,
    PLL3 = 0x1a,
    LVDS = 0x1c,
    LANE_SEQ_CTL = 0x21,
    SEQ_INST0 = 0x22,
    SEQ_INST8 = 0x2a,
    XBAR_CTRL = 0x4a,
    XBAR_POL = 0x4b,
    DP_LINKCTL0 = 0x4c,
    LANE_DRIVE_CURRENT0 = 0x4e,
    LANE_PREEMPHASIS0 = 0x52,
    LANE_POSTCURSOR0 = 0x56,
    DP_CONFIG0 = 0x58,
    DP_PADCTL0 = 0x5c,
    DP_SPARE0 = 0x60,
    DP_AUDIO_CTRL = 0x62,
    DP_AUDIO_HBLANK_SYMBOLS = 0x63,
    DP_AUDIO_VBLANK_SYMBOLS = 0x64,
    DP_TPG = 0x6d,
    PLL4 = 0x72,
    PADCTL2 = 0x73,
    AUDIO_CNTRL0 = 0xfc,
    AUDIO_NVAL_0441 = 0x100,
    AUDIO_NVAL_0480 = 0x103,
    AUDIO_HDA_ELD_BUFWR = 0x10c,
    AUDIO_HDA_PRESENSE = 0x10d,
    DP_AUDIO_TIMESTAMP_0441 = 0x134,
    DP_AUDIO_TIMESTAMP_0480 = 0x137,
};

constexpr uint32_t kAttached = 1u << 3, kModeNormal = 1u << 2, kHeadAwake = 2u;
constexpr uint32_t kTrigger = 1u << 31, kNormalPowerUp = 1u << 0, kModeSafe = 1u << 28;
constexpr uint32_t kTestAttached = 1u << 10, kTestHeadMask = 3u << 8, kTestHeadAwake = 2u << 8;
constexpr uint32_t kPll0VcoPd = 1u << 2, kPll0Pwr = 1u << 0, kPll0ResistorExt = 1u << 4;
constexpr uint32_t kPll1TermCompout = 1u << 15, kPll1TermAdjMask = 0xfu << 9, kPll1Term = 1u << 8;
constexpr uint32_t kPll1LoadAdjMask = 0xfu << 20;
constexpr uint32_t kPll2Enforce = 1u << 24, kPll2PortPd = 1u << 23, kPll2BandgapPd = 1u << 22;
constexpr uint32_t kPll2Override = 1u << 18, kPll2CapPd = 1u << 17;
constexpr uint32_t kPll3Vdd3V3 = 1u << 13;
constexpr uint32_t kClkSelMask = 3u, kClkSelDiffDp = 3u, kLinkSpeedMask = 0x1fu << 2;
constexpr uint32_t kSpareSeqEnable = 1u << 0, kSparePanelInternal = 1u << 1, kSpareMacroSorClk = 1u << 2;
constexpr uint32_t kPadCalPd = 1u << 23, kTxPuEnable = 1u << 22, kTxPuMask = 0xffu << 8;
constexpr uint32_t kLaneSeqTrigger = 1u << 31, kLaneSeqDown = 1u << 20, kLaneStateDown = 1u << 16;
constexpr uint32_t kLaneSeqBusy = 1u << 28;
constexpr uint32_t kSeqInstWaitVsync = 2u << 12, kSeqInstHalt = 1u << 15, kSeqInstDrivePwmLo = 1u << 23;
constexpr uint32_t kLinkEnable = 1u << 0, kEnhancedFrame = 1u << 14, kLaneCountMask = 0x1fu << 16;
constexpr uint32_t kTuSizeMask = 0x7fu << 2;
constexpr uint32_t kProtocolMask = 0xfu << 8, kProtocolDpA = 0x8u << 8, kOwnerMask = 0xfu, kSubownerMask = 3u << 4;
constexpr uint32_t kPixelDepthMask = 0xfu << 17, kBpp24 = 0x5u << 17, kCrcComplete = 1u << 6;
constexpr uint32_t kHsyncLow = 1u << 12, kVsyncLow = 1u << 13;
constexpr uint8_t kCrossbar[5] = {2, 1, 0, 3, 4};

constexpr uint8_t kSwing[4][10] = {
    {0x13, 0x19, 0x1e, 0x28, 0x1e, 0x25, 0x2d, 0x28, 0x32, 0x39},
    {0x12, 0x17, 0x1b, 0x25, 0x1c, 0x23, 0x2a, 0x25, 0x2f, 0x37},
    {0x12, 0x16, 0x1a, 0x22, 0x1b, 0x20, 0x27, 0x24, 0x2d, 0x35},
    {0x11, 0x14, 0x17, 0x1f, 0x19, 0x1e, 0x24, 0x22, 0x2a, 0x32},
};
constexpr uint8_t kEmphasis[4][10] = {
    {0x00, 0x08, 0x12, 0x24, 0x01, 0x0e, 0x1d, 0x01, 0x13, 0x00},
    {0x00, 0x08, 0x12, 0x24, 0x00, 0x0e, 0x1d, 0x00, 0x13, 0x00},
    {0x00, 0x08, 0x12, 0x24, 0x00, 0x0e, 0x1d, 0x00, 0x13, 0x00},
    {0x00, 0x08, 0x12, 0x24, 0x00, 0x0e, 0x1d, 0x00, 0x13, 0x00},
};
constexpr uint8_t kPostcursor[4][10] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x02, 0x02, 0x04, 0x05, 0x02, 0x04, 0x05, 0x04, 0x05, 0x05},
    {0x04, 0x05, 0x08, 0x0b, 0x05, 0x09, 0x0b, 0x08, 0x0a, 0x0b},
    {0x05, 0x09, 0x0b, 0x12, 0x09, 0x0d, 0x12, 0x0b, 0x0f, 0x12},
};
constexpr uint8_t kTxPu[4][10] = {
    {0x20, 0x30, 0x40, 0x60, 0x30, 0x40, 0x60, 0x40, 0x60, 0x60},
    {0x20, 0x20, 0x30, 0x50, 0x30, 0x40, 0x50, 0x40, 0x50, 0x60},
    {0x20, 0x20, 0x30, 0x40, 0x30, 0x30, 0x40, 0x40, 0x50, 0x60},
    {0x20, 0x20, 0x20, 0x40, 0x30, 0x30, 0x40, 0x40, 0x40, 0x60},
};

unsigned driveIndex(unsigned swing, unsigned emphasis)
{
    static const unsigned start[4] = {0, 4, 7, 9};
    return start[swing & 3u] + emphasis;
}
}  // namespace

uint32_t TegraSor1::read(uint32_t word) const
{
    return switch_mmio_read32(base, word * 4u);
}

void TegraSor1::write(uint32_t word, uint32_t value)
{
    switch_mmio_write32(base, word * 4u, value);
}

void TegraSor1::rmw(uint32_t word, uint32_t clear, uint32_t set)
{
    switch_mmio_rmw32(base, word * 4u, clear, set);
}

bool TegraSor1::waitClear(uint32_t word, uint32_t bits, uint32_t us)
{
    return switch_mmio_wait32(base, word * 4u, bits, 0, us);
}

void TegraSor1::superUpdate()
{
    write(SUPER_STATE0, 0);
    write(SUPER_STATE0, 1);
    write(SUPER_STATE0, 0);
}

void TegraSor1::update()
{
    write(STATE0, 0);
    write(STATE0, 1);
    write(STATE0, 0);
}

uint32_t TegraSor1::referenceClockKhz()
{
    return 38400u >> ((switch_car_read(OSC_CTRL) >> 26) & 3u);
}

bool TegraSor1::pixelClock(uint32_t pixel_khz, char *why, size_t size)
{
    const bool b01 = switch_car_soc_t210b01();
    const uint32_t ref_khz = referenceClockKhz();
    struct switch_dp_plld2 pll = {};
    if (!switch_dp_plld2_for(pixel_khz, ref_khz, b01, &pll)) {
        snprintf(why, size, "PLL_D2 cannot make %u kHz", pixel_khz);
        return false;
    }
    const uint32_t dividers = (pll.pdiv_hw << 19) | (pll.n << 8) | pll.m;
    switch_car_rmw(PLLD2_BASE, kPllEnable, 0);
    IODelay(1);
    switch_car_write(PLLD2_BASE, dividers | kPllIddq);
    IODelay(2);
    switch_car_write(PLLD2_MISC0, b01 ? 0x40000000 : 0x40000020);
    switch_car_write(PLLD2_MISC2, b01 ? 0xf6e0f620 : 0);
    switch_car_write(PLLD2_MISC3, (b01 ? 0x00010000u : 0) | pll.sdm_din);
    if (b01)
        switch_car_write(PLLD2_MISC4, 0);
    switch_car_write(PLLD2_MISC1, 0x10000000 | kSdmEnable);
    switch_car_write(PLLD2_BASE, dividers);
    IODelay(5);
    switch_car_write(PLLD2_BASE, dividers | kPllEnable);
    if (!switch_car_wait(PLLD2_BASE, kPllLock, kPllLock, 300)) {
        snprintf(why, size, "PLL_D2 did not lock (base %#x)", switch_car_read(PLLD2_BASE));
        return false;
    }
    switch_car_write(CLK_SOURCE_DISP2, kSourcePllD2Out0);
    switch_car_rmw(CLK_SOURCE_SOR1, kSourceMask | 0xffu, kSourcePllD2Out0);
    return true;
}

bool TegraSor1::powerUp(char *why, size_t size)
{
    if (base == nullptr) {
        base = switch_mmio_map(kSor1PA, 0x1000);
    }
    const uint8_t on = kLdo8On1V0;
    if (tegra_i2c_write(kPmicBus, kPmic, kLdo8Config, &on, 1) != 0) {
        snprintf(why, size, "MAX77620 LDO8 (vdd-hdmi-dp) did not take 1.0 V");
        return false;
    }
    IODelay(100);
    if (!switch_pmc_partition_is_on(kPartitionSor) &&
        !switch_pmc_power_up(kPartitionSor, kClockSor1, kClockSor1, nullptr)) {
        snprintf(why, size, "the SOR power partition did not come up");
        return false;
    }
    switch_car_reset_assert(kClockSor1);
    switch_car_rmw(CLK_SOURCE_SOR1, kOutMask, 0);
    switch_car_clock_enable(kClockSor1);
    IOSleep(1);
    switch_car_reset_deassert(kClockSor1);
    return true;
}

bool TegraSor1::lanesPower(uint8_t count, bool up)
{
    if (!waitClear(LANE_SEQ_CTL, kLaneSeqBusy, kHandshakeUs))
        return false;
    uint32_t value = read(DP_PADCTL0) & ~0xfu;
    if (up) {
        for (unsigned lane = 0; lane < count; ++lane) {
            value |= 1u << lane;
        }
    }
    write(DP_PADCTL0, value);
    write(LANE_SEQ_CTL, kLaneSeqTrigger | kLaneSeqDown | (15u << 12) | (up ? 0 : kLaneStateDown));
    return waitClear(LANE_SEQ_CTL, kLaneSeqTrigger, kHandshakeUs);
}

bool TegraSor1::terminationCalibrate(char *why, size_t size)
{
    if (!switch_pmc_io_pad_on(kPadLvds)) {
        snprintf(why, size, "the DP pads did not leave deep power down for calibration");
        return false;
    }
    IODelay(5);
    rmw(PLL2, kPll2BandgapPd, 0);
    rmw(PLL1, 0, kPll1Term);
    IODelay(20);
    rmw(DP_PADCTL0, kPadCalPd, 0);
    IODelay(10);
    uint32_t adjust = 0;
    for (uint32_t mask = 0x8; mask != 0; mask >>= 1) {
        adjust |= mask;
        rmw(PLL1, kPll1TermAdjMask, adjust << 9);
        IODelay(100);
        if ((read(PLL1) & kPll1TermCompout) != 0) {
            adjust &= ~mask;
        }
    }
    rmw(PLL1, kPll1TermAdjMask, adjust << 9);
    rmw(DP_PADCTL0, 0, kPadCalPd);
    IODelay(10);
    rmw(PLL2, 0, kPll2BandgapPd);
    rmw(PLL1, kPll1Term, 0);
    IODelay(20);
    if (!switch_pmc_io_pad_off(kPadLvds)) {
        snprintf(why, size, "the DP pads did not enter deep power down after calibration");
        return false;
    }
    IODelay(5);
    return true;
}

bool TegraSor1::startLink(const struct switch_dp_link &link, bool enhanced, char *why, size_t size)
{
    lanes = link.lanes;
    switch_car_clock_disable(kClockSor1);
    switch_car_rmw(CLK_SOURCE_SOR1, kOutMask, 0);
    switch_car_clock_enable(kClockSor1);
    rmw(CLK_CNTRL, kClkSelMask | kLinkSpeedMask, kClkSelDiffDp | (uint32_t)(link.rate & 0x1fu) << 2);
    IODelay(300);
    if (switch_car_soc_t210b01()) {
        rmw(PLL0, 0x000f0000, 0x00050000);
        rmw(PLL1, kPll1LoadAdjMask | kPll1Term, (4u << 20) | kPll1Term);
        rmw(PLL4, 0x00400000, 0);
        rmw(PADCTL2, 0xff000000, 0x34000000);
        rmw(DP_PADCTL0, kTxPuMask, link.rate == SWITCH_DP_RATE_HBR2 ? 0x2000u : 0x3000u);
    } else {
        const uint32_t loadAdjust = link.rate == SWITCH_DP_RATE_RBR ? 3u : link.rate == SWITCH_DP_RATE_HBR ? 4u : 6u;
        rmw(PLL0, (0xfu << 24) | (0xfu << 8) | kPll0ResistorExt, (1u << 24) | (3u << 8) | kPll0ResistorExt);
        rmw(PLL1, kPll1LoadAdjMask, loadAdjust << 20);
        rmw(PLL3, 0, kPll3Vdd3V3);
    }
    rmw(DP_SPARE0, kSparePanelInternal | kSpareMacroSorClk, kSpareSeqEnable);
    write(LVDS, 0);
    if (!terminationCalibrate(why, size))
        return false;
    rmw(PLL2, kPll2Override, kPll2CapPd | kPll2Enforce | kPll2PortPd | kPll2BandgapPd);
    rmw(PLL0, 0, kPll0VcoPd | kPll0Pwr);
    if (!switch_pmc_io_pad_on(kPadLvds)) {
        snprintf(why, size, "the DP pads did not leave deep power down");
        return false;
    }
    IODelay(5);
    rmw(PLL2, kPll2BandgapPd, 0);
    IODelay(100);
    rmw(PLL0, kPll0VcoPd | kPll0Pwr, 0);
    rmw(PLL2, kPll2Enforce, 0);
    IODelay(200);
    rmw(PLL2, kPll2PortPd, 0);
    rmw(PLL1, 0, kPll1Term);
    IODelay(10);
    rmw(STATE1, kProtocolMask, kProtocolDpA);
    rmw(DP_LINKCTL0, kLinkEnable | kLaneCountMask | kEnhancedFrame,
        (((1u << link.lanes) - 1u) << 16) | (enhanced ? kEnhancedFrame : 0));
    if (!lanesPower(link.lanes, true)) {
        snprintf(why, size, "SOR1's lane sequencer did not finish (LANE_SEQ_CTL %#x)", read(LANE_SEQ_CTL));
        return false;
    }
    return true;
}

void TegraSor1::enableLink(const struct switch_dp_tu &tu)
{
    rmw(DP_LINKCTL0, kTuSizeMask, (tu.tu_size & 0x7fu) << 2);
    rmw(DP_CONFIG0, (1u << 24) | (0xfu << 16) | (0x7fu << 8) | 0x3fu,
        (1u << 31) | (1u << 28) | (1u << 26) | (tu.active_polarity != 0 ? 1u << 24 : 0) |
            ((tu.active_frac & 0xfu) << 16) | ((tu.active_count & 0x7fu) << 8) | (tu.watermark & 0x3fu));
    rmw(DP_LINKCTL0, 0, kLinkEnable);
    uint32_t crossbar = 0, mask = 3u;
    for (uint32_t i = 0; i < 5; ++i) {
        crossbar |= (uint32_t)kCrossbar[i] << (i * 3 + 2);
        mask |= 7u << (i * 3 + 2);
    }
    rmw(XBAR_CTRL, mask, crossbar);
    write(XBAR_POL, 0);
    switch_car_clock_disable(kClockSor1);
    switch_car_rmw(CLK_SOURCE_SOR1, kOutMask, kOutPad);
    switch_car_clock_enable(kClockSor1);
    rmw(DP_SPARE0, kSparePanelInternal, kSpareMacroSorClk);
    const uint32_t lanesMask = (1u << lanes) - 1u;
    rmw(DP_PADCTL0, 0xfu << 4, lanesMask << 4);
    IODelay(20);
    rmw(DP_PADCTL0, 0xfu << 4, 0);
}

#if DEVELOPMENT || DEBUG
void TegraSor1::logTrainingState()
{
    IOLog("SwitchDisplayPort: SOR lane state %#x, pad %#x, link %#x, config %#x, clock %#x, power %#x\n",
          read(LANE_SEQ_CTL), read(DP_PADCTL0), read(DP_LINKCTL0), read(DP_CONFIG0), read(CLK_CNTRL), read(PWR));
    IOLog("SwitchDisplayPort: SOR drive %#x, emphasis %#x, pattern %#x, crossbar %#x/%#x, OSC %#x\n",
          read(LANE_DRIVE_CURRENT0), read(LANE_PREEMPHASIS0), read(DP_TPG), read(XBAR_CTRL), read(XBAR_POL),
          switch_car_read(OSC_CTRL));
}
#endif

void TegraSor1::drive(const struct switch_dp_train &train, const struct switch_dp_train_out &out)
{
    uint32_t swing = 0, emphasis = 0, postcursor = 0, txPu = 0, pattern = 0;
    for (unsigned lane = 0; lane < lanes; ++lane) {
        const unsigned index = driveIndex(train.swing[lane], train.emphasis[lane]);
        const unsigned pc = (train.postcursor >> (lane * 2)) & 3u;
        postcursor |= (uint32_t)kPostcursor[pc][index] << (lane * 8);
        swing |= (uint32_t)kSwing[pc][index] << (lane * 8);
        emphasis |= (uint32_t)kEmphasis[pc][index] << (lane * 8);
        txPu = kTxPu[pc][index] > txPu ? kTxPu[pc][index] : txPu;
        const uint32_t lanePattern = out.pattern == 0 ? (1u << 4) : out.pattern;  // none: Galois scrambler on
        pattern = (pattern << 8) | lanePattern | (1u << 6);                       // 8b/10b channel coding
    }
    if (train.tps3)
        write(LANE_POSTCURSOR0, postcursor);
    write(LANE_DRIVE_CURRENT0, swing);
    write(LANE_PREEMPHASIS0, emphasis);
    write(DP_TPG, pattern);
    rmw(DP_PADCTL0, kTxPuMask, kTxPuEnable | (txPu << 8));
    IODelay(20);
}

bool TegraSor1::attach(const struct switch_dp_mode &mode, const struct switch_dp_tu &tu, char *why, size_t size)
{
    rmw(DP_AUDIO_HBLANK_SYMBOLS, 0x1ffffu, tu.hblank_symbols & 0xffffu);
    rmw(DP_AUDIO_VBLANK_SYMBOLS, 0xffffu, tu.vblank_symbols & 0xffffu);

    rmw(STATE1, kPixelDepthMask | (3u << 6) | kOwnerMask | kHsyncLow | kVsyncLow,
        kBpp24 | kCrcComplete | (kHead + 1) | (mode.hsync_high ? 0 : kHsyncLow) | (mode.vsync_high ? 0 : kVsyncLow));
    const uint32_t vtotal = switch_dp_mode_vtotal(&mode) - 1u, htotal = switch_dp_mode_htotal(&mode) - 1u;
    const uint32_t vse = mode.vsync - 1u, hse = mode.hsync - 1u;
    const uint32_t vbe = vse + mode.vback, hbe = hse + mode.hback;
    write(HEAD_STATE0 + kHead, 0);
    write(HEAD_STATE1 + kHead, ((vtotal & 0x7fffu) << 16) | (htotal & 0x7fffu));
    write(HEAD_STATE2 + kHead, ((vse & 0x7fffu) << 16) | (hse & 0x7fffu));
    write(HEAD_STATE3 + kHead, ((vbe & 0x7fffu) << 16) | (hbe & 0x7fffu));
    write(HEAD_STATE4 + kHead, (((vbe + mode.vactive) & 0x7fffu) << 16) | ((hbe + mode.hactive) & 0x7fffu));
    write(HEAD_STATE5 + kHead, 1);
    update();

    // Reset SEQ_INST leaves TRISTATE_IOS=1 (TRM 27.8.24); clear it before triggering SOR_PWR.
    const uint32_t seqInst = 1u | kSeqInstWaitVsync | kSeqInstHalt | kSeqInstDrivePwmLo;
    write(SEQ_INST0, seqInst);
    write(SEQ_INST8, seqInst);
    rmw(PWR, 0, kTrigger | kNormalPowerUp);
    if (!waitClear(PWR, kTrigger, kHandshakeUs)) {
        snprintf(why, size, "SOR1 did not power up (PWR %#x)", read(PWR));
        return false;
    }
    headAttached = true;
    write(SUPER_STATE1, kAttached);
    superUpdate();
    if (!switch_mmio_wait32(base, TEST * 4u, kTestAttached, kTestAttached, kHandshakeUs)) {
        snprintf(why, size, "SOR1 did not attach (TEST %#x)", read(TEST));
        return false;
    }
    write(SUPER_STATE1, kAttached | kModeNormal);
    superUpdate();
    return true;
}

bool TegraSor1::wake(char *why, size_t size)
{
    write(SUPER_STATE1, kAttached | kModeNormal | kHeadAwake);
    superUpdate();
    if (!switch_mmio_wait32(base, TEST * 4u, kTestHeadMask, kTestHeadAwake, kHandshakeUs)) {
        snprintf(why, size, "SOR1's head did not wake (TEST %#x)", read(TEST));
        return false;
    }
    return true;
}

void TegraSor1::detachHead()
{
    if (!headAttached || base == nullptr || !switch_car_clock_enabled(kClockSor1)) {
        headAttached = false;
        return;
    }
    headAttached = false;
    rmw(SUPER_STATE1, kModeNormal, 0);
    superUpdate();
    (void)switch_mmio_wait32(base, PWR * 4u, kModeSafe, kModeSafe, kHandshakeUs);
    rmw(SUPER_STATE1, 3u, 0);
    superUpdate();
    (void)waitClear(TEST, kTestHeadMask, kHandshakeUs);
    rmw(SUPER_STATE1, kAttached, 0);
    superUpdate();
    (void)waitClear(TEST, kTestAttached, kHandshakeUs);
    write(STATE1, 0);
    update();
}

void TegraSor1::powerDown()
{
    headAttached = false;
    if (base == nullptr || !switch_car_clock_enabled(kClockSor1)) {
        return;
    }
    switch_car_clock_disable(kClockSor1);
    switch_car_rmw(CLK_SOURCE_SOR1, kOutMask, 0);
    switch_car_clock_enable(kClockSor1);
    rmw(DP_SPARE0, kSpareMacroSorClk, 0);
    (void)lanesPower(0, false);
    rmw(DP_LINKCTL0, kLinkEnable, 0);
    rmw(STATE1, kProtocolMask | kSubownerMask | kOwnerMask, 0);
    update();
    rmw(PWR, kNormalPowerUp, kTrigger);
    (void)waitClear(PWR, kTrigger, kHandshakeUs);
    rmw(PLL2, 0, kPll2PortPd);
    IODelay(20);
    rmw(PLL0, 0, kPll0VcoPd | kPll0Pwr);
    rmw(PLL2, 0, kPll2CapPd | kPll2Enforce);
    IODelay(20);
    (void)switch_pmc_io_pad_off(kPadLvds);
    switch_car_reset_assert(kClockSor1);
    switch_car_clock_disable(kClockSor1);
    switch_car_write(PLLD2_BASE, (switch_car_read(PLLD2_BASE) & ~kPllEnable) | kPllIddq);
    const uint8_t off = kLdo8Off;
    (void)tegra_i2c_write(kPmicBus, kPmic, kLdo8Config, &off, 1);
    lanes = 0;
}

bool TegraSor1::audioFormat(uint32_t rate, uint32_t channels, uint32_t bits)
{
    if (!base || !lanes || (rate != 48000 && rate != 44100) || channels != 2 ||
        (bits != 16 && bits != 24 && bits != 32))
        return false;
    // TRM 27.6.5 Table 162: MAUD is 102 MHz, N/(N + D_N) = 512 * fs / MAUD. DP's Naud is always 2^15; the NVAL
    // registers reset to HDMI's N. The SOR picks the pair for the rate the converter reports.
    write(DP_AUDIO_TIMESTAMP_0480, (512u << 16) | (2125u - 512u));
    write(AUDIO_NVAL_0480, 0x8000);
    write(DP_AUDIO_TIMESTAMP_0441, (2352u << 16) | (10625u - 2352u));
    write(AUDIO_NVAL_0441, 0x8000);
    rmw(AUDIO_CNTRL0, (3u << 20) | 1u, (2u << 20) | (1u << 12));
    return true;
}

void TegraSor1::audioEnable(bool enable)
{
    if (!base || !lanes)
        return;
#if DEVELOPMENT || DEBUG
    if (!enable)
        logAudioState("at stop");
#endif
    write(DP_AUDIO_CTRL, (1u << 31) | (0x1fu << 16) | (enable ? 1u : 0u));
#if DEVELOPMENT || DEBUG
    if (enable) {
        IODelay(1);
        logAudioState("at start");
    }
#endif
}

void TegraSor1::audioPresence(const uint8_t *eld, uint32_t bytes, bool present)
{
    if (!base || !lanes)
        return;
    write(AUDIO_HDA_PRESENSE, 0);
    if (present && eld && bytes && bytes <= 96) {
        for (uint32_t i = 0; i < 96; ++i)
            write(AUDIO_HDA_ELD_BUFWR, (i << 8) | (i < bytes ? eld[i] : 0));
        write(AUDIO_HDA_PRESENSE, 3);
    }
#if DEVELOPMENT || DEBUG
    IOLog("SwitchDisplayPort: SOR audio presence: ELD %u bytes, HDA_PRESENSE %#x\n", present ? bytes : 0,
          read(AUDIO_HDA_PRESENSE));
#endif
}

#if DEVELOPMENT || DEBUG
void TegraSor1::logAudioState(const char *when)
{
    IOLog("SwitchDisplayPort: SOR audio %s: DP_AUDIO_CTRL %#x, AUDIO_CNTRL0 %#x, N 44.1k %#x 48k %#x\n", when,
          read(DP_AUDIO_CTRL), read(AUDIO_CNTRL0), read(AUDIO_NVAL_0441), read(AUDIO_NVAL_0480));
}
#endif
