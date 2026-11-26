// XusbLanes.h on the host: when SS port 0's lane is up across DisplayPort alt mode's decisions, the role and PCIe's PLL
// calibrations, what bringing it up and down writes (against the Linux sequences it follows, and never a bit of USB2
// pad 0, its port or another lane), what the USB-C port's transports do as the lane and the role change (SC7
// included), the falcon's SuperSpeed mailbox messages, the SMMU state the host's bring-up accepts, and which
// AppleUSBXHCI writes SwitchXHCI adjusts.
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <vector>

#include "../XusbLanes.h"
#include "../XusbPorts.h"

namespace {

using Regs = std::map<uint32_t, uint32_t>;

// What XusbSuperSpeed.cpp writes to bring the lane up and down, applied to a register model.
void laneUp(Regs &r, bool b01 = true, std::vector<uint32_t> *elpgTrace = nullptr)
{
    const unsigned lane = xusb_ss_lane(b01);
    if (b01) {
        for (const auto &write : xusb_b01_usb3_lane_data)
            r[XUSB_PADCTL_UPHY_MISC_CTL8(lane)] = xusb_uphy_cfg(write[0], write[1]);
    }
    r[XUSB_PADCTL_USB3_PAD_MUX] = xusb_pad_mux_ss(r[XUSB_PADCTL_USB3_PAD_MUX], b01);
    r[XUSB_PADCTL_SS_PORT_MAP] = xusb_ss_port_map(r[XUSB_PADCTL_SS_PORT_MAP]);
    r[XUSB_PADCTL_USB3_ECTL(0, 1)] = xusb_ectl1(r[XUSB_PADCTL_USB3_ECTL(0, 1)], b01);
    r[XUSB_PADCTL_USB3_ECTL(0, 2)] = xusb_ectl2(r[XUSB_PADCTL_USB3_ECTL(0, 2)]);
    r[XUSB_PADCTL_USB3_ECTL(0, 3)] = XUSB_ECTL3;
    r[XUSB_PADCTL_USB3_ECTL(0, 4)] = xusb_ectl4(r[XUSB_PADCTL_USB3_ECTL(0, 4)]);
    r[XUSB_PADCTL_USB3_ECTL(0, 6)] = XUSB_ECTL6;
    for (const auto &step : xusb_ss_up_steps) {
        r[XUSB_PADCTL_ELPG_PROGRAM_1] = xusb_elpg_apply(r[XUSB_PADCTL_ELPG_PROGRAM_1], step);
        if (elpgTrace)
            elpgTrace->push_back(r[XUSB_PADCTL_ELPG_PROGRAM_1]);
    }
}

void laneDown(Regs &r, std::vector<uint32_t> *elpgTrace = nullptr)
{
    for (const auto &step : xusb_ss_down_steps) {
        r[XUSB_PADCTL_ELPG_PROGRAM_1] = xusb_elpg_apply(r[XUSB_PADCTL_ELPG_PROGRAM_1], step);
        if (elpgTrace)
            elpgTrace->push_back(r[XUSB_PADCTL_ELPG_PROGRAM_1]);
    }
    r[XUSB_PADCTL_SS_PORT_MAP] = xusb_ss_port_unmap(r[XUSB_PADCTL_SS_PORT_MAP]);
}

// Reset-like values with every bit outside SS port 0 and the SoC's SS lane set, so a stray write shows.
Regs boardState(bool b01 = true)
{
    const unsigned lane = xusb_ss_lane(b01);
    return {
        {XUSB_PADCTL_USB3_PAD_MUX, 0xffffffffu & ~(3u << XUSB_PAD_MUX_SHIFT(lane)) & ~XUSB_PAD_MUX_IDDQ_DISABLE(lane)},
        {XUSB_PADCTL_SS_PORT_MAP, 0x7777777u},
        {XUSB_PADCTL_ELPG_PROGRAM_1, 0xffffffffu},
        {XUSB_USB2_VBUS_ID, 0x12345678u},
        {XUSB_USB2_PORT_CAP, 0x3u},
        {XUSB_CHRG_OTGPAD0_CTL0, 0xabcdu},
        {XUSB_PADCTL_UPHY_MISC_CTL8(0), 0x55u},
        {XUSB_PADCTL_UPHY_MISC_CTL1(0), 0x66u},
    };
}

TEST(SwitchUsbLanes, LaneForEitherConnectedDataRoleWhosePairIsFree)
{
    for (const bool connected : {false, true}) {
        for (const auto lanes : {SWITCH_USB3_LANES_PENDING, SWITCH_USB3_LANES_FREE, SWITCH_USB3_LANES_TAKEN}) {
            for (const bool uphy : {false, true})
                EXPECT_EQ(xusb_ss_lane_wanted(connected, lanes, uphy),
                          connected && uphy && lanes == SWITCH_USB3_LANES_FREE);
        }
    }
}

// The lane and the USB-C port's transports across one session, as SwitchUSB.cpp's role thread, PCIe's thread
// (SwitchUPHY.h) and SC7 move them: the lane follows xusb_ss_lane_wanted under the UPHY announcement, each transport
// follows xusb_transport_decide on its own, and the falcon's SuperSpeed interface reset is owed from each host role's start
// and done (by SwitchUSB.cpp's applyLanes) with the USB 3 transport out. The model's device is a USB 3 stick: it links at
// SuperSpeed while the USB 3 transport is the host's, else on its USB 2.0 pair while the USB 2.0 one is.
struct Session {
    bool host = false, plugged = false, uphyReady = false, laneUp = false;
    xusb_port_role role = XUSB_PORT_EMPTY;  // the USB 2.0 transport's
    xusb_port_role usb3Role = XUSB_PORT_EMPTY;
    bool usb3 = false, superSpeed = false, usb2Connected = false, owed = false;
    unsigned resets = 0, usb2Changes = 0;
    unsigned controllerResetsDuringReset = 0;  // each owes the reset again from inside its power-off window
    switch_usb3_lanes lanes = SWITCH_USB3_LANES_FREE;

    void follow()
    {
        laneUp = xusb_ss_lane_wanted(host, lanes, uphyReady);  // switch_usb_ss_lane: up only while the PLL is ready
        xusb_port_role wanted = plugged ? XUSB_PORT_DEVICE : XUSB_PORT_EMPTY;
        if (host)
            wanted = XUSB_PORT_HOST;
        owed = owed || (wanted == XUSB_PORT_HOST && role != XUSB_PORT_HOST);
        if (wanted == XUSB_PORT_HOST && owed) {
            take(wanted, false);
            EXPECT_FALSE(usb3) << "the USB 3 transport is out while the reset is owed";
            ++resets;
            owed = controllerResetsDuringReset > 0;
            if (owed)
                --controllerResetsDuringReset;
        }
        take(wanted, !owed);
    }

    static xusb_port_role after(xusb_transport_step step, xusb_port_role live, xusb_port_role wanted)
    {
        if (step == XUSB_TRANSPORT_KEEP)
            return live;
        return step == XUSB_TRANSPORT_REMOVE ? XUSB_PORT_EMPTY : wanted;
    }

    void take(xusb_port_role wanted, bool reset)
    {
        const xusb_transport_step usb2Step = xusb_transport_decide(role, wanted != XUSB_PORT_EMPTY, wanted);
        const xusb_transport_step usb3Step =
            xusb_transport_decide(usb3Role, xusb_usb3_transport_wanted(wanted, laneUp, reset), wanted);
        usb2Changes += usb2Step != XUSB_TRANSPORT_KEEP ? 1 : 0;
        role = after(usb2Step, role, wanted);
        usb3Role = after(usb3Step, usb3Role, wanted);
        usb3 = usb3Role != XUSB_PORT_EMPTY;
        EXPECT_FALSE(usb3 && !laneUp) << "a USB 3 transport without the lane";
        superSpeed = usb3Role == XUSB_PORT_HOST;
        usb2Connected = role == XUSB_PORT_HOST && !superSpeed;
    }

    // The host role starts: the cable is in, the pad goes to the host, then the lane and the transports.
    void hostRole()
    {
        plugged = true;
        host = true;
        follow();
    }

    void deviceRole()
    {
        host = false;
        follow();
    }

    // PCIe's calibration: "not ready" clamps the lane on PCIe's thread before it resets the UPHY.
    void calibrate()
    {
        uphyReady = false;
        follow();
        uphyReady = true;
        follow();
    }

    // SC7: the PLL and the lane are lost (no write, no listener); PCIe calibrates later on its own worker.
    void sleepAndWake()
    {
        uphyReady = false;
        follow();
        calibrate();
    }
};

TEST(SwitchUsbLanes, StickSessionLinksAtSuperSpeed)
{
    Session s;
    s.uphyReady = true;
    s.hostRole();
    EXPECT_TRUE(s.laneUp && s.usb3 && s.superSpeed);
    EXPECT_EQ(s.resets, 1u) << "RESET_SSPI once, before the USB 3 transport";
    s.deviceRole();
    EXPECT_FALSE(s.laneUp || s.usb3 || s.superSpeed);
    EXPECT_EQ(s.role, XUSB_PORT_DEVICE);
    s.plugged = false;
    s.follow();
    EXPECT_EQ(s.role, XUSB_PORT_EMPTY);
    s.plugged = true;
    s.hostRole();
    EXPECT_EQ(s.resets, 2u) << "every host role resets it again";
}

// At boot the host role can come before PCIe has calibrated the PLL: USB 2.0 until "ready", then the USB 3 transport
// joins without taking the cable from the ports.
TEST(SwitchUsbLanes, LaneWaitsForPcie)
{
    Session s;
    s.hostRole();
    EXPECT_FALSE(s.laneUp || s.usb3 || s.superSpeed);
    EXPECT_TRUE(s.usb2Connected);
    s.calibrate();
    EXPECT_TRUE(s.laneUp && s.usb3);
    EXPECT_EQ(s.resets, 1u) << "the host role was already on";
}

TEST(SwitchUsbLanes, Sc7TakesTheUsb3TransportAndGivesItBack)
{
    Session s;
    s.uphyReady = true;
    s.hostRole();
    for (int wake = 0; wake < 3; ++wake) {
        s.sleepAndWake();
        EXPECT_TRUE(s.laneUp && s.usb3) << wake;
    }
}

// The controller reset while the role stays host (it went Off and came back): the USB 3 transport comes out before the
// reset is done again and goes back after.
TEST(SwitchUsbLanes, ControllerResetRedoesTheReset)
{
    Session s;
    s.uphyReady = true;
    s.hostRole();
    s.owed = true;
    s.follow();
    EXPECT_EQ(s.resets, 2u);
    EXPECT_TRUE(s.usb3);
}

// AppleUSBXHCI's reset right after its first run can land inside the SuperSpeed power-off window (hub run: PORTSC 1
// left 0x202a0). The reset is owed again, so the USB 3 transport stays out until the run that reset brings has redone it.
TEST(SwitchUsbLanes, ControllerResetDuringTheResetOwesItAgain)
{
    Session s;
    s.uphyReady = true;
    s.controllerResetsDuringReset = 1;
    s.hostRole();
    EXPECT_EQ(s.resets, 1u);
    EXPECT_FALSE(s.usb3) << "no USB 3 transport while the reset is owed";
    s.follow();
    EXPECT_EQ(s.resets, 2u);
    EXPECT_TRUE(s.usb3);
}

// WLAN's retry after a failed post-wake bring-up recalibrates under a live lane: the lane is clamped first, the USB 3
// transport goes, and comes back once the PLL is ready again.
TEST(SwitchUsbLanes, RecalibrationUnderALiveLane)
{
    Session s;
    s.uphyReady = true;
    s.hostRole();
    s.calibrate();
    EXPECT_TRUE(s.laneUp && s.usb3);
}

// Pin C: the pairs are DisplayPort's, no USB 3 transport; pin D or no DisplayPort: SuperSpeed.
TEST(SwitchUsbLanes, PinAssignmentDecidesTheUsb3Transport)
{
    Session c;
    c.uphyReady = true;
    c.lanes = SWITCH_USB3_LANES_PENDING;
    c.hostRole();
    c.lanes = SWITCH_USB3_LANES_TAKEN;
    c.follow();
    EXPECT_FALSE(c.laneUp || c.usb3);
    Session d;
    d.uphyReady = true;
    d.lanes = SWITCH_USB3_LANES_PENDING;
    d.hostRole();
    d.lanes = SWITCH_USB3_LANES_FREE;
    d.follow();
    EXPECT_TRUE(d.laneUp && d.usb3);
}

// Each type on its own: added when wanted and absent, removed when present and unwanted, swapped only when its data role
// changes, and kept otherwise; the USB 3 one is wanted with the lane and, in the host role, the reset taken.
TEST(SwitchUsbLanes, TransportStepTable)
{
    for (const auto live : {XUSB_PORT_EMPTY, XUSB_PORT_DEVICE, XUSB_PORT_HOST}) {
        for (const bool wanted : {false, true}) {
            for (const auto to : {XUSB_PORT_DEVICE, XUSB_PORT_HOST}) {
                const xusb_transport_step step = xusb_transport_decide(live, wanted, to);
                if (live == XUSB_PORT_EMPTY) {
                    EXPECT_EQ(step, wanted ? XUSB_TRANSPORT_ADD : XUSB_TRANSPORT_KEEP);
                } else if (!wanted) {
                    EXPECT_EQ(step, XUSB_TRANSPORT_REMOVE);
                } else {
                    EXPECT_EQ(step, live == to ? XUSB_TRANSPORT_KEEP : XUSB_TRANSPORT_SWAP);
                }
            }
        }
    }
    for (const auto to : {XUSB_PORT_EMPTY, XUSB_PORT_DEVICE, XUSB_PORT_HOST}) {
        for (const bool lane : {false, true}) {
            for (const bool reset : {false, true}) {
                EXPECT_EQ(xusb_usb3_transport_wanted(to, lane, reset),
                          lane && to != XUSB_PORT_EMPTY && (to != XUSB_PORT_HOST || reset));
            }
        }
    }
}

// The SuperSpeed side never touches the USB 2.0 transport: one addition per host role, whatever the resets and lane do.
TEST(SwitchUsbLanes, Usb2TransportStaysFromAttachToDetach)
{
    Session s;
    s.uphyReady = true;
    s.controllerResetsDuringReset = 1;
    s.hostRole();
    s.follow();
    s.owed = true;
    s.follow();
    s.calibrate();
    s.sleepAndWake();
    EXPECT_EQ(s.usb2Changes, 1u);
    EXPECT_EQ(s.role, XUSB_PORT_HOST);
    EXPECT_TRUE(s.usb3);
}

// The reason development builds log with a transport's change: the role's first, then the lane, then the reset.
TEST(SwitchUsbLanes, TransportChangeReasons)
{
    EXPECT_STREQ(xusb_port_change_reason(XUSB_PORT_EMPTY, XUSB_PORT_HOST, true, false, false), "attach");
    EXPECT_STREQ(xusb_port_change_reason(XUSB_PORT_HOST, XUSB_PORT_EMPTY, false, true, true), "detach");
    EXPECT_STREQ(xusb_port_change_reason(XUSB_PORT_HOST, XUSB_PORT_DEVICE, false, true, true), "role change");
    EXPECT_STREQ(xusb_port_change_reason(XUSB_PORT_HOST, XUSB_PORT_HOST, true, false, true), "lane up");
    EXPECT_STREQ(xusb_port_change_reason(XUSB_PORT_HOST, XUSB_PORT_HOST, false, true, true), "lane down");
    EXPECT_STREQ(xusb_port_change_reason(XUSB_PORT_HOST, XUSB_PORT_HOST, true, true, false), "SuperSpeed reset owed");
    EXPECT_STREQ(xusb_port_change_reason(XUSB_PORT_HOST, XUSB_PORT_HOST, true, true, true), "SuperSpeed reset taken");
    EXPECT_STREQ(xusb_port_role_name(XUSB_PORT_EMPTY), "none");
    EXPECT_STREQ(xusb_port_role_name(XUSB_PORT_DEVICE), "device");
    EXPECT_STREQ(xusb_port_role_name(XUSB_PORT_HOST), "host");
}

// Tegra XUSB: USB 3 ports 1-4 first in the Supported Protocol capabilities; SS port 0 is xHCI port 1.
TEST(SwitchUsbLanes, SuperSpeedRootPortNumber)
{
    EXPECT_EQ(xusb_ss_xhci_port(1, 4), 1u);
    EXPECT_EQ(xusb_ss_xhci_port(1, 0), 0u) << "no USB 3 capability";
    EXPECT_EQ(XUSB_MBOX_RESET_SSPI, 16u);
    EXPECT_EQ(XUSB_MBOX_RESET_SSPI_DATA, 1u) << "xhci-tegra.c usb3_otg_port_base_1";
}

// The host's bring-up after SC7 finds XUSB_HOST translated by its own ASID (TegraSMMU restored it): accepted. Bypass is
// accepted; any other ASID is not.
TEST(SwitchUsbLanes, HostAsidAsTheBringUpFindsIt)
{
    constexpr uint32_t kOurs = 124;
    EXPECT_TRUE(xusb_host_asid_usable(0, kOurs));
    EXPECT_TRUE(xusb_host_asid_usable(kOurs, kOurs)) << "an ASID without ENABLE is bypass";
    EXPECT_TRUE(xusb_host_asid_usable(0x80000000u | kOurs, kOurs));
    EXPECT_FALSE(xusb_host_asid_usable(0x80000000u | 125u, kOurs));
    EXPECT_FALSE(xusb_host_asid_usable(0x80000000u, kOurs));
}

// r24-dpalt's sequencer as agreed: PENDING at attach, FREE for pin D (or no DisplayPort), TAKEN for pin C, hub enable as
// a second call, FREE+false at exit or detach. The lane is never up while the pairs are undecided or DisplayPort's.
TEST(SwitchUsbLanes, DockSessions)
{
    struct Step {
        bool host;
        switch_usb3_lanes lanes;
        bool up;
    };

    const std::vector<std::vector<Step>> sessions = {
        // Pin D: USB2 host first (the DR swap), SuperSpeed once DP Configure is acknowledged, kept with the hub on.
        {{false, SWITCH_USB3_LANES_PENDING, false},
         {true, SWITCH_USB3_LANES_PENDING, false},
         {true, SWITCH_USB3_LANES_FREE, true},
         {true, SWITCH_USB3_LANES_FREE, true},
         {false, SWITCH_USB3_LANES_FREE, false}},
        // Pin C: USB2 only for the whole session.
        {{false, SWITCH_USB3_LANES_PENDING, false},
         {true, SWITCH_USB3_LANES_PENDING, false},
         {true, SWITCH_USB3_LANES_TAKEN, false},
         {true, SWITCH_USB3_LANES_FREE, true}},
        // An OTG adapter: no alt mode engine, the pairs stay free.
        {{true, SWITCH_USB3_LANES_FREE, true}, {false, SWITCH_USB3_LANES_FREE, false}},
    };
    for (const auto &session : sessions) {
        Regs regs = boardState();
        bool up = false;
        for (const Step &step : session) {
            const bool want = xusb_ss_lane_wanted(step.host, step.lanes, true);
            if (want && !up)
                laneUp(regs);
            if (!want && up)
                laneDown(regs);
            up = want;
            EXPECT_EQ(up, step.up);
            EXPECT_EQ(xusb_ss_clamped(regs[XUSB_PADCTL_ELPG_PROGRAM_1]), !up);
        }
    }
}

TEST(SwitchUsbLanes, UpAndDownTouchOnlyThePortsOwnBits)
{
    for (const bool b01 : {true, false}) {
        SCOPED_TRACE(b01 ? "T210B01 pcie-5" : "T210 Erista pcie-6");
        const unsigned lane = xusb_ss_lane(b01);
        const uint32_t muxMask = (3u << XUSB_PAD_MUX_SHIFT(lane)) | XUSB_PAD_MUX_IDDQ_DISABLE(lane);
        const Regs before = boardState(b01);
        Regs regs = before;
        laneUp(regs, b01);
        EXPECT_EQ(regs[XUSB_PADCTL_USB3_PAD_MUX] & ~muxMask, before.at(XUSB_PADCTL_USB3_PAD_MUX));
        EXPECT_EQ((regs[XUSB_PADCTL_USB3_PAD_MUX] >> XUSB_PAD_MUX_SHIFT(lane)) & 3u, 1u) << "lane carries xusb";
        EXPECT_TRUE(regs[XUSB_PADCTL_USB3_PAD_MUX] & XUSB_PAD_MUX_IDDQ_DISABLE(lane)) << "lane out of IDDQ";
        EXPECT_EQ(regs[XUSB_PADCTL_SS_PORT_MAP] & ~7u, before.at(XUSB_PADCTL_SS_PORT_MAP) & ~7u);
        EXPECT_EQ(regs[XUSB_PADCTL_SS_PORT_MAP] & 7u, 0u) << "SS port 0 paired with USB2 port 0";
        EXPECT_EQ(regs[XUSB_PADCTL_ELPG_PROGRAM_1], 0xffffffffu & ~7u);
        for (const uint32_t untouched : {XUSB_USB2_VBUS_ID, XUSB_USB2_PORT_CAP, XUSB_CHRG_OTGPAD0_CTL0,
                                         XUSB_PADCTL_UPHY_MISC_CTL8(0), XUSB_PADCTL_UPHY_MISC_CTL1(0)})
            EXPECT_EQ(regs[untouched], before.at(untouched)) << std::hex << untouched;
        laneDown(regs);
        EXPECT_EQ(regs[XUSB_PADCTL_ELPG_PROGRAM_1], 0xffffffffu);
        EXPECT_EQ(regs[XUSB_PADCTL_SS_PORT_MAP], before.at(XUSB_PADCTL_SS_PORT_MAP))
            << "SS port 0 unmapped to disabled (7) on lane-down";
        EXPECT_EQ(regs[XUSB_USB2_VBUS_ID], before.at(XUSB_USB2_VBUS_ID));
    }
}

// tegra210_pcie_phy_power_on/off: VCORE_DOWN, CLAMP_EN_EARLY, CLAMP_EN cleared in that order; set back in reverse.
TEST(SwitchUsbLanes, ClampOrderAsLinux)
{
    Regs regs = {{XUSB_PADCTL_ELPG_PROGRAM_1, 7u}};
    std::vector<uint32_t> up;
    std::vector<uint32_t> down;
    laneUp(regs, true, &up);
    EXPECT_EQ(up, (std::vector<uint32_t>{3u, 1u, 0u}));
    laneDown(regs, &down);
    EXPECT_EQ(down, (std::vector<uint32_t>{2u, 3u, 7u}));
    EXPECT_GE(xusb_ss_up_steps[0].delay_us, 100u);
    EXPECT_GE(xusb_ss_up_steps[1].delay_us, 100u);
    EXPECT_GE(xusb_ss_down_steps[0].delay_us, 100u);
    EXPECT_GE(xusb_ss_down_steps[1].delay_us, 250u);
}

TEST(SwitchUsbLanes, PadSettingsAsLinuxAndProds)
{
    EXPECT_EQ(xusb_ss_lane(true), 5u);
    EXPECT_EQ(xusb_ss_lane(false), 6u);
    EXPECT_EQ(XUSB_PADCTL_UPHY_MISC_CTL8(5), 0x5bcu);
    EXPECT_EQ(XUSB_PADCTL_UPHY_MISC_CTL1(5), 0x5a0u);
    EXPECT_EQ(XUSB_PADCTL_UPHY_MISC_CTL8(6), 0x5fcu);
    EXPECT_EQ(XUSB_PADCTL_UPHY_MISC_CTL1(6), 0x5e0u);
    EXPECT_EQ(XUSB_PADCTL_USB3_ECTL(0, 1), 0xa60u);
    EXPECT_EQ(XUSB_PADCTL_USB3_ECTL(0, 6), 0xa74u);
    EXPECT_EQ(xusb_ectl1(0, true), 0x20027u);
    EXPECT_EQ(xusb_ectl1(0xffffffffu, true), 0xfffeffe7u);
    EXPECT_EQ(xusb_ectl1(0, false), 0x20000u);
    EXPECT_EQ(xusb_ectl1(0xffffffffu, false), 0xfffeffffu);
    EXPECT_EQ(xusb_ectl2(0xabcd1234u), 0xabcd00fcu);
    EXPECT_EQ(xusb_ectl4(0x1234abcdu), 0x01c7abcdu);
    ASSERT_EQ(XUSB_B01_USB3_LANE_WRITES, 11u);
    EXPECT_EQ(xusb_uphy_cfg(0x35, 0x2587), 0x09352587u);
    EXPECT_EQ(xusb_uphy_cfg(0x97, 0x0080), 0x09970080u);
}

TEST(SwitchUsbLanes, LfpsDetection)
{
    const uint32_t other = 0x0f00ff0fu & ~((3u << 20) | (1u << 18) | (1u << 13));
    EXPECT_EQ(xusb_lfps_detect(other, true), other);
    EXPECT_EQ(xusb_lfps_detect(other, false), other | (1u << 20) | (1u << 18) | (1u << 13));
    EXPECT_EQ(xusb_lfps_detect(xusb_lfps_detect(other, false), true), other);
}

TEST(SwitchUsbLanes, FalconSuperSpeedMessages)
{
    auto d = xusb_mbox_ss_decide(XUSB_MBOX_DISABLE_SS_LFPS_DETECTION, 1u << 1);
    EXPECT_TRUE(d.handled && d.lfps && !d.lfps_enable && d.lfps_settle);
    EXPECT_EQ(d.reply, XUSB_MBOX_ACK);
    EXPECT_EQ(d.data, 1u << 1);
    d = xusb_mbox_ss_decide(XUSB_MBOX_ENABLE_SS_LFPS_DETECTION, 1u << 1);
    EXPECT_TRUE(d.handled && d.lfps && d.lfps_enable && !d.lfps_settle);
    d = xusb_mbox_ss_decide(XUSB_MBOX_DISABLE_SS_LFPS_DETECTION, 1u << 2);
    EXPECT_TRUE(d.handled && !d.lfps) << "SS port 1 is not on this board";
    EXPECT_EQ(d.reply, XUSB_MBOX_ACK);
    d = xusb_mbox_ss_decide(XUSB_MBOX_SAVE_DFE_CTLE_CTX, 0);
    EXPECT_TRUE(d.handled);
    EXPECT_EQ(d.reply, XUSB_MBOX_NAK) << "T210 has no DFE/CTLE context save (xhci-tegra NAKs)";
    for (const uint32_t cmd : {2u, 3u, 4u, 5u, 6u, 12u, 13u})
        EXPECT_FALSE(xusb_mbox_ss_decide(cmd, 0).handled) << cmd;
}

// Tegra XUSB: runtime registers at 0x2000 here, one interrupter.
TEST(SwitchUsbLanes, WritesSwitchXhciAdjusts)
{
    const uint32_t rt = 0x2000;
    EXPECT_TRUE(xhci_is_imod(rt + 0x24, rt, 1));
    EXPECT_FALSE(xhci_is_imod(rt + 0x20, rt, 1)) << "IMAN";
    EXPECT_FALSE(xhci_is_imod(rt + 0x44, rt, 1)) << "an interrupter the hardware does not have";
    EXPECT_FALSE(xhci_is_imod(0x40 + 0x400 + 4, rt, 1)) << "a port's PORTPMSC";
    EXPECT_EQ(xhci_imod_floor(0), 160u);
    EXPECT_EQ(xhci_imod_floor(0x00050010u), 0x000500a0u);
    EXPECT_EQ(xhci_imod_floor(4000u), 4000u);
}

}  // namespace

// tegra210_xusb_padctl_init_usb3_port(3) on a T210B01: SS port 3 mapped to USB2 port 3, then VCORE_DOWN, CLAMP_EN_EARLY
// and CLAMP_EN cleared in that order, one write each, 100 us apart; SS port 0's map and clamps and the other ports
// untouched.
TEST(SwitchUsbLanes, UnusedPortThreeLeavesElpgAsL4T)
{
    const uint32_t map = 7u << 15 | 7u << 10 | 7u << 5;
    EXPECT_EQ(xusb_ss_unused_port_map(map), 3u << 15 | 7u << 10 | 7u << 5);
    uint32_t elpg = 0x7u << 9 | 0x7u << 0;
    std::vector<uint32_t> trace;
    for (const auto &step : xusb_ss_unused_port_steps) {
        elpg = xusb_elpg_apply(elpg, step);
        trace.push_back(elpg);
    }
    EXPECT_EQ(trace, (std::vector<uint32_t>{0x3u << 9 | 0x7u, 0x1u << 9 | 0x7u, 0x7u}));
    EXPECT_TRUE(xusb_ss_clamped(elpg));
    // 100 us apart, as the Tegra X1 TRM's ELPG and LP0 exits release an SS port (22.8.7.5).
    EXPECT_EQ(xusb_ss_unused_port_steps[0].delay_us, 100u);
    EXPECT_EQ(xusb_ss_unused_port_steps[1].delay_us, 100u);
    EXPECT_EQ(xusb_ss_unused_port_steps[2].delay_us, 0u);
}
