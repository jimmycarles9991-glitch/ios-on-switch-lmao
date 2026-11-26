// XusbUtmiPad.cpp against the register model: USB2 pad 0 powered as L4T's xusb-tegra210.c powers it in either role
// (tegra210_usb2_phy_power_on, then tegra210_utmi_bias_pad_power_on and tegra210_utmi_pad_power_on), every write and
// delay in order, per SoC revision (the prod values and board offsets L4T gives a T210 and a T210B01), and the PLLs left
// alone when they already run.
#include "drivers/SwitchUSB/XusbUtmiPad.h"
#include "xusb_register_model.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <tuple>

namespace {
constexpr uintptr_t kCar = 0x60006000, kFuse = 0x7000F800, kPadctl = 0x7009F000;
constexpr uintptr_t kPortCap = kPadctl + 0x008, kChrgCtl0 = kPadctl + 0x080, kChrgCtl1 = kPadctl + 0x084,
                    kOtgCtl0 = kPadctl + 0x088, kOtgCtl1 = kPadctl + 0x08c, kBiasCtl0 = kPadctl + 0x284,
                    kBiasCtl1 = kPadctl + 0x288;
// FUSE_USB_CALIB: HS_SQUELCH 31:29 = 5, HS_TERM_RANGE_ADJ 10:7 = 0xb, pad 0's HS_CURR_LEVEL 5:0 = 0x21;
// FUSE_USB_CALIB_EXT: RPD_CTRL 4:0 = 0x13.
constexpr uint32_t kCalib = 5u << 29 | 0xbu << 7 | 0x21u, kCalibExt = 0x13;
// APB_MISC_GP_HIDREV: chip 0x21, major revision 1 (T210) or 2 (T210B01).
constexpr uintptr_t kHidrev = 0x70000804;
constexpr uint32_t kT210 = 0x21u << 8 | 1u << 4, kT210B01 = 0x21u << 8 | 2u << 4;

void bootState()
{
    auto &r = xusbModel().registers;
    r[kFuse + 0x1f0] = kCalib;
    r[kFuse + 0x350] = kCalibExt;
    r[kOtgCtl0] = 1u << 26 | 1u << 29;  // USB2_OTG_PD, PD_ZI
    r[kOtgCtl1] = 1u << 2;              // USB2_OTG_PD_DR
    r[kChrgCtl0] = 1u << 0;             // PD_CHG
    r[kBiasCtl0] = 1u << 11;            // BIAS_PAD_PD
    r[kBiasCtl1] = 1u << 26;            // USB2_PD_TRK
}

class XusbUtmiPad : public ::testing::Test {
protected:
    void SetUp() override
    {
        xusbModelReset();
        bootState();
    }

    static void powerOn()
    {
        xpad::powerOn(reinterpret_cast<volatile uint8_t *>(kCar), reinterpret_cast<volatile uint8_t *>(0x7000F000),
                      reinterpret_cast<volatile uint8_t *>(kPadctl));
    }
};

TEST_F(XusbUtmiPad, PowersThePadAsL4T)
{
    xusbModel().registers[kHidrev] = kT210B01;
    powerOn();
    EXPECT_EQ(xusbModel().log, (std::vector<XusbEvent>{
                                   xusbWrite(kCar + 0x328, 1u << 7),   // CLK_ENB_H_SET: FUSE
                                   xusbWrite(kCar + 0x048, 1u << 28),  // MISC_CLK_ENB: CFG_ALL_VISIBLE
                                   xusbWrite(kChrgCtl1, 1u << 6),
                                   xusbWrite(kPadctl + 0x0c4, 1u << 6),
                                   xusbWrite(kPadctl + 0x104, 1u << 6),
                                   xusbWrite(kPadctl + 0x144, 1u << 6),
                                   xusbWrite(kChrgCtl1, 1u << 6),
                                   xusbWrite(kBiasCtl0, 1u << 11 | 0x3a),
                                   xusbWrite(kBiasCtl0, 1u << 11 | 0x300 | 0x3a),
                                   xusbWrite(kPortCap, 3),
                                   xusbWrite(kOtgCtl0, 1u << 26 | (0x21u + 6)),
                                   xusbWrite(kOtgCtl1, 1u << 2 | 0xbu << 3 | 0x13u << 26),
                                   xusbWrite(kChrgCtl1, 1u << 6 | 1u << 7),
                                   xusbWrite(kChrgCtl1, 1u << 6 | 1u << 7),
                                   xusbWrite(kChrgCtl0, 1),
                                   xusbWrite(kOtgCtl0, 1u << 26 | (0x21u + 6)),
                                   xusbWrite(kCar + 0x29c, 1u << 18),
                                   xusbWrite(kCar + 0x6cc, 6),
                                   xusbWrite(kBiasCtl1, 1u << 26 | 0x1eu << 12 | 0xau << 19),
                                   xusbWrite(kBiasCtl0, 0x300 | 7u << 3 | 5),
                                   xusbDelay(1),
                                   xusbWrite(kBiasCtl1, 0x1eu << 12 | 0xau << 19),
                                   xusbDelay(2),
                                   xusbWrite(kOtgCtl0, 0x21 + 6),
                                   xusbWrite(kOtgCtl1, 0xbu << 3 | 0x13u << 26),
                               }));
}

// VREG_FIX18 (pad protection off) is T210B01's alone, on all four USB2 pads.
TEST_F(XusbUtmiPad, PadProtectionOffOnT210B01Only)
{
    auto &r = xusbModel().registers;
    r[kHidrev] = kT210;
    powerOn();
    for (const uintptr_t ctl1 : {kChrgCtl1, kPadctl + 0x0c4, kPadctl + 0x104, kPadctl + 0x144})
        EXPECT_EQ(r[ctl1] & (1u << 6), 0u) << std::hex << ctl1;
    xusbModelReset();
    bootState();
    r[kHidrev] = kT210B01;
    powerOn();
    for (const uintptr_t ctl1 : {kChrgCtl1, kPadctl + 0x0c4, kPadctl + 0x104, kPadctl + 0x144})
        EXPECT_EQ(r[ctl1] & (1u << 6), 1u << 6) << std::hex << ctl1;
}

// tegra210-prods.dtsi has prod_c_bias's asic tuple only: bits 10:8 stay as they were.
TEST_F(XusbUtmiPad, T210HasNoCharBiasTuple)
{
    xusbModel().registers[kHidrev] = kT210;
    powerOn();
    EXPECT_EQ(xusbModel().registers[kBiasCtl0], 7u << 3 | 5);
}

// HS_CURR_LEVEL is the fuse's plus the T210B01 board's offset of 6, wrapped to the 6-bit field as Linux's
// HS_CURR_LEVEL() masks it; a T210 board has no offset.
TEST_F(XusbUtmiPad, HsCurrentLevelTakesTheBoardOffset)
{
    auto &r = xusbModel().registers;
    for (const auto &[hidrev, fuse, level] :
         {std::tuple{kT210B01, 0x21u, 0x27u}, std::tuple{kT210B01, 0x3cu, 0x02u}, std::tuple{kT210, 0x21u, 0x21u}}) {
        xusbModelReset();
        bootState();
        r[kHidrev] = hidrev;
        r[kFuse + 0x1f0] = fuse;
        powerOn();
        EXPECT_EQ(r[kOtgCtl0] & 0x3f, level) << std::hex << hidrev << " " << fuse;
    }
}

// The squelch level is the fuse's; the disconnect level is 7 whatever the boot left.
TEST_F(XusbUtmiPad, BiasLevelsComeFromTheFuseAndL4T)
{
    auto &r = xusbModel().registers;
    r[kHidrev] = kT210B01;
    r[kFuse + 0x1f0] = 2u << 29;
    r[kBiasCtl0] = 1u << 11 | 1u << 3 | 6;
    powerOn();
    EXPECT_EQ(r[kBiasCtl0] & 0x3f, 7u << 3 | 2);
    EXPECT_EQ(r[kBiasCtl0] & (1u << 11), 0u);
}

// Port 0 is OTG in either role (the device trees' usb2-0 mode = "otg"); ports 1-3 keep what they have. VREG_LEV is set
// to 1 with VREG_FIX18 left as found, and prod_c_utmi0 clears bit 5.
TEST_F(XusbUtmiPad, PhyFieldsAsTegra210Usb2PhyPowerOn)
{
    auto &r = xusbModel().registers;
    r[kHidrev] = kT210B01;
    r[kPortCap] = 0x111e;  // port 0: DEVICE_ONLY, INTERNAL and REVERSE_ID set
    r[kChrgCtl1] = 1u << 6 | 1u << 5 | 3u << 7;
    powerOn();
    EXPECT_EQ(r[kPortCap], 0x1113u);  // OTG, not internal, no reverse ID (TRM 22.9.11.1)
    EXPECT_EQ(r[kChrgCtl1], 1u << 6 | 1u << 7);
}

// What a boot stage's charger detection left (hekate clears PD_CHG; a BC1.2 detection sets the D+/D- pull overrides and
// PD2) goes as tegra210_xusb_padctl_utmi_pad_charger_detect_off leaves it.
TEST_F(XusbUtmiPad, ChargerDetectionLeftOff)
{
    auto &r = xusbModel().registers;
    r[kChrgCtl0] = 0;
    r[kChrgCtl1] = 0xffu << 16;
    r[kOtgCtl0] |= 3u << 27;
    powerOn();
    EXPECT_EQ(r[kChrgCtl0] & 1u, 1u);
    EXPECT_EQ(r[kChrgCtl1] & (0xffu << 16), 1u << 17 | 1u << 19 | 1u << 21 | 1u << 23);
    EXPECT_EQ(r[kOtgCtl0] & (3u << 27), 0u);
}

TEST_F(XusbUtmiPad, TrackingKeepsItsClockWhileThePadIsPowered)
{
    powerOn();
    EXPECT_EQ(xusbFirstWrite(kCar + 0x2a0), -1);  // CLK_ENB_Y_CLR: the USB2_TRK clock is never turned off
    EXPECT_EQ(xusbModel().registers[kBiasCtl1] & (1u << 26), 0u);
}

TEST_F(XusbUtmiPad, RunningPllsAreLeftAlone)
{
    auto &r = xusbModel().registers;
    r[kCar + 0x0c0] = 1u << 30 | 1u << 27;  // PLLU_BASE enabled and locked
    r[kCar + 0x52c] = 1u << 31;             // UTMIPLL locked
    ASSERT_TRUE(xpad::startPlls(reinterpret_cast<volatile uint8_t *>(kCar)));
    EXPECT_TRUE(xusbWritesTo(kCar + 0x0cc).empty());  // PLLU_MISC
    EXPECT_TRUE(xusbWritesTo(kCar + 0x480).empty());  // UTMIP_PLL_CFG0
}

// The Tegra X1 TRM's PLL start (5.1.5): IDDQ off, 5 us, program, enable, then the lock bit read once after twice the
// 7-10 us PLLU lock time. A lock bit raises no interrupt and is never polled.
TEST_F(XusbUtmiPad, PlluStartsInTheTrmOrder)
{
    xusbModel().registers[kCar + 0x0cc] = 1u << 31;  // PLLU_MISC: IDDQ on
    xusbModel().registers[kCar + 0x52c] = 1u << 31;
    ASSERT_TRUE(xpad::startPlls(reinterpret_cast<volatile uint8_t *>(kCar)));
    const auto &log = xusbModel().log;
    ASSERT_GE(log.size(), 5u);
    EXPECT_EQ(log[0], xusbWrite(kCar + 0x0cc, 1u << 29));  // IDDQ off, lock detect on
    EXPECT_EQ(log[1], xusbDelay(5));
    EXPECT_EQ(log[2].address, kCar + 0x0c0);  // programmed with ENABLE clear
    EXPECT_EQ(log[2].value & (1u << 30), 0u);
    EXPECT_EQ(log[3].value & (1u << 30), 1u << 30);
    EXPECT_EQ(log[4], xusbDelay(20));
}

TEST_F(XusbUtmiPad, PlluLockIsReadOnceAfterItsLockTime)
{
    xusbModel().pllsLock = false;
    EXPECT_FALSE(xpad::startPlls(reinterpret_cast<volatile uint8_t *>(kCar)));
    EXPECT_EQ(xusbModel().reads[kCar + 0x0c0], 2u);  // the running check, then the lock
    EXPECT_EQ(xusbModel().log.back(), xusbDelay(20));
}

// UTMIPLL: IDDQ off, 10 us, programmed and enabled, its lock read once after twice its 10-15 us lock time.
TEST_F(XusbUtmiPad, UtmipllStartsInTheTrmOrderAndIsReadOnce)
{
    xusbModel().registers[kCar + 0x0c0] = 1u << 30 | 1u << 27;
    ASSERT_TRUE(xpad::startPlls(reinterpret_cast<volatile uint8_t *>(kCar)));
    const auto &log = xusbModel().log;
    const long iddq = xusbFirstWrite(kCar + 0x52c);
    ASSERT_GE(iddq, 0);
    EXPECT_EQ(log[static_cast<size_t>(iddq) + 1], xusbDelay(10));
    EXPECT_LT(iddq, xusbFirstWrite(kCar + 0x480));
    EXPECT_NE(std::ranges::find(log, xusbDelay(30)), log.end());
    EXPECT_EQ(xusbModel().reads[kCar + 0x52c], 3u);  // the running check, the IDDQ read-modify-write, the lock
}

TEST_F(XusbUtmiPad, UtmipllThatNeverLocksFails)
{
    xusbModel().registers[kCar + 0x0c0] = 1u << 30 | 1u << 27;
    xusbModel().pllsLock = false;
    EXPECT_FALSE(xpad::startPlls(reinterpret_cast<volatile uint8_t *>(kCar)));
}

TEST_F(XusbUtmiPad, StoppedPllLocksBeforeItsOutputsGoOn)
{
    ASSERT_TRUE(xpad::startPlls(reinterpret_cast<volatile uint8_t *>(kCar)));
    const auto pllu = xusbWritesTo(kCar + 0x0c0);
    ASSERT_GE(pllu.size(), 3u);
    EXPECT_EQ(pllu[1] & (1u << 30), 1u << 30);
    EXPECT_EQ(pllu[2] & 0x2e00000u, 0x2e00000u);
}
}  // namespace
