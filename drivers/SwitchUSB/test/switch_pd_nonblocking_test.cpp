// SwitchPD.cpp non-blocking workloop transitions: timer-driven 10 ms source-rail ramp (v_vdd5v3 -> VCONN) and
// timer-driven startup SYS_RESET, with zero IOSleep calls and uninterrupted ALERT delivery during the ramp.
#include "drivers/SwitchUSB/SwitchPDService.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace {

struct FakePdHardware {
    uint16_t fwType = 0x0400;
    uint16_t fwRevision = 0x0700;
    uint16_t alert = 0;
    uint16_t status1 = 0;
    uint16_t status2 = 0;
    uint16_t config1 = 0xc000;  // SPDSRC12_OFF
    uint16_t config2 = 0;
    uint16_t dpStatus = 0;
    uint16_t dpAlertEnable = 0;
    uint16_t vendor = 0;
    uint8_t alertEnable[4] = {0xff, 0xfb, 0xff, 0xff};
    std::vector<uint16_t> commands;
    std::map<unsigned, int> gpioOutputs;
    int failGpio = -1;
    bool boostOn = false;
    int boostEnables = 0;
    int boostDisables = 0;
    IOService nub;
};

FakePdHardware *gHw = nullptr;

}  // namespace

extern "C" {
int tegra_i2c_init(unsigned)
{
    return 0;
}

int tegra_i2c_read(unsigned, uint8_t, uint8_t reg, void *buf, size_t len)
{
    auto *b = static_cast<uint8_t *>(buf);
    if (reg == switchpd::ALERT_ENABLE && len == 4) {
        memcpy(b, gHw->alertEnable, 4);
        return 0;
    }
    uint16_t v = 0;
    switch (reg) {
    case switchpd::ALERT_STATUS:
        v = gHw->alert;
        gHw->alert = 0;
        break;
    case switchpd::STATUS1:
        v = gHw->status1;
        break;
    case switchpd::STATUS2:
        v = gHw->status2;
        break;
    case switchpd::CONFIG1:
        v = gHw->config1;
        break;
    case switchpd::CONFIG2:
        v = gHw->config2;
        break;
    case switchpd::DP_STATUS:
        v = gHw->dpStatus;
        break;
    case switchpd::DP_ALERT_EN:
        v = gHw->dpAlertEnable;
        break;
    case switchpd::VENDOR_CONFIG:
        v = gHw->vendor;
        break;
    case switchpd::FW_TYPE:
        v = gHw->fwType;
        break;
    case switchpd::FW_REVISION:
        v = gHw->fwRevision;
        break;
    default:
        memset(b, 0, len);
        return 0;
    }
    b[0] = static_cast<uint8_t>(v);
    b[1] = static_cast<uint8_t>(v >> 8);
    return 0;
}

int tegra_i2c_write(unsigned, uint8_t, uint8_t reg, const void *buf, size_t len)
{
    const auto *b = static_cast<const uint8_t *>(buf);
    if (reg == switchpd::ALERT_ENABLE && len == 4) {
        memcpy(gHw->alertEnable, b, 4);
        return 0;
    }
    const uint16_t v = static_cast<uint16_t>(b[0] | static_cast<unsigned>(b[1]) << 8);
    switch (reg) {
    case switchpd::COMMAND:
        gHw->commands.push_back(v);
        if (v == switchpd::SYS_RESET_CMD) {
            gHw->status1 &= static_cast<uint16_t>(~(0x3u | 0x70u));
            gHw->vendor |= switchpd::VENDOR_CONFIG_OCP_DISABLE;
            gHw->config2 = 0;
        }
        break;
    case switchpd::CONFIG1:
        gHw->config1 = v;
        break;
    case switchpd::CONFIG2:
        gHw->config2 = v;
        break;
    case switchpd::DP_ALERT_EN:
        gHw->dpAlertEnable = v;
        break;
    case switchpd::VENDOR_CONFIG:
        gHw->vendor = v;
        break;
    default:
        break;
    }
    return 0;
}

IOService *tegra_gpio_interrupt_nub(unsigned, unsigned)
{
    return &gHw->nub;
}

int tegra_gpio_output(unsigned gpio, int value)
{
    if (gHw->failGpio == static_cast<int>(gpio) && value != 0)
        return -1;
    gHw->gpioOutputs[gpio] = value;
    return 0;
}

int tegra_gpio_config(unsigned gpio)
{
    return 1 | 2 | (gHw->gpioOutputs[gpio] ? 4 : 0);
}

void tegra_gpio_describe(unsigned, char *buffer, size_t size)
{
    snprintf(buffer, size, "gpio");
}

int switch_battery_set_otg_boost(bool on, bool)
{
    gHw->boostOn = on;
    if (on)
        ++gHw->boostEnables;
    else
        ++gHw->boostDisables;
    return 0;
}

int switch_battery_prepare_pd_input(const struct SwitchPDState *)
{
    return 0;
}
}  // extern "C"

void switch_pd_alt_alert(IOService *, IOWorkLoop *, bool (*)(const void *), uint16_t, uint16_t, bool, bool, bool)
{}

bool switch_pd_alt_partner_is_usb_host()
{
    return false;
}

void switch_pd_alt_leave()
{}

void switch_pd_alt_resume()
{}

namespace {

class SwitchPdNonblockingTest : public ::testing::Test {
protected:
    FakePdHardware hw;
    SwitchPD *pd = nullptr;
    SwitchPDState observed = {};

    void SetUp() override
    {
        gHw = &hw;
        gPdNow = 0;
        gPdSleptMs = 0;
        gPdDelayedUs = 0;
        gPdLog.clear();
        gPdPowerInterests.clear();
        IOInterruptEventSource::all.clear();
        IOTimerEventSource::all.clear();
    }

    void TearDown() override
    {
        pd = nullptr;
        gHw = nullptr;
    }

    void startController(bool allowSource = true)
    {
        pd = new SwitchPD;
        uint16_t type = 0;
        uint16_t revision = 0;
        uint16_t config = 0;
        bool turnedOff = false;
        ASSERT_TRUE(pd->begin(type, revision, config, turnedOff));
        ASSERT_EQ(pd->addListener({
                      [](void *ctx, const SwitchPDState *s) {
                          *static_cast<SwitchPDState *>(ctx) = *s;
                      },
                      &observed,
                  }),
                  kIOReturnSuccess);
        if (allowSource)
            pd->setAllowSource(true);
    }

    static void deliverAlerts()
    {
        for (int round = 0; round < 32 && gHw->alert != 0; ++round) {
            bool fired = false;
            for (auto *src : IOInterruptEventSource::all) {
                if (src->enabled && gHw->alert != 0) {
                    fired = true;
                    src->fire();
                }
            }
            if (!fired)
                break;
        }
    }

    static void advanceMs(uint64_t ms)
    {
        const uint64_t target = gPdNow + ms * 1000000ULL;
        for (;;) {
            deliverAlerts();
            IOTimerEventSource *next = nullptr;
            for (auto *t : IOTimerEventSource::all) {
                if (t->armed && t->deadline <= target && (!next || t->deadline < next->deadline))
                    next = t;
            }
            if (!next)
                break;
            if (next->deadline > gPdNow)
                gPdNow = next->deadline;
            next->fire();
        }
        if (target > gPdNow)
            gPdNow = target;
        deliverAlerts();
    }

    static bool alertEnabled()
    {
        return !IOInterruptEventSource::all.empty() && IOInterruptEventSource::all.back()->enabled;
    }
};

TEST_F(SwitchPdNonblockingTest, WaitSourceRailRampUsesTimerNotSleep)
{
    startController(true);
    EXPECT_EQ(gPdSleptMs, 0u);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kSourceRailGpio], 0);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kVconnGpio], 0);

    // Sink partner attaches with Rd + OTG_INSERT: enters WaitSource immediately, raises PX3, defers PK5 by 10 ms.
    hw.status1 = 0x1280;
    hw.status2 = 0x2000;
    hw.alert = SWITCH_PD_ALERT_PLUGPULL;
    deliverAlerts();

    EXPECT_EQ(gPdSleptMs, 0u);
    EXPECT_TRUE(hw.boostOn);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kSourceRailGpio], 1);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kVconnGpio], 0) << "PK5 waits for kRailRampMS timer";
    EXPECT_EQ(hw.config2 & switchpd::CONFIG2_VSRC_SWAP, 0u);

    advanceMs(switchpd::kRailRampMS - 1);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kVconnGpio], 0);

    advanceMs(1);
    EXPECT_EQ(gPdSleptMs, 0u);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kSourceRailGpio], 1);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kVconnGpio], 1);
    EXPECT_NE(hw.config2 & switchpd::CONFIG2_VSRC_SWAP, 0u);
}

TEST_F(SwitchPdNonblockingTest, MidRampAlertIsNotBlockedAndCancelsPendingVconn)
{
    startController(true);
    hw.status1 = 0x1280;
    hw.status2 = 0x2000;
    hw.alert = SWITCH_PD_ALERT_PLUGPULL;
    deliverAlerts();
    ASSERT_EQ(hw.gpioOutputs[switchpd::kSourceRailGpio], 1);
    ASSERT_EQ(hw.gpioOutputs[switchpd::kVconnGpio], 0);

    // Partner detaches 3 ms into the 10 ms ramp: ALERT runs immediately and cancels the pending VCONN timer.
    advanceMs(3);
    hw.status1 = 0;
    hw.status2 = 0;
    hw.alert = SWITCH_PD_ALERT_PLUGPULL;
    deliverAlerts();

    EXPECT_EQ(gPdSleptMs, 0u);
    EXPECT_FALSE(hw.boostOn);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kSourceRailGpio], 0);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kVconnGpio], 0);
    EXPECT_EQ(observed.role, SWITCH_PD_ROLE_NONE);

    advanceMs(20);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kVconnGpio], 0) << "canceled ramp timer must never raise PK5";
    EXPECT_EQ(hw.gpioOutputs[switchpd::kSourceRailGpio], 0);
    EXPECT_EQ(gPdSleptMs, 0u);
}

TEST_F(SwitchPdNonblockingTest, MidRampSrcPluginPromotesAndStillCompletesVconnAt10Ms)
{
    startController(true);
    hw.status1 = 0x1280;
    hw.status2 = 0x2000;
    hw.alert = SWITCH_PD_ALERT_PLUGPULL;
    deliverAlerts();

    // SRC_PLUGIN arrives at 4 ms (mid-ramp): promotes to SOURCE immediately, then PK5 completes at 10 ms.
    advanceMs(4);
    hw.status1 = 0x9680;  // SPDSRC1 | VSAFE | SRC_MODE | DFP | INSERT
    hw.status2 = 0x2000;
    hw.alert = SWITCH_PD_ALERT_SRC_PLUGIN;
    deliverAlerts();

    EXPECT_EQ(gPdSleptMs, 0u);
    EXPECT_EQ(observed.role, SWITCH_PD_ROLE_SOURCE);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kVconnGpio], 0);

    advanceMs(6);
    EXPECT_EQ(gPdSleptMs, 0u);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kSourceRailGpio], 1);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kVconnGpio], 1);
    EXPECT_NE(hw.config2 & switchpd::CONFIG2_VSRC_SWAP, 0u);
}

TEST_F(SwitchPdNonblockingTest, MidRampVconnFailureRollsBackToHeld)
{
    startController(true);
    hw.failGpio = static_cast<int>(switchpd::kVconnGpio);
    hw.status1 = 0x1280;
    hw.status2 = 0x2000;
    hw.alert = SWITCH_PD_ALERT_PLUGPULL;
    deliverAlerts();
    ASSERT_EQ(hw.gpioOutputs[switchpd::kSourceRailGpio], 1);

    advanceMs(switchpd::kRailRampMS);
    EXPECT_EQ(gPdSleptMs, 0u);
    EXPECT_FALSE(hw.boostOn);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kSourceRailGpio], 0);
    EXPECT_EQ(hw.gpioOutputs[switchpd::kVconnGpio], 0);
    EXPECT_FALSE(observed.settled);
    EXPECT_NE(observed.role, SWITCH_PD_ROLE_SOURCE);
}

TEST_F(SwitchPdNonblockingTest, StartupSystemResetUsesTimerNotSleep)
{
    hw.status1 = 0x0280;  // INSERT | DFP: triggers switch_pd_reset_at_start
    hw.vendor = switchpd::VENDOR_CONFIG_OCP_DISABLE;
    startController(false);

    EXPECT_EQ(gPdSleptMs, 0u) << "startup SYS_RESET must not IOSleep";
    ASSERT_FALSE(hw.commands.empty());
    EXPECT_EQ(hw.commands.front(), switchpd::SYS_RESET_CMD);
    EXPECT_FALSE(alertEnabled()) << "ALERT stays masked during the 100 ms reset window";
    EXPECT_NE(hw.vendor & switchpd::VENDOR_CONFIG_OCP_DISABLE, 0u) << "OCP waits for reset window end";

    advanceMs(switchpd::kStartResetMS - 1);
    EXPECT_FALSE(alertEnabled());
    EXPECT_NE(hw.vendor & switchpd::VENDOR_CONFIG_OCP_DISABLE, 0u);

    advanceMs(1);
    EXPECT_EQ(gPdSleptMs, 0u);
    EXPECT_TRUE(alertEnabled()) << "ALERT re-enabled after startup reset timer";
    EXPECT_EQ(hw.vendor & switchpd::VENDOR_CONFIG_OCP_DISABLE, 0u) << "OCP re-enabled after startup reset timer";
    EXPECT_EQ(hw.config2 & (switchpd::CONFIG2_PR_SWAP_MASK | switchpd::CONFIG2_DR_SWAP_MASK),
              switchpd::CONFIG2_PR_SWAP_ACCEPT_SRC_REJECT_SNK | switchpd::CONFIG2_DR_SWAP_ALWAYS_ACCEPT);
}

}  // namespace
