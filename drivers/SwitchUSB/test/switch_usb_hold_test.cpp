// AppleUSBXHCI's Save State and Restore State on Tegra's XUSB host through XusbPorts.h xusb_hold, as SwitchXHCI's
// regRead32, regWrite32 and regWrite64 apply it (switch_usb_host_source_test checks they do).
//
// AppleUSBXHCI, as disassembled from 24B119's kernelcache (21H461 inlines the save and the restore into
// lowerOnePowerStateTo 0xfffffff005ec9b64 and raiseOnePowerStateTo 0xfffffff005ecac88, through the same registers):
//   * lowerOnePowerStateTo(1) 0xfffffff009d41eac clears USBCMD.R/S, waits for HCH and calls saveState 0xfffffff009d45090:
//     USBCMD |= CSS, SSS waited out, then SRE (or SSS still set) is hardware exception 4, "save state failed", and
//     kIOReturnDeviceError, which AppleUSBHostController::setPowerStateGated logs as "going to state 1 returned";
//     success sets the saved flag;
//   * raiseOnePowerStateTo(2) 0xfffffff009d42d9c with the flag calls restoreState 0xfffffff009d4590c: USBCMD, DNCTRL,
//     DCBAAP and CONFIG back, each interrupter's IMOD, ERSTSZ, ERDP and ERSTBA (0xfffffff009d49f50), USBCMD |= CRS, RSS
//     waited out, SRE is exception 4; without the flag it calls reset 0xfffffff009d44754 (HCRST);
//   * lowerOnePowerStateTo(0) discards every device and clears the flag; exception 4's recovery takes the controller to
//     Off and back up, through reset.
// The model Tegra host does what boot 1109-2145 showed (CSS answered with SRE) and what xHCI 1.2 says of the rest: a
// halted controller keeps its state while powered, ERSTBA written rewinds the event ring's enqueue pointer, and SC7
// leaves a controller with nothing in it.
#include <gtest/gtest.h>

#include <cstdint>

#include "switch_usb_host_harness.h"

namespace {

constexpr uint32_t kUsbcmd = 0x80, kUsbsts = 0x84, kRuntime = 0x600;
constexpr uint32_t kRs = 1u << 0, kHcrst = 1u << 1, kCss = 1u << 8, kCrs = 1u << 9;
constexpr uint32_t kHch = 1u << 0, kSss = 1u << 8, kRss = 1u << 9, kSre = 1u << 10;
constexpr uint64_t kRing = 0x80001000;
constexpr int kNoException = 0, kSaveStateFailed = 4;

struct Tegra {
    uint32_t usbcmd = 0, usbsts = kHch;
    uint64_t erstba = 0;
    unsigned enqueue = 0, slots = 0;
    bool sawCss = false, sawCrs = false;

    uint32_t erstbaAt() const
    {
        return kRuntime + hold_erstba0();
    }

    uint32_t read32(uint32_t at) const
    {
        if (at == kUsbcmd)
            return usbcmd;
        if (at == kUsbsts)
            return usbsts;
        if (at == erstbaAt())
            return static_cast<uint32_t>(erstba);
        if (at == erstbaAt() + 4)
            return static_cast<uint32_t>(erstba >> 32);
        return 0;
    }

    void write32(uint32_t at, uint32_t value)
    {
        if (at == kUsbsts) {
            usbsts &= ~(value & kSre);
        } else if (at == kUsbcmd) {
            if (value & kHcrst) {
                *this = Tegra{};
                return;
            }
            sawCss = sawCss || ((value & kCss) != 0u);
            sawCrs = sawCrs || ((value & kCrs) != 0u);
            if (value & kCss)
                usbsts |= kSre;
            usbcmd = value & ~(kCss | kCrs);
            usbsts = (value & kRs) ? (usbsts & ~kHch) : (usbsts | kHch);
        } else if (at == erstbaAt() || at == erstbaAt() + 4) {
            const unsigned shift = at == erstbaAt() ? 0 : 32;
            erstba = (erstba & ~(0xffffffffULL << shift)) | (static_cast<uint64_t>(value) << shift);
            enqueue = 0;
        }
    }

    void write64(uint32_t at, uint64_t value)
    {
        if (at == erstbaAt()) {
            erstba = value;
            enqueue = 0;
        }
    }

    void sc7()
    {
        *this = Tegra{};
    }
};

// SwitchXHCI's register overrides around the model host.
struct Switch {
    Tegra &hw;

    uint32_t read32(uint32_t at) const
    {
        const uint32_t value = hw.read32(at);
        return at == kUsbsts ? hold_usbsts(value) : value;
    }

    void write32(uint32_t at, uint32_t value)
    {
        if (at == kUsbsts)
            hold_usbsts_write(value);
        if (at == kUsbcmd)
            value = hold_usbcmd(value);
        if ((at == hw.erstbaAt() || at == hw.erstbaAt() + 4) && (hold_keeps(value, hw.read32(at)) != 0))
            return;
        hw.write32(at, value);
    }

    void write64(uint32_t at, uint64_t value)
    {
        if (at == hw.erstbaAt() && (hold_keeps(value, hw.erstba) != 0))
            return;
        hw.write64(at, value);
    }
};

template <typename Io> struct Apple {
    Io &io;
    bool saved = false;
    int exception = kNoException;
    unsigned devices = 0;
    uint32_t savedUsbcmd = 0;

    // lowerOnePowerStateTo(1): kIOReturnSuccess (0) or kIOReturnDeviceError (1 here)
    int sleep()
    {
        io.write32(kUsbcmd, io.read32(kUsbcmd) & ~kRs);
        if (!(io.read32(kUsbsts) & kHch))
            return 1;
        savedUsbcmd = io.read32(kUsbcmd);
        io.write32(kUsbcmd, io.read32(kUsbcmd) | kCss);
        const uint32_t status = io.read32(kUsbsts);
        if ((status & kSre) || (status & kSss)) {
            exception = kSaveStateFailed;
            return 1;
        }
        saved = true;
        return 0;
    }

    void off()
    {
        devices = 0;
        saved = false;
    }

    void reset()
    {
        io.write32(kUsbcmd, kHcrst);
        io.write64(kRuntime + hold_erstba0(), kRing);
    }

    // raiseOnePowerStateTo(2) and the run that follows
    int wake()
    {
        if (saved) {
            io.write32(kUsbcmd, savedUsbcmd);
            io.write64(kRuntime + hold_erstba0(), kRing);
            io.write32(kUsbcmd, io.read32(kUsbcmd) | kCrs);
            const uint32_t status = io.read32(kUsbsts);
            if ((status & kSre) || (status & kRss)) {
                exception = kSaveStateFailed;
                return 1;
            }
        } else {
            reset();
        }
        io.write32(kUsbcmd, io.read32(kUsbcmd) | kRs);
        return 0;
    }

    // exception 4's recovery: Off, then up through reset
    void recover()
    {
        io.write32(kUsbsts, io.read32(kUsbsts) & kSre);
        off();
        exception = kNoException;
        (void)wake();
    }
};

class SwitchUsbHold : public testing::Test {
protected:
    void SetUp() override
    {
        hold_reset();
        apple.reset();
        tegra.slots = 1;
        tegra.enqueue = 5;
        apple.devices = 1;
        apple.io.write32(kUsbcmd, kRs);
    }

    Tegra tegra;
    Switch io{tegra};
    Apple<Switch> apple{io};
};

}  // namespace

TEST(SwitchUsbHoldBare, TegraAnswersSaveStateWithSre)
{
    Tegra tegra;
    Apple<Tegra> apple{tegra};
    apple.reset();
    tegra.write32(kUsbcmd, kRs);
    EXPECT_EQ(apple.sleep(), 1);
    EXPECT_EQ(apple.exception, kSaveStateFailed);
    EXPECT_FALSE(apple.saved);
    EXPECT_EQ(tegra.usbsts & (kHch | kSre), kHch | kSre);  // boot 1109-2145's +1 s line: USBSTS 0x409
}

TEST_F(SwitchUsbHold, IdleSleepAndWakeKeepTheControllersState)
{
    for (int cycle = 0; cycle < 3; ++cycle) {
        EXPECT_EQ(apple.sleep(), 0);
        EXPECT_EQ(apple.exception, kNoException);
        EXPECT_TRUE(apple.saved);
        EXPECT_EQ(apple.wake(), 0);
        EXPECT_EQ(apple.exception, kNoException);
    }
    EXPECT_FALSE(tegra.sawCss);
    EXPECT_FALSE(tegra.sawCrs);
    EXPECT_EQ(tegra.usbsts & (kHch | kSre), 0u);
    EXPECT_EQ(tegra.slots, 1u);
    EXPECT_EQ(tegra.enqueue, 5u) << "the restore's ERSTBA rewrite would rewind the event ring under AppleUSBXHCI";
    EXPECT_EQ(tegra.erstba, kRing);
}

TEST_F(SwitchUsbHold, ThirtyTwoBitHalvesOfErstbaAreKeptToo)
{
    ASSERT_EQ(apple.sleep(), 0);
    io.write32(tegra.erstbaAt(), static_cast<uint32_t>(kRing));
    io.write32(tegra.erstbaAt() + 4, static_cast<uint32_t>(kRing >> 32));
    EXPECT_EQ(tegra.enqueue, 5u);
}

TEST_F(SwitchUsbHold, OffAndBackGoesThroughReset)
{
    ASSERT_EQ(apple.sleep(), 0);
    apple.off();
    EXPECT_EQ(apple.wake(), 0);
    EXPECT_EQ(apple.exception, kNoException);
    EXPECT_EQ(tegra.slots, 0u);
    EXPECT_EQ(tegra.enqueue, 0u) << "after HCRST the ring is AppleUSBXHCI's to set up again";
    EXPECT_EQ(tegra.erstba, kRing);
}

TEST_F(SwitchUsbHold, Sc7RestoreReportsSreAndRecoveryResets)
{
    ASSERT_EQ(apple.sleep(), 0);
    tegra.sc7();
    hold_lost();
    EXPECT_EQ(apple.wake(), 1);
    EXPECT_EQ(apple.exception, kSaveStateFailed);
    EXPECT_EQ(tegra.erstba, kRing) << "a host that lost its state gets the restore's ERSTBA";
    EXPECT_FALSE(tegra.sawCrs);
    apple.recover();
    EXPECT_EQ(apple.exception, kNoException);
    EXPECT_EQ(io.read32(kUsbsts) & kSre, 0u);
    EXPECT_EQ(apple.sleep(), 0);
    EXPECT_EQ(apple.wake(), 0);
    EXPECT_EQ(apple.exception, kNoException);
}

TEST_F(SwitchUsbHold, Sc7AfterOffNeedsNothing)
{
    ASSERT_EQ(apple.sleep(), 0);
    apple.off();
    tegra.sc7();
    hold_lost();
    EXPECT_EQ(apple.wake(), 0);
    EXPECT_EQ(apple.exception, kNoException);
}

TEST_F(SwitchUsbHold, RestoreWithNothingHeldFails)
{
    io.write32(kUsbcmd, io.read32(kUsbcmd) & ~kRs);
    io.write32(kUsbcmd, io.read32(kUsbcmd) | kCrs);
    EXPECT_NE(io.read32(kUsbsts) & kSre, 0u);
    io.write32(kUsbsts, kSre);
    EXPECT_EQ(io.read32(kUsbsts) & kSre, 0u);
}

TEST_F(SwitchUsbHold, HcrstEndsTheHold)
{
    ASSERT_EQ(apple.sleep(), 0);
    hold_lost();
    io.write32(kUsbcmd, kHcrst);
    EXPECT_EQ(hold_keeps(kRing, kRing), 0);
    EXPECT_EQ(hold_usbcmd(kCss) & (kCss | kCrs), 0u);
    EXPECT_EQ(hold_keeps(kRing, kRing), 1);
    EXPECT_EQ(hold_keeps(kRing, 0), 0);
}
