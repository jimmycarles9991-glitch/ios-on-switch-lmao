// XusbPadctl.cpp against the register model: device mode takes only the fake SS port (SS port 0) out of ELPG, in
// tegra210_usb2_phy_power_on's order, and leaves every other ELPG bit (wake enables and events, the other ports'
// clamps, PCIe's AUX clamps) as it found them.
#include "drivers/SwitchUSB/XusbPadctl.h"
#include "xusb_register_model.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace {
constexpr uintptr_t kPadctl = 0x7009F000;
constexpr uintptr_t kSsPortMap = kPadctl + 0x014, kElpg0 = kPadctl + 0x020, kElpg1 = kPadctl + 0x024;
// Every SS port clamped and its VCORE down, the AUX mux clamped (bits 29-31), SS port 0 unmapped (7), SS port 3 mapped.
constexpr uint32_t kElpg1AllClamped = 0xe0000fffu, kPortMap = 3u << 15 | 7u;
// USB2 port 0's wake interrupt enable and a pending SS port 1 wake event.
constexpr uint32_t kElpg0Wake = 1u << 0 | 1u << 22;

volatile uint8_t *padctl()
{
    return reinterpret_cast<volatile uint8_t *>(kPadctl);
}

TEST(XusbPadctl, DeviceModeReleasesOnlyTheFakePort)
{
    xusbModelReset();
    auto &r = xusbModel().registers;
    r[kElpg0] = kElpg0Wake;
    r[kElpg1] = kElpg1AllClamped;
    r[kSsPortMap] = kPortMap;
    switch_usb_padctl_fake_port_up(padctl());
    EXPECT_EQ(xusbModel().log, (std::vector<XusbEvent>{
                                   xusbWrite(kSsPortMap, 3u << 15),
                                   xusbWrite(kElpg1, kElpg1AllClamped & ~(1u << 2)),
                                   xusbDelay(100),
                                   xusbWrite(kElpg1, kElpg1AllClamped & ~(1u << 2 | 1u << 1)),
                                   xusbDelay(100),
                                   xusbWrite(kElpg1, kElpg1AllClamped & ~7u),
                               }));
    EXPECT_EQ(r[kElpg0], kElpg0Wake);
}

// Device mode off: SS port 0 back into ELPG in the TRM's entry order (22.8.7.1: CLAMP_EN_EARLY, CLAMP_EN, 250 us, then
// VCORE_DOWN; the writes not combined), then unmapped; nothing else in ELPG_PROGRAM_0/1 moves.
TEST(XusbPadctl, DeviceModeOffClampsTheFakePortAgain)
{
    xusbModelReset();
    auto &r = xusbModel().registers;
    constexpr uint32_t kOthersClamped = 0xe0000ff8u;
    r[kElpg0] = kElpg0Wake;
    r[kElpg1] = kOthersClamped;
    r[kSsPortMap] = 3u << 15;
    switch_usb_padctl_fake_port_down(padctl());
    EXPECT_EQ(xusbModel().log, (std::vector<XusbEvent>{
                                   xusbWrite(kElpg1, kOthersClamped | 1u << 1),
                                   xusbDelay(100),
                                   xusbWrite(kElpg1, kOthersClamped | 1u << 1 | 1u << 0),
                                   xusbDelay(250),
                                   xusbWrite(kElpg1, kOthersClamped | 7u),
                                   xusbWrite(kSsPortMap, 3u << 15 | 7u),
                               }));
    EXPECT_EQ(r[kElpg0], kElpg0Wake);
}

TEST(XusbPadctl, ModifyIsOneReadModifyWrite)
{
    xusbModelReset();
    xusbModel().registers[kSsPortMap] = kPortMap;
    switch_padctl_modify(padctl(), 0x014, 0xfu, 0);
    EXPECT_EQ(xusbWritesTo(kSsPortMap), (std::vector<uint32_t>{3u << 15}));
}

TEST(XusbPadctl, FakePortLeavesTheLiveSuperSpeedLaneUntouched)
{
    xusbModelReset();
    xusbModel().realLaneUp = true;
    switch_usb_padctl_fake_port_up(padctl());
    switch_usb_padctl_fake_port_down(padctl());
    EXPECT_TRUE(xusbModel().log.empty());
}

TEST(XusbPadctl, TheLockSerialisesItsHolders)
{
    xusbModelReset();
    switch_padctl_lock();
    std::atomic<bool> modified = false;
    std::thread other([&] {
        switch_padctl_modify(padctl(), 0x014, 0, 1);
        modified = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(modified);
    switch_padctl_unlock();
    other.join();
    EXPECT_TRUE(modified);
}
}  // namespace
