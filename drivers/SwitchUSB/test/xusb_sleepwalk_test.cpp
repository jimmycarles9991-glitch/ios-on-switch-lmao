// XusbSleepwalk.h on the host: pad 0's sleepwalk phases per speed (pulls on D+ and D- in every phase, phase A idle
// high-Z, phases B to D drive the K: D- for full and high speed, D+ for low speed, nothing for no known speed), PORTSC's
// speed IDs as the PMC saves them, and the pad trims moved from the XUSB pad controller's fields into the PMC's.
#include <gtest/gtest.h>

#include <cstdint>

#include "../XusbSleepwalk.h"

namespace {

constexpr uint32_t kPullDowns = (3u << 0) | (3u << 8) | (3u << 16) | (3u << 24);
constexpr uint32_t kHighZ[4] = {1u << 6, 1u << 14, 1u << 22, 1u << 30};
constexpr uint32_t kDrivePlus[4] = {1u << 4, 1u << 12, 1u << 20, 1u << 28};
constexpr uint32_t kDriveMinus[4] = {1u << 5, 1u << 13, 1u << 21, 1u << 29};

uint32_t portsc(uint32_t speedId)
{
    return 1u | speedId << 10;
}

}  // namespace

TEST(XusbSleepwalk, PhasesKeepTheIdleStateAndDriveTheK)
{
    const uint32_t fullOrHigh =
        kPullDowns | kHighZ[0] | kDrivePlus[0] | kDriveMinus[1] | kDriveMinus[2] | kDriveMinus[3];
    EXPECT_EQ(xusb_sleepwalk_phases(XUSB_UTMI_HS), fullOrHigh);
    EXPECT_EQ(xusb_sleepwalk_phases(XUSB_UTMI_FS), fullOrHigh);
    EXPECT_EQ(xusb_sleepwalk_phases(XUSB_UTMI_LS),
              kPullDowns | kHighZ[0] | kDriveMinus[0] | kDrivePlus[1] | kDrivePlus[2] | kDrivePlus[3]);
    EXPECT_EQ(xusb_sleepwalk_phases(XUSB_UTMI_RESET), kPullDowns | kHighZ[0] | kHighZ[1] | kHighZ[2] | kHighZ[3]);
}

TEST(XusbSleepwalk, PortSpeedAsThePmcSavesIt)
{
    EXPECT_EQ(xusb_utmi_speed(portsc(1)), XUSB_UTMI_FS);
    EXPECT_EQ(xusb_utmi_speed(portsc(2)), XUSB_UTMI_LS);
    EXPECT_EQ(xusb_utmi_speed(portsc(3)), XUSB_UTMI_HS);
    EXPECT_EQ(xusb_utmi_speed(portsc(0)), XUSB_UTMI_RESET);
    EXPECT_EQ(xusb_utmi_speed(portsc(4)), XUSB_UTMI_RESET);  // SuperSpeed is not pad 0's to keep
}

TEST(XusbSleepwalk, PadTrimsMoveIntoThePmcFields)
{
    // USB2_BIAS_PAD_CTL1: TCTRL 0x15 (bits 0-5), PCTRL 0x2a (bits 6-11); TERM_PAD_CFG: PCTRL bits 1-6, TCTRL 7-12.
    const uint32_t bias = 0x15u | 0x2au << 6 | 0xffu << 20;
    EXPECT_EQ(xusb_sleepwalk_term(0x80000001u | 0xfffu << 1, bias), 0x80000001u | 0x2au << 1 | 0x15u << 7);
    // USB2_OTG_PAD0_CTL1 RPD_CTRL (bits 26-30) into PAD_CFG0's bits 22-26, nothing else moved.
    EXPECT_EQ(xusb_sleepwalk_rpd(0x1u | 0x1fu << 22, 0x0bu << 26 | 0x3ffu), 0x1u | 0x0bu << 22);
}
