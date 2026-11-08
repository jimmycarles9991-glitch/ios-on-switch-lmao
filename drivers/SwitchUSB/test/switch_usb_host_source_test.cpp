// What switch_usb_host_test's headers cannot show about the kernel side of USB host mode:
//   * the interrupt routing SwitchXHCI's comment claims, against the OLED DT (as decompiled in L4T's nx-platform
//     sources: the xusb node's interrupts) and the driver sources: GIC SPI 39/40 level-high, the LIC enabled before the
//     distributor, IPFS programmed as Linux's tegra_xusb_config, one panic in checkFired and only for a proven line;
//   * the kernel files use the tested headers, in the order the model assumes (SwitchPD's enter, the pad's role before
//     the controller's first start).
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bazel/testing/source_text.h"

using switch_ios::test::Body;
using switch_ios::test::Count;
using switch_ios::test::SourceText;
using testing::HasSubstr;
using testing::Not;

namespace {

// Where `needle` starts in `text` (a failure and npos when it is not there).
size_t at(const std::string &text, const std::string &needle)
{
    const size_t where = text.find(needle);
    EXPECT_NE(where, std::string::npos) << "not found: " << needle;
    return where;
}

}  // namespace

// --- Interrupt routing -----------------------------------------------------------------------------------------------

TEST(SwitchUsbHostSource, DriverConstants)
{
    // DT <0 0x27 4>, <0 0x28 4>: SPI 39, 40
    EXPECT_THAT(SourceText("../XusbHost.h"), HasSubstr("kHostIntid = 32 + 39, kMailboxIntid = 32 + 40"));
    const std::string xhci = SourceText("../SwitchXHCI.cpp");
    EXPECT_THAT(xhci, HasSubstr("xhost::makeNub(xhost::kXhciPA, xhost::kXhciBytes, xhost::kHostIntid)"));
    // level, as IRQ_TYPE_LEVEL_HIGH
    EXPECT_THAT(SourceText("../SwitchXHCIServices.cpp"), HasSubstr("switch_interrupt_nub(intid, false)"));
    EXPECT_THAT(xhci, HasSubstr("switch_interrupt_nub(xhost::kMailboxIntid, false)"));
}

TEST(SwitchUsbHostSource, IpfsAsTegraXusbConfig)
{
    const std::string source = SourceText("../XusbHost.cpp");
    const std::string power = Body(source, "bool Controller::powerOn(");
    const std::vector<std::string> order = {
        "rmw32(ipfs, IPFS_CONFIGURATION, 0, IPFS_EN_FPCI)", "rmw32(fpci, XUSB_CFG_4, CFG_4_BASE_MASK",
        "rmw32(fpci, XUSB_CFG_1, 0, CFG_1_ENABLES)", "rmw32(ipfs, IPFS_INTR_MASK, 0, IPFS_IP_INT_MASK)",
        "write32(ipfs, IPFS_CLKGATE_HYSTERESIS, 0x80)"};
    for (size_t i = 0; i + 1 < order.size(); ++i)
        EXPECT_LT(at(power, order[i]), at(power, order[i + 1])) << order[i] << " before " << order[i + 1];
    EXPECT_THAT(source, HasSubstr("IPFS_CONFIGURATION = 0x180"));
    EXPECT_THAT(source, HasSubstr("IPFS_INTR_MASK = 0x188"));
    EXPECT_THAT(source, HasSubstr("IPFS_IP_INT_MASK = 1u << 16"));
    EXPECT_THAT(source, HasSubstr("IPFS_INTR_ENABLE = 0x198"));
    EXPECT_THAT(source, Not(HasSubstr("write32(ipfs, IPFS_INTR_ENABLE")));  // Linux never writes it
    EXPECT_THAT(source, Not(HasSubstr("rmw32(ipfs, IPFS_INTR_ENABLE")));
}

TEST(SwitchUsbHostSource, GicLevelSpiWithLic)
{
    const std::string gic = SourceText("../../ArmGIC/ArmGIC.cpp");
    const std::string init = Body(gic, "void ArmGIC::initVector(");
    EXPECT_THAT(init, HasSubstr("isEdge(intid) ? (read32(config) | edgeBit) : (read32(config) & ~edgeBit)"));
    EXPECT_THAT(init, HasSubstr("ICTLR_CPU_IEP_CLASS"));
    const std::string enable = Body(gic, "void ArmGIC::enableVector(");
    EXPECT_LT(at(enable, "ICTLR_CPU_IER_SET"), at(enable, "writeBit(GICD_ISENABLER, intid)"));
    EXPECT_THAT(gic, HasSubstr("uint32_t switch_interrupt_count(uint32_t intid)"));
}

TEST(SwitchUsbHostSource, NoPanicBeforeProof)
{
    const std::string check = Body(SourceText("../SwitchXHCIEvents.cpp"), "void SwitchXHCI::checkFired(");
    EXPECT_EQ(Count(check, "panic("), 1u);
    EXPECT_LT(at(check, "if (verdict == XUSB_IRQ_PANIC)"), at(check, "panic("));
    EXPECT_THAT(check, HasSubstr("switch_usb_host_failed()"));
}

// --- The kernel files use these headers, in the order the tests assume ----------------------------------------------

TEST(SwitchUsbHostSource, SwitchPdUsesThePolicy)
{
    const std::string source = SourceText("../SwitchPD.cpp");
    EXPECT_THAT(source, HasSubstr("#include \"SwitchPDPolicy.h\""));
    EXPECT_THAT(source, HasSubstr("switch_pd_decide("));
    EXPECT_THAT(source, HasSubstr("switch_pd_window_end("));
    const std::string enter = Body(source, "void SwitchPD::enter(");
    EXPECT_LT(at(enter, "if (!switches)\n        setSwitches(false)"), at(enter, "setRails(rails)"));
    EXPECT_LT(at(enter, "setRails(rails)"), at(enter, "if (switches)\n        setSwitches(true)"));
}

TEST(SwitchUsbHostSource, HostRoleUsesThePortHelpers)
{
    const std::string source = SourceText("../XusbHost.cpp");
    EXPECT_THAT(source, HasSubstr("#include \"XusbPorts.h\""));
    for (const char *name : {"xhci_find_ports(", "xhci_portsc_power(", "xusb_vbus_id_host(", "xusb_chrg_ctl1_host("})
        EXPECT_THAT(source, HasSubstr(name));
    const std::string apply = Body(SourceText("../SwitchUSB.cpp"), "void applyRole(");
    // The pad goes to the host before the controller first starts.
    EXPECT_LT(at(apply, "switchxhci_set_host_role(true)"), at(apply, "switchxhci_start("));
}

// AppleUSBXHCI powers a root port only for a cable (an iPad's port controller reports it): the host role reports it
// once the controller has started, and the device role takes it back before the pad leaves the host.
TEST(SwitchUsbHostSource, TheCableFollowsTheRole)
{
    const std::string apply = Body(SourceText("../SwitchUSB.cpp"), "void applyRole(");
    const size_t start = at(apply, "switchxhci_start(");
    EXPECT_NE(apply.find("switchxhci_cable(true)", start), std::string::npos);
    const size_t device = at(apply, "} else if (!wantHost && gHostRole) {");
    EXPECT_LT(apply.find("switchxhci_cable(false)", device), apply.find("switchxhci_set_host_role(false)", device));
    EXPECT_THAT(SourceText("../SwitchXHCIServices.cpp"), HasSubstr("\"UsbForcePower\""));
}

// No sleep hold: the Tegra side goes down with SC7 and comes back before IOKit raises AppleUSBXHCI.
TEST(SwitchUsbHostSource, HostFollowsSc7)
{
    const std::string usb = SourceText("../SwitchUSB.cpp");
    EXPECT_THAT(usb, Not(HasSubstr("createPMAssertion")));
    EXPECT_THAT(Body(usb, "void switch_usb_suspend("), HasSubstr("switchxhci_sleep(true)"));
    EXPECT_THAT(Body(usb, "void switch_usb_resume("), HasSubstr("switchxhci_sleep(false)"));
    const std::string sleep = Body(SourceText("../SwitchXHCIEvents.cpp"), "void switchxhci_sleep(");
    EXPECT_LT(at(sleep, "gHost.powerOn("), at(sleep, "gHost.loadFirmware("));
    EXPECT_LT(at(sleep, "gHost.loadFirmware("), at(sleep, "xhost::MBOX_MSG_ENABLED"));
}

// SuperSpeed's lane and PCIe's UPHY PLL (kits/SwitchCAR/SwitchUPHY.h): PCIe announces "not ready" before powerUp holds
// the UPHY in reset and "ready" only once its PLL has locked; SwitchUSB clamps on "not ready", reads the announcement
// under its lane lock, marks the PLL lost before the lane at suspend and takes the lane up before the cable's root
// ports, whose SuperSpeed one goes on before and off before the USB 2.0 one.
TEST(SwitchUsbHostSource, SuperSpeedFollowsThePll)
{
    const std::string pcie = SourceText("../../SwitchWLAN/TegraPCIe.cpp");
    const std::string bringUp = Body(pcie, "bool TegraPcie::bringUp(");
    EXPECT_LT(at(bringUp, "switch_uphy_announce(false)"), at(bringUp, "powerUp(why, whyLength)"));
    const std::string uphy = Body(pcie, "bool TegraPcie::uphy(");
    EXPECT_EQ(Count(uphy, "switch_uphy_announce(true)"), 1u);
    EXPECT_LT(at(uphy, "kCtl1Lockdet"), at(uphy, "switch_uphy_announce(true)"));
    const std::string usb = SourceText("../SwitchUSB.cpp");
    EXPECT_THAT(Body(usb, "void uphyChanged("), HasSubstr("switch_usb_ss_clamp()"));
    const std::string suspend = Body(usb, "void switch_usb_suspend(");
    EXPECT_LT(at(suspend, "switch_uphy_lost()"), at(suspend, "switch_usb_ss_lost()"));
    const std::string apply = Body(usb, "void applyRole(");
    const size_t start = at(apply, "switchxhci_start(");
    EXPECT_LT(apply.find("followLane()", start), apply.find("switchxhci_cable(true)", start));
    const std::string lane = Body(SourceText("../XusbSuperSpeed.cpp"), "bool switch_usb_ss_lane(");
    EXPECT_LT(at(lane, "IOLockLock(gLock)"), at(lane, "switch_uphy_ready()"));
    const std::string cable = Body(SourceText("../SwitchXHCIEvents.cpp"), "void switchxhci_cable(");
    EXPECT_LT(at(cable, "followSuperSpeed(controller)"), at(cable, "xhost::forcePower(port, connected)"));
}

// After SC7 TegraSMMU has restored the host's own translation before the host comes back: the bring-up accepts it.
TEST(SwitchUsbHostSource, BringUpAcceptsItsOwnSmmuAsid)
{
    const std::string power = Body(SourceText("../XusbHost.cpp"), "bool Controller::powerOn(");
    EXPECT_THAT(power, HasSubstr("xusb_host_asid_usable(asid, TEGRA_SMMU_ASID_XUSB_HOST)"));
    EXPECT_THAT(SourceText("../UsbSmmuMapper.cpp"), HasSubstr("kAsid = TEGRA_SMMU_ASID_XUSB_HOST"));
}
