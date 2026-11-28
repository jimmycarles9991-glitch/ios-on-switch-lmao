// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "../SwitchButtons.cpp"

namespace {
struct FakeHidDevice : IOService {
    SwitchHIDKind kind = kSwitchHIDTouch;
    IOWorkLoop *sharedLoop = nullptr;

    void free() override
    {
        OSSafeReleaseNULL(sharedLoop);
        IOService::free();
    }
};

FakeHidDevice *gHidDevice = nullptr;
IOService gVolUpNub;
IOService gVolDownNub;
IOService gPmicNub;
int gVolUpLevel = 1;
int gVolDownLevel = 1;
uint32_t gPmcCntrl = 0;
uint8_t gPmicRegs[256] = {};
std::vector<SwitchButtonsReport> gReports;
std::vector<unsigned> gWakeEvents;

void resetMocks()
{
    if (gHidDevice) {
        gHidDevice->release();
        gHidDevice = nullptr;
    }
    gVolUpLevel = 1;
    gVolDownLevel = 1;
    gPmcCntrl = 0;
    std::memset(gPmicRegs, 0, sizeof(gPmicRegs));
    gReports.clear();
    gWakeEvents.clear();
    IOInterruptEventSource::all.clear();
    IOTimerEventSource::all.clear();
    gNowNS = 1000000000ULL;
}
}  // namespace

extern "C" {
IOService *tegra_gpio_interrupt_nub(unsigned gpio, unsigned)
{
    if (gpio == TEGRA_GPIO_VOLUME_UP)
        return &gVolUpNub;
    if (gpio == TEGRA_GPIO_VOLUME_DOWN)
        return &gVolDownNub;
    return nullptr;
}

void tegra_gpio_describe(unsigned, char *buf, size_t size)
{
    std::snprintf(buf, size, "gpio stub");
}

int tegra_gpio_set_debounce(unsigned, unsigned)
{
    return 0;
}

int tegra_gpio_read(unsigned gpio)
{
    if (gpio == TEGRA_GPIO_VOLUME_UP)
        return gVolUpLevel;
    if (gpio == TEGRA_GPIO_VOLUME_DOWN)
        return gVolDownLevel;
    return 1;
}

void tegra_gpio_check_delivery(unsigned, const char *)
{}

int tegra_i2c_init(unsigned)
{
    return TEGRA_I2C_OK;
}

int tegra_i2c_read(unsigned, uint8_t, uint8_t reg, void *buf, size_t len)
{
    if (len == 1)
        *static_cast<uint8_t *>(buf) = gPmicRegs[reg];
    return TEGRA_I2C_OK;
}

int tegra_i2c_write(unsigned, uint8_t, uint8_t reg, const void *buf, size_t len)
{
    if (len == 1)
        gPmicRegs[reg] = *static_cast<const uint8_t *>(buf);
    return TEGRA_I2C_OK;
}

void switch_pmc_rmw(uint32_t offset, uint32_t clear, uint32_t set)
{
    if (offset == 0)
        gPmcCntrl = (gPmcCntrl & ~clear) | set;
}

uint32_t switch_pmc_read(uint32_t offset)
{
    return offset == 0 ? gPmcCntrl : 0;
}

void switch_rtc_interrupt(void)
{}

void switch_wake_event(unsigned source)
{
    gWakeEvents.push_back(source);
}
}  // extern "C"

IOService *switch_interrupt_nub(uint32_t intid, bool)
{
    return intid == 118 ? &gPmicNub : nullptr;
}

void switch_interrupt_describe(uint32_t, char *buf, size_t size)
{
    std::snprintf(buf, size, "gic stub");
}

int switch_check_after_ms(IOTimerEventSource *, uint32_t)
{
    return 0;
}

IOService *switch_hid_create(SwitchHIDKind kind, IOWorkLoop *sharedLoop)
{
    auto *dev = new FakeHidDevice;
    dev->kind = kind;
    if (sharedLoop) {
        sharedLoop->retain();
        dev->sharedLoop = sharedLoop;
    }
    gHidDevice = dev;
    return dev;
}

IOService *switch_hid_create(SwitchHIDKind kind)
{
    return switch_hid_create(kind, nullptr);
}

IOReturn switch_hid_post(IOService *, const void *report, size_t length)
{
    if (length != sizeof(SwitchButtonsReport))
        return -1;
    SwitchButtonsReport r = {};
    std::memcpy(&r, report, sizeof(r));
    gReports.push_back(r);
    return kIOReturnSuccess;
}

TEST(SwitchButtons, SharesWorkLoopAndPostsReports)
{
    resetMocks();
    const unsigned loopsBefore = IOWorkLoop::created;

    ASSERT_EQ(switch_buttons_start(), 0);
    EXPECT_EQ(IOWorkLoop::created, loopsBefore + 1u);
    ASSERT_NE(IOWorkLoop::last, nullptr);
    ASSERT_NE(gHidDevice, nullptr);
    EXPECT_EQ(gHidDevice->kind, kSwitchHIDButtons);
    EXPECT_EQ(gHidDevice->sharedLoop, IOWorkLoop::last);

    IOInterruptEventSource *volUp = nullptr;
    IOInterruptEventSource *pmic = nullptr;
    for (IOInterruptEventSource *src : IOInterruptEventSource::all) {
        if (src->provider == &gVolUpNub)
            volUp = src;
        else if (src->provider == &gPmicNub)
            pmic = src;
    }
    ASSERT_NE(volUp, nullptr);
    ASSERT_NE(pmic, nullptr);

    gVolUpLevel = 0;
    volUp->action(volUp->owner, volUp, 1);
    ASSERT_EQ(gReports.size(), 1u);
    EXPECT_EQ(gReports.back().buttons, kSwitchButtonVolumeUp);

    gVolUpLevel = 1;
    volUp->action(volUp->owner, volUp, 1);
    ASSERT_EQ(gReports.size(), 2u);
    EXPECT_EQ(gReports.back().buttons, 0u);

    gPmicRegs[0x05] = 1u << 1;
    gPmicRegs[0x0b] = 1u << 2;
    gPmicRegs[0x15] = 1u << 2;
    pmic->action(pmic->owner, pmic, 1);
    ASSERT_EQ(gReports.size(), 3u);
    EXPECT_EQ(gReports.back().buttons, kSwitchButtonPower);
    ASSERT_EQ(gWakeEvents.size(), 1u);
    EXPECT_EQ(gWakeEvents.back(), SWITCH_WAKE_POWER_KEY);

    gPmicRegs[0x0b] = 1u << 3;
    gPmicRegs[0x15] = 0;
    pmic->action(pmic->owner, pmic, 1);
    ASSERT_EQ(gReports.size(), 4u);
    EXPECT_EQ(gReports.back().buttons, 0u);

    switch_buttons_bt_click(SWITCH_BT_HOME | SWITCH_BT_CAPTURE);
    ASSERT_EQ(gReports.size(), 5u);
    EXPECT_EQ(gReports.back().buttons, static_cast<uint8_t>(kSwitchButtonHome | kSwitchButtonSnapshot));

    ASSERT_GE(IOTimerEventSource::all.size(), 2u);
    IOTimerEventSource *btTimer = IOTimerEventSource::all.back();
    EXPECT_TRUE(btTimer->armed);
    gNowNS = btTimer->deadline;
    btTimer->fire();
    ASSERT_EQ(gReports.size(), 6u);
    EXPECT_EQ(gReports.back().buttons, 0u);
}

TEST(SwitchButtons, TeardownRetainsSharedWorkLoop)
{
    resetMocks();
    const unsigned liveBefore = IOWorkLoop::live;
    const unsigned loopsBefore = IOWorkLoop::created;

    auto *driver = OSTypeAlloc(SwitchButtons);
    ASSERT_TRUE(driver->init());
    ASSERT_TRUE(driver->start(IOService::getResourceService()));
    EXPECT_EQ(IOWorkLoop::created, loopsBefore + 1u);
    EXPECT_EQ(IOWorkLoop::live, liveBefore + 1u);
    ASSERT_NE(gHidDevice, nullptr);
    EXPECT_EQ(gHidDevice->sharedLoop, IOWorkLoop::last);

    driver->release();
    EXPECT_EQ(IOWorkLoop::live, liveBefore + 1u);

    gHidDevice->release();
    gHidDevice = nullptr;
    EXPECT_EQ(IOWorkLoop::live, liveBefore);
}
