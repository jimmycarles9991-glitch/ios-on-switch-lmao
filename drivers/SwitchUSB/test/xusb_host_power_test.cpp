// XusbHostPower.cpp against the register model: the XUSB host powers up in tegra_xusb_probe's order (clock sources and
// PLLs, the USB2 pad, partitions XUSBA then XUSBC, each clock after its partition, the host's memory client released
// last, then tegra_xusb_config), and the falcon runs from PLL_RE_OUT as L4T's clock driver sets it.
#include "drivers/SwitchUSB/XusbHost.h"
#include "xusb_register_model.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>

namespace {
constexpr uintptr_t kCar = 0x60006000, kPmc = 0x7000e400, kMc = 0x70019000, kPadctl = 0x7009F000, kIpfs = 0x70099000;
constexpr uintptr_t kPllreBase = kCar + 0x4c4, kPllreMisc = kCar + 0x4c8, kFalconSource = kCar + 0x604;
constexpr uint32_t kToggle = 0x30, kStart = 1u << 8, kEnable = 1u << 30, kLock = 1u << 27, kIddq = 1u << 24;
constexpr uintptr_t kHidrev = 0x70000804;
constexpr uint32_t kT210 = 0x21u << 8 | 1u << 4, kT210B01 = 0x21u << 8 | 2u << 4;

void bootState(uint32_t hidrev)
{
    auto &r = xusbModel().registers;
    r[kHidrev] = hidrev;
    r[kCar + 0x0c0] = 1u << 30 | 1u << 27;  // PLLU running
    r[kCar + 0x52c] = 1u << 31;             // UTMIPLL locked
    r[kPadctl + 0x088] = 1u << 26 | 1u << 29;
}

long toggleOf(unsigned partition)
{
    return xusbFirstWrite(kPmc + kToggle, partition | kStart);
}

class XusbHostPower : public ::testing::Test {
public:
    xhost::Controller host;
    char why[160] = "";

protected:
    void SetUp() override
    {
        xusbModelReset();
        ASSERT_TRUE(host.map());
    }
};

TEST_F(XusbHostPower, FollowsTegraXusbProbeOrder)
{
    bootState(kT210B01);
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    const long falconSource = xusbFirstWrite(kFalconSource);
    const long bias = xusbFirstWrite(kPadctl + 0x284);
    const long xusba = toggleOf(20);
    const long xusbc = toggleOf(22);
    const long release = xusbFirstWrite(kMc + 0x200);
    const long fpci = xusbFirstWrite(kIpfs + 0x180, 1);
    ASSERT_GE(falconSource, 0);
    ASSERT_GE(bias, 0);
    ASSERT_GE(xusba, 0);
    ASSERT_GE(xusbc, 0);
    EXPECT_LT(falconSource, bias);
    EXPECT_LT(bias, xusba);
    EXPECT_LT(xusba, xusbc);
    EXPECT_LT(xusbc, release);
    EXPECT_LT(release, fpci);
    // Each partition's clock goes on after its toggle (Tegra X1 TRM 12.2 and 22.8.7.5).
    EXPECT_LT(xusbc, xusbFirstWrite(kCar + 0x330, 1u << 25));
    EXPECT_LT(xusba, xusbFirstWrite(kCar + 0x448, 1u << 28));
    EXPECT_EQ(xusbWritesTo(kMc + 0x200), (std::vector<uint32_t>{~(1u << 19)}));
}

// clk-tegra210.c's init table: XUSB_FALCON_SRC from PLL_RE_OUT (mux index 5) at 336 MHz, PLLRE's VCO 672 MHz from
// pll_ref (38.4 MHz x 70 / 4) with its defaults, IDDQ off and lock wait in clk_pll_enable's order.
// After SC7 the PMC keeps the partitions on and the CAR comes back with their clocks off: each clock goes on again.
TEST_F(XusbHostPower, PartitionsFoundOnGetTheirClocks)
{
    bootState(kT210B01);
    xusbModel().registers[kPmc + 0x38] = 1u << 20 | 1u << 22;
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    EXPECT_EQ(toggleOf(20), -1);
    EXPECT_EQ(toggleOf(22), -1);
    EXPECT_GE(xusbFirstWrite(kCar + 0x330, 1u << 25), 0);
    EXPECT_GE(xusbFirstWrite(kCar + 0x448, 1u << 28), 0);
}

// The xHCI's BAR is memory space with posted writes before FPCI goes on, and CFG_1 enables memory space and bus
// master only (Tegra X1 TRM 22.8.4 step 9).
TEST_F(XusbHostPower, BarIsPostedMemorySpace)
{
    bootState(kT210B01);
    xusbModel().registers[kIpfs + 0x080] = 0x00700901;
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    EXPECT_EQ(xusbModel().registers[kIpfs + 0x080], 0x00700900u);
    EXPECT_LT(xusbFirstWrite(kIpfs + 0x080), xusbFirstWrite(kIpfs + 0x180, 1));
    EXPECT_EQ(xusbModel().registers[0x70098000 + 0x004] & 7u, 6u);
}

// FS logic at 48 MHz: FO_48M undivided (Tegra X1 TRM 22.8.4 step 6 p.1351, XUSB_FS_CLK_DIVISOR 0), whatever divisor a
// boot stage left.
TEST_F(XusbHostPower, FsClockIsFo48Undivided)
{
    bootState(kT210B01);
    xusbModel().registers[kCar + 0x608] = 0x5;
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    EXPECT_EQ(xusbModel().registers[kCar + 0x608], 2u << 29);
}

TEST_F(XusbHostPower, FalconFromPllreAt336MHz)
{
    bootState(kT210B01);
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    const auto base = xusbWritesTo(kPllreBase);
    const auto misc = xusbWritesTo(kPllreMisc);
    ASSERT_EQ(base.size(), 3u);
    EXPECT_EQ(base[0], 0u);                        // defaults: disabled, bits 28:26 clear
    EXPECT_EQ(base[1] & 0x1fffff, 70u << 8 | 4u);  // DIVN 70, DIVM 4, DIVP 0 (undivided)
    EXPECT_EQ(base[2], kEnable | 70u << 8 | 4u);   // enabled last
    ASSERT_EQ(misc.size(), 2u);
    EXPECT_EQ(misc[0], 0x41000000u);  // lock enable, IDDQ
    EXPECT_EQ(misc[1] & kIddq, 0u);   // IDDQ off before the enable
    EXPECT_LT(xusbFirstWrite(kPllreMisc, 0x41000000), xusbFirstWrite(kPllreBase, kEnable));
    EXPECT_LT(xusbFirstWrite(kPllreBase, kEnable), xusbFirstWrite(kFalconSource));
    EXPECT_EQ(xusbWritesTo(kFalconSource), (std::vector<uint32_t>{5u << 29 | 2u}));
}

// A T210B01 whose boot loader took UTMIPLL's 60 MHz for PLLRE (CLKIN_SEL) runs it at 625 MHz (x 125 / 12).
// The Tegra X1 TRM's PLL start (5.1.5): IDDQ off and 5 us before the dividers are programmed, then ENABLE.
// tegra_xusb_disable_hsic_wake runs on a T210B01 only (tegra210b01_soc.disable_hsic_wake, tegra210_soc false).
// The host role sets port 0 OTG with INTERNAL and REVERSE_ID clear (Tegra X1 TRM 22.9.11.1), whatever was there.
TEST_F(XusbHostPower, HostRolePortCapIsPlainOtg)
{
    bootState(kT210B01);
    xusbModel().registers[kPadctl + 0x008] = 0x110e;
    host.setHostRole(true);
    EXPECT_EQ(xusbModel().registers[kPadctl + 0x008], 0x1103u);
}

TEST_F(XusbHostPower, HsicWakeDisabledOnT210B01Only)
{
    constexpr uintptr_t kC11PageSel = 0x70098000 + 0x404;
    bootState(kT210B01);
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    EXPECT_FALSE(xusbWritesTo(kC11PageSel).empty());
    xusbModelReset();
    xhost::Controller t210;
    ASSERT_TRUE(t210.map());
    bootState(kT210);
    ASSERT_TRUE(t210.powerOn(why, sizeof(why))) << why;
    EXPECT_TRUE(xusbWritesTo(kC11PageSel).empty());
}

TEST_F(XusbHostPower, PllreProgrammedOutOfIddq)
{
    bootState(kT210B01);
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    const auto &log = xusbModel().log;
    long iddqOff = -1;
    long dividers = -1;
    for (size_t i = 0; i < log.size(); ++i) {
        const XusbEvent &e = log[i];
        if (e.kind != XusbEvent::Write)
            continue;
        if (iddqOff < 0 && e.address == kPllreMisc && (e.value & kIddq) == 0)
            iddqOff = static_cast<long>(i);
        if (dividers < 0 && e.address == kPllreBase && (e.value & 0x1fffffu) == (70u << 8 | 4u))
            dividers = static_cast<long>(i);
    }
    ASSERT_GE(iddqOff, 0);
    ASSERT_GE(dividers, 0);
    EXPECT_LT(iddqOff, dividers);
    EXPECT_EQ(log[static_cast<size_t>(iddqOff) + 1], xusbDelay(5));
    EXPECT_LT(dividers, xusbFirstWrite(kPllreBase, kEnable));
}

TEST_F(XusbHostPower, PllreFromUtmipllAt625MHz)
{
    bootState(kT210B01);
    xusbModel().registers[kPllreBase] = 1u << 22;
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    EXPECT_EQ(xusbModel().registers[kPllreBase], 1u << 22 | kEnable | 125u << 8 | 12u);
}

// CLKIN_SEL means nothing on a T210: 672 MHz from pll_ref.
TEST_F(XusbHostPower, T210PllreFromPllRef)
{
    bootState(kT210);
    xusbModel().registers[kPllreBase] = 1u << 22;
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    EXPECT_EQ(xusbModel().registers[kPllreBase] & 0x1fffff, 70u << 8 | 4u);
}

TEST_F(XusbHostPower, LockedPllreAtItsRateIsLeftRunning)
{
    bootState(kT210B01);
    auto &r = xusbModel().registers;
    r[kPllreBase] = kEnable | 70u << 8 | 4u;
    r[kPllreMisc] = kLock;
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    EXPECT_TRUE(xusbWritesTo(kPllreBase).empty());
    EXPECT_TRUE(xusbWritesTo(kPllreMisc).empty());
}

TEST_F(XusbHostPower, PllreThatNeverLocksFailsTheBringUp)
{
    bootState(kT210B01);
    xusbModel().afterWrite = [](uintptr_t address, uint32_t) {
        if (address == kPllreBase)
            xusbModel().registers[kPllreMisc] &= ~kLock;
    };
    EXPECT_FALSE(host.powerOn(why, sizeof(why)));
    EXPECT_NE(std::string(why).find("PLLRE did not lock"), std::string::npos);
    EXPECT_TRUE(xusbWritesTo(kFalconSource).empty());
    // IDDQ's read-modify-write, one look at the lock bit after twice L4T's lock time, the reason line: never polled.
    EXPECT_EQ(xusbModel().reads[kPllreMisc], 3u);
    EXPECT_NE(std::find(xusbModel().log.begin(), xusbModel().log.end(), xusbSleep(1)), xusbModel().log.end());
}

// tegra_xhci_save_context's registers, FPCI then IPFS in tegra_xhci_restore_context's order.
constexpr uintptr_t kFpci = 0x70098000;
constexpr std::array<uintptr_t, 19> kContext = {
    kFpci + 0x478, kFpci + 0x47c, kFpci + 0x480, kFpci + 0x484, kFpci + 0x43c, kFpci + 0x0f8, kFpci + 0x060,
    kFpci + 0x040, kIpfs + 0x0c0, kIpfs + 0x0c4, kIpfs + 0x0c8, kIpfs + 0x100, kIpfs + 0x140, kIpfs + 0x184,
    kIpfs + 0x188, kIpfs + 0x198, kIpfs + 0x19c, kIpfs + 0x1bc, kIpfs + 0x1dc};
constexpr uintptr_t kVbusId = kPadctl + 0xc60;

TEST_F(XusbHostPower, FirstPowerOnRestoresNothing)
{
    bootState(kT210B01);
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    EXPECT_EQ(xusbFirstWrite(kFpci + 0x478), -1);
}

// tegra_xhci_exit_elpg after SC7: the partitions, the pad, then the host role's ID override (VBUS override off first),
// then tegra_xusb_config, then the saved FPCI and IPFS context, each register with the value it had before SC7.
TEST_F(XusbHostPower, AfterSc7IdOverrideBeforeConfigThenContext)
{
    bootState(kT210B01);
    host.setHostRole(true);
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    for (size_t i = 0; i < kContext.size(); ++i)
        xusbModel().registers[kContext[i]] = 0x1000u + static_cast<uint32_t>(i);
    xusbModel().registers[kVbusId] |= 1u << 14;  // a VBUS override some role left on
    host.saveContext();
    host.lostPower();

    const uint32_t vbusId = xusbModel().registers[kVbusId];
    xusbModelReset();
    bootState(kT210B01);
    xusbModel().registers[kVbusId] = vbusId;
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;

    const auto ids = xusbWritesTo(kVbusId);
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0] & (1u << 14), 0u);    // VBUS override off first
    EXPECT_EQ(ids[1] & (0xfu << 18), 0u);  // then ID grounded
    const long id = xusbFirstWrite(kVbusId);
    EXPECT_GT(id, toggleOf(22));
    EXPECT_LT(id, xusbFirstWrite(kIpfs + 0x180, 1));
    long last = xusbFirstWrite(kFpci + 0x404, 1u << 12);  // after tegra_xusb_disable_hsic_wake
    for (size_t i = 0; i < kContext.size(); ++i) {
        const auto writes = xusbWritesTo(kContext[i]);
        ASSERT_FALSE(writes.empty()) << std::hex << kContext[i];
        EXPECT_EQ(writes.back(), 0x1000u + i) << std::hex << kContext[i];
        long at = -1;
        for (size_t e = 0; e < xusbModel().log.size(); ++e) {
            const XusbEvent &ev = xusbModel().log[e];
            if (ev.kind == XusbEvent::Write && ev.address == kContext[i] && ev.value == 0x1000u + i)
                at = static_cast<long>(e);
        }
        EXPECT_GT(at, last) << std::hex << kContext[i];
        last = at;
    }
}

// A device role at SC7 gets no ID override back from the host's bring-up.
TEST_F(XusbHostPower, AfterSc7DeviceRoleKeepsItsId)
{
    bootState(kT210B01);
    host.setHostRole(false);
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    host.saveContext();
    host.lostPower();
    xusbModelReset();
    bootState(kT210B01);
    ASSERT_TRUE(host.powerOn(why, sizeof(why))) << why;
    EXPECT_TRUE(xusbWritesTo(kVbusId).empty());
}
}  // namespace

extern "C" void tegra_mc_hot_reset(uint32_t ctrl, uint32_t bit, bool on)
{
    xusbModel().log.push_back(xusbWrite(kMc + ctrl, on ? bit : ~bit));
}
