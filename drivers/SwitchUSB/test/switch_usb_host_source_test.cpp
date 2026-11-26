// What switch_usb_host_test's headers cannot show about the kernel side of USB host mode:
//   * the interrupt routing SwitchXHCI's comment claims, against the OLED DT (as decompiled in L4T's nx-platform
//     sources: the xusb node's interrupts) and the driver sources: GIC SPI 39/40 level-high, the LIC enabled before the
//     distributor, IPFS programmed as Linux's tegra_xusb_config, no timer and no soft path;
//   * the kernel files use the tested headers, in the order the model assumes (SwitchPD's enter, the pad's role before
//     the controller's first start);
//   * each kind's transfer-buffer cache policy and iOS 17's mapper answer.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "bazel/testing/source_text.h"

using switch_ios::test::Between;
using switch_ios::test::Block;
using switch_ios::test::Body;
using switch_ios::test::Configured;
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
    const std::string source = SourceText("../XusbHostPower.cpp") + SourceText("../XusbHost.cpp");
    const std::string registers = SourceText("../XusbHostRegisters.h");
    const std::string power = Body(SourceText("../XusbHostPower.cpp"), "bool Controller::powerOn(");
    const std::vector<std::string> order = {
        "rmw32(ipfs, IPFS_CONFIGURATION, 0, IPFS_EN_FPCI)", "rmw32(fpci, XUSB_CFG_4, CFG_4_BASE_MASK",
        "rmw32(fpci, XUSB_CFG_1, 0, CFG_1_ENABLES)", "rmw32(ipfs, IPFS_INTR_MASK, 0, IPFS_IP_INT_MASK)",
        "write32(ipfs, IPFS_CLKGATE_HYSTERESIS, 0x80)"};
    for (size_t i = 0; i + 1 < order.size(); ++i)
        EXPECT_LT(at(power, order[i]), at(power, order[i + 1])) << order[i] << " before " << order[i + 1];
    EXPECT_THAT(registers, HasSubstr("IPFS_CONFIGURATION = 0x180"));
    EXPECT_THAT(registers, HasSubstr("IPFS_INTR_MASK = 0x188"));
    EXPECT_THAT(registers, HasSubstr("IPFS_IP_INT_MASK = 1u << 16"));
    EXPECT_THAT(registers, HasSubstr("IPFS_INTR_ENABLE = 0x198"));
    // Linux never sets it: only tegra_xhci_restore_context puts back what was saved before SC7.
    EXPECT_THAT(source, Not(HasSubstr("write32(ipfs, IPFS_INTR_ENABLE")));
    EXPECT_THAT(Body(source, "kIpfsContext[] ="), HasSubstr("IPFS_INTR_ENABLE"));
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

// No timer and no soft path: a mailbox line that keeps firing with nothing to take panics, and nothing turns host mode
// off on its own.
TEST(SwitchUsbHostSource, NoTimersNoSoftPath)
{
    const std::string events = SourceText("../SwitchXHCIEvents.cpp");
    const std::string xhci = SourceText("../SwitchXHCI.cpp");
    for (const std::string &source : {events, xhci, SourceText("../SwitchUSBPort.cpp")}) {
        EXPECT_THAT(source, Not(HasSubstr("IOTimerEventSource")));
        EXPECT_THAT(source, Not(HasSubstr("setTimeout")));
    }
    EXPECT_THAT(SourceText("../SwitchUSB.cpp"), Not(HasSubstr("switch_usb_host_failed")));
    EXPECT_THAT(Body(events, "void SwitchXHCI::mailboxAction("), HasSubstr("if (self->mailboxStorm) {"));
    EXPECT_EQ(Count(Body(events, "void SwitchXHCI::mailboxAction("), "panic("), 2u);
}

// --- The kernel files use these headers, in the order the tests assume ----------------------------------------------

TEST(SwitchUsbHostSource, SwitchPdUsesThePolicy)
{
    const std::string source = SourceText("../SwitchPD.cpp");
    EXPECT_THAT(SourceText("../SwitchPDService.h"), HasSubstr("#include \"SwitchPDPolicy.h\""));
    EXPECT_THAT(source, HasSubstr("switch_pd_decide("));
    EXPECT_TRUE(Block(source, "void SwitchPD::handle(").has("allowSource && !asleepSinkOnly && switchesOn"));
    EXPECT_THAT(source, HasSubstr("switch_pd_window_end("));
    const auto enter = Block(source, "void SwitchPD::enter(");
    EXPECT_TRUE(enter.before("setSwitches(false);", "setRails(vbus, vconn)"));
    EXPECT_TRUE(enter.before("setRails(vbus, vconn)", "setSwitches(true);"));
}

TEST(SwitchUsbHostSource, SourcePowerIsReadyBeforeTheWaitSourceWindow)
{
    const std::string source = SourceText("../SwitchPD.cpp");
    const auto enter = Block(source, "void SwitchPD::enter(");
    EXPECT_TRUE(enter.has("if (boost && !boosting) {"));
    EXPECT_TRUE(enter.before("switch_battery_set_otg_boost(true,", "setSwitches(true);"));
    EXPECT_TRUE(enter.before("setSwitches(true);", "window->setTimeoutMS(kWaitSourceMS)"));
    const auto promote = Block(source, "void SwitchPD::startSourcing(");
    EXPECT_FALSE(promote.has("switch_battery_set_otg_boost"));
    EXPECT_TRUE(promote.has("sendPrSwap()"));
    const auto rails = Block(source, "bool SwitchPD::setRails(");
    EXPECT_TRUE(rails.before("if (vbus && switch_pd_vbus_ours(state.status1))", "tegra_gpio_output(kSourceRailGpio, 1)"));
    EXPECT_TRUE(rails.before("tegra_gpio_output(kSourceRailGpio, 1)", "IOSleep(kRailRampMS)"));
    EXPECT_TRUE(rails.before("IOSleep(kRailRampMS)", "tegra_gpio_output(kVconnGpio, 1)"));
}

TEST(SwitchUsbHostSource, ProvisionalSourcePowerHasLifecycleCleanup)
{
    const std::string source = SourceText("../SwitchPD.cpp");
    const auto enter = Block(source, "void SwitchPD::enter(");
    EXPECT_TRUE(enter.has("const bool boosting = switch_pd_boost_on(previous);"));
    EXPECT_TRUE(enter.has("if (boosting && !boost) (void)switch_battery_set_otg_boost(false, false);"));
    EXPECT_TRUE(enter.before("setSwitches(false);", "switch_battery_set_otg_boost(false, false)"));
    EXPECT_TRUE(enter.before("switch_battery_set_otg_boost(false, false)", "setRails(vbus, vconn)"));
    const auto leave = Block(SourceText("../SwitchPDSettings.cpp"), "void SwitchPD::leave(");
    EXPECT_TRUE(leave.has("if (switch_pd_boost_on(phase))"));
    EXPECT_TRUE(leave.before("setSwitches(false);", "switch_battery_set_otg_boost(false, false)"));
    EXPECT_TRUE(leave.before("switch_battery_set_otg_boost(false, false)", "setRails(false, false)"));
    EXPECT_TRUE(
        leave.has("restoreField(CONFIG2, CONFIG2_PR_SWAP_MASK | CONFIG2_DR_SWAP_MASK | CONFIG2_VSRC_SWAP, found.config2)"));
    const auto sleep = Block(source, "void SwitchPD::systemSleep(");
    EXPECT_TRUE(sleep.has("phase == SWITCH_PD_CLASSIFY || phase == SWITCH_PD_WAIT_SOURCE"));
    EXPECT_TRUE(sleep.before("window->cancelTimeout();", "enter(SWITCH_PD_EMPTY,"));
    EXPECT_TRUE(sleep.before("asleepSinkOnly =", "enter(SWITCH_PD_EMPTY,"));
    EXPECT_TRUE(sleep.before("setSwitches(false);", "setRails(false, false)"));
}

TEST(SwitchUsbHostSource, HostRoleUsesThePortHelpers)
{
    const std::string source = SourceText("../XusbHost.cpp") + SourceText("../XusbHostPower.cpp");
    EXPECT_THAT(SourceText("../XusbHost.cpp"), HasSubstr("#include \"XusbPorts.h\""));
    EXPECT_THAT(SourceText("../XusbHostPower.cpp"), HasSubstr("#include \"XusbPorts.h\""));
    for (const char *name : {"xhci_find_ports(", "xusb_vbus_id_host(", "xusb_chrg_ctl1_host("})
        EXPECT_THAT(source, HasSubstr(name));
    const std::string apply = Body(SourceText("../SwitchUSB.cpp"), "void applyRole(");
    // The pad goes to the host before the controller first starts.
    EXPECT_LT(at(apply, "switchxhci_set_host_role(true)"), at(apply, "switchxhci_start("));
}

// The host role's transports go to the port once the controller has started (applyLanes, at the end of the pass), and
// the device role takes them back before the pad leaves the host. Port power is Apple's own: the one PP write of ours
// is the SuperSpeed port's off for RESET_SSPI.
TEST(SwitchUsbHostSource, TheTransportsFollowTheRole)
{
    const std::string usb = SourceText("../SwitchUSB.cpp");
    const std::string apply = Body(usb, "void applyRole(");
    EXPECT_LT(at(apply, "switchxhci_start("), at(apply, "applyLanes(attached)"));
    EXPECT_THAT(Body(usb, "void applyLanes("), HasSubstr("portRole(attached)"));
    EXPECT_THAT(Body(usb, "enum xusb_port_role portRole("),
                HasSubstr("if (gHostRole)\n        return XUSB_PORT_HOST;"));
    const size_t device = at(apply, "} else if (!wantHost && gHostRole) {");
    EXPECT_LT(apply.find("switch_usb_port_follow(", device), apply.find("finishHostTeardown()", device));
    const std::string teardown = Body(usb, "void finishHostTeardown(");
    EXPECT_LT(at(teardown, "switch_usb_port_holding_retired() || !switchxhci_is_off()"),
              at(teardown, "switchxhci_set_host_role(false)"));
    EXPECT_LT(at(teardown, "switchxhci_set_host_role(false)"), at(teardown, "switchxudc_set_host_owns_port(false)"));
    for (const char *file : {"../SwitchXHCIServices.cpp", "../SwitchXHCIEvents.cpp", "../SwitchUSB.cpp",
                             "../SwitchUSBPort.cpp", "../SwitchXHCI.cpp", "../XusbHost.cpp"}) {
        EXPECT_THAT(SourceText(file), Not(HasSubstr("UsbForcePower"))) << file;
        const std::string name = file;
        const size_t writes = name == "../SwitchXHCIEvents.cpp" || name == "../XusbHost.cpp" ? 1u : 0u;
        EXPECT_EQ(Count(SourceText(file), "setPortPower("), writes) << file;
    }
    // Each transport type on its own (XusbPortTransports.h, xusb_port_transports_test): the port holds one of a type, so
    // nothing is added beside a live one of its type, and each transport has its role before the port has it.
    const std::string port = SourceText("../SwitchUSBPort.cpp");
    const std::string follow = Body(port, "void switch_usb_port_follow(");
    EXPECT_THAT(follow, HasSubstr("gTransports.follow(role, laneUp, superSpeedReset, why)"));
    EXPECT_THAT(port, Not(HasSubstr("gIndex")));
    // RESET_SSPI on the role thread with the SuperSpeed port's PP off and the controller running, owed from each host
    // role and controller reset; the USB 3 transport out while it is owed and back only after (Apple's port powers the
    // port for it: we never turn it on).
    const std::string events = SourceText("../SwitchXHCIEvents.cpp");
    const std::string reset = Body(events, "bool SwitchXHCI::resetSuperSpeed(");
    EXPECT_LT(at(reset, "!controllerRuns()"), at(reset, "__atomic_store_n(&sspiDue, false"));
    EXPECT_LT(at(reset, "__atomic_store_n(&sspiDue, false"), at(reset, "gHost.setPortPower(port, false)"));
    EXPECT_EQ(Count(reset, "&sspiDue"), 1u) << "a reset owed again during the power-off must not be cleared after it";
    EXPECT_LT(at(reset, "gHost.setPortPower(port, false)"),
              at(reset, "xusb::settle(\"SuperSpeed root port power-off\", 1, 700"));
    EXPECT_LT(at(reset, "xusb::settle(\"SuperSpeed root port power-off\", 1, 700"), at(reset, "MBOX_RESET_SSPI"));
    const std::string powerFailure = Body(reset, "if (!off && !interrupted())");
    EXPECT_THAT(powerFailure, HasSubstr("superSpeedResetDue();"));
    EXPECT_THAT(powerFailure, HasSubstr("return false;"));
    EXPECT_THAT(Body(reset, "if (!attempt.taken)"), HasSubstr("superSpeedResetDue();"));
    // AppleUSBXHCI halts, saves, resets and runs the controller again right after its first run: a halt or reset in
    // the power-off window ends the wait at its next look and defers the reset to the next run, without RESET_SSPI.
    const std::string header = SourceText("../SwitchXHCI.h");
    const std::string runs = Between(header, "static bool controllerRuns(", "\n    }\n");
    EXPECT_THAT(runs, HasSubstr("XHCI_USBCMD_RS"));
    EXPECT_THAT(runs, HasSubstr("XHCI_USBSTS_HCH"));
    EXPECT_THAT(reset, HasSubstr("superSpeedResetOwed() || !controllerRuns()"));
    EXPECT_THAT(reset, HasSubstr("== 0 || interrupted()"));
    EXPECT_LT(at(reset, "!round->controller->superSpeedResetOwed() && controllerRuns()"), at(reset, "MBOX_RESET_SSPI"));
    EXPECT_LT(at(reset, "MBOX_RESET_SSPI"), at(reset, "if (!attempt.sent || interrupted()) {"));
    const std::string deferred = reset.substr(at(reset, "if (!attempt.sent || interrupted()) {"));
    EXPECT_LT(at(deferred, "superSpeedResetDue();"), at(deferred, "return false;"));
    EXPECT_LT(at(deferred, "return false;"), at(deferred, "SuperSpeed reset taken"));
    EXPECT_THAT(events, Not(HasSubstr("setPortPower(port, true)")));
    const std::string held = Body(SourceText("../SwitchXHCI.cpp"), "uint32_t SwitchXHCI::heldWrite32(");
    EXPECT_LT(at(held, "XHCI_USBCMD_HCRST"), at(held, "switch_usb_reconsider()"));
    const std::string lanes = Body(usb, "void applyLanes(");
    EXPECT_LT(at(lanes, "switch_usb_port_follow(role, gSsUp, false)"), at(lanes, "switchxhci_reset_superspeed()"));
    EXPECT_LT(at(lanes, "switchxhci_reset_superspeed()"),
              at(lanes, "switch_usb_port_follow(role, gSsUp, superSpeedReset)"));
    const std::string create = Body(port, "void *createTransport(");
    EXPECT_LT(at(create, "switch_original_port_usb3_set_data_role("), at(create, "} else {"));
    EXPECT_THAT(Body(port, "bool addTransport("),
                HasSubstr("switch_original_port_add_transport(gPort, transport, index)"));
}

// The host role's transports outlive the device on the root port when the port empties: Apple's port sets their data
// rate and signalling frequencies as it destroys the device (iOS 17 logged "Transport has been terminated" when they
// went first). They stay until its termination, which wakes the role thread to take them out. A swap to the device role
// removes and adds instead (XusbPortTransports.h): the port holds one transport of a type.
TEST(SwitchUsbHostSource, TransportsOutliveTheRootPortDevice)
{
    const std::string port = SourceText("../SwitchUSBPort.cpp");
    const std::string follow = Body(port, "void switch_usb_port_follow(");
    EXPECT_LT(at(follow, "gRetiredDeviceGone"), at(follow, "gTransports.follow("));
    EXPECT_LT(at(follow, "retiresForDevice(role)"), at(follow, "gTransports.follow("));
    EXPECT_THAT(follow, HasSubstr("!watchActiveDevice()"));
    const std::string retires = Body(port, "bool retiresForDevice(");
    EXPECT_THAT(retires, HasSubstr("role != XUSB_PORT_EMPTY || gTransports.role() != XUSB_PORT_HOST"));
    EXPECT_THAT(retires, HasSubstr("watchActiveDevice()"));
    const std::string active = Body(port, "bool watchActiveDevice(");
    EXPECT_LT(at(active, "switchxhci_copy_root_device()"), at(active, "watchUntilGone("));
    const std::string watch = Body(port, "bool watchUntilGone(");
    EXPECT_THAT(watch, HasSubstr("gIOTerminatedNotification"));
    EXPECT_THAT(watch, HasSubstr("device->isInactive()"));
    EXPECT_THAT(Body(port, "bool deviceTerminated("), HasSubstr("thread_call_enter(gRoleCall)"));
    EXPECT_THAT(port, Not(HasSubstr("_delayed")));
    const std::string events = SourceText("../SwitchXHCIEvents.cpp");
    const std::string copy = Body(events, "IOService *switchxhci_copy_root_device(");
    EXPECT_THAT(copy, HasSubstr("kIORegistryIterateRecursively"));
    EXPECT_THAT(copy, HasSubstr("IOUSBHostDevice"));
    EXPECT_THAT(copy, HasSubstr("IOUSBHostInterface"));
    const std::string didChange = Body(events, "IOReturn SwitchXHCI::powerStateDidChangeTo(");
    EXPECT_LT(at(didChange, "AppleUSBXHCI::powerStateDidChangeTo("), at(didChange, "switch_usb_reconsider()"));
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
    EXPECT_LT(at(sleep, "gHost.loadFirmware("), at(sleep, "controller->enableMessages()"));
}

// A mailbox command completes from the interrupt (device boot of 2026-11-14: the firmware ACKs RESET_SSPI by message and
// the owner stays ours until that ACK is handled; MSG_ENABLED it releases without one): every command is sent on the
// mailbox's loop and sleeps on its answer with the gate open, mailboxAction wakes it, SC7 abandons it, and the owner is
// looked at twice, never in a loop.
TEST(SwitchUsbHostSource, MailboxCommandsCompleteFromTheInterrupt)
{
    const std::string events = SourceText("../SwitchXHCIEvents.cpp");
    const std::string xhci = SourceText("../SwitchXHCI.cpp");
    const std::string send = Body(events, "bool SwitchXHCI::sendCommand(");
    EXPECT_LT(at(send, "xusb_mbox_wait_begin(&commandWait, cmd)"), at(send, "gHost.sendMailbox(cmd, data)"));
    EXPECT_THAT(send, HasSubstr("commandGate->commandSleep(&commandWait, looks[look], THREAD_UNINT)"));
    EXPECT_THAT(send, HasSubstr("constexpr uint32_t kFirstLookMs = 1;"));
    EXPECT_THAT(send, HasSubstr("constexpr uint32_t kAnswerMs = 250;"));
    EXPECT_THAT(send, HasSubstr("xusb_mbox_released(gHost.mailboxOwner())"));
    const std::string action = Body(events, "void SwitchXHCI::mailboxAction(");
    EXPECT_LT(at(action, "gHost.serviceMailbox(answer, data)"),
              at(action, "commandGate->commandWakeup(&self->commandWait)"));
    const std::string asleep = Body(events, "IOReturn SwitchXHCI::systemSleep(");
    EXPECT_LT(at(asleep, "mailbox->disable()"), at(asleep, "xusb_mbox_wait_abandon(&commandWait)"));
    EXPECT_THAT(Body(events, "bool SwitchXHCI::enableMessages("), HasSubstr("tegraLoop->runAction(send, this)"));
    EXPECT_THAT(Body(xhci, "bool SwitchXHCI::start("), HasSubstr("enableMessages()"));
    EXPECT_THAT(Body(events, "bool SwitchXHCI::resetSuperSpeed("),
                HasSubstr("round->controller->sendCommand(xhost::MBOX_RESET_SSPI"));
    for (const std::string &source : {events, xhci}) {
        EXPECT_THAT(source, Not(HasSubstr("gHost.sendMailbox(xhost::MBOX_")));
    }
    EXPECT_LT(at(xhci, "commandGate = IOCommandGate::commandGate(this)"), at(xhci, "mailbox->enable()"));
}

// SuperSpeed's lane and PCIe's UPHY PLL (kits/SwitchCAR/SwitchUPHY.h): PCIe announces "not ready" before powerUp holds
// the UPHY in reset and "ready" only once its PLL has locked; SwitchUSB clamps on "not ready", reads the announcement
// under its lane lock, marks the PLL lost before the lane at suspend and takes the lane up before the USB-C port's
// transports follow it (XusbLanes.h xusb_transport_decide).
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
    const std::string lanes = Body(usb, "void applyLanes(");
    EXPECT_LT(at(lanes, "followLane(attached)"), at(lanes, "switch_usb_port_follow(role, gSsUp"));
    const std::string lane = Body(SourceText("../XusbSuperSpeed.cpp"), "bool switch_usb_ss_lane(");
    EXPECT_LT(at(lane, "switch_padctl_lock()"), at(lane, "switch_uphy_ready()"));
}

// After SC7 TegraSMMU has restored the host's own translation before the host comes back: the bring-up accepts it.
TEST(SwitchUsbHostSource, BringUpAcceptsItsOwnSmmuAsid)
{
    const std::string power = Body(SourceText("../XusbHostPower.cpp"), "bool Controller::powerOn(");
    EXPECT_THAT(power, HasSubstr("xusb_host_asid_usable(asid, TEGRA_SMMU_ASID_XUSB_HOST)"));
    EXPECT_THAT(SourceText("../UsbSmmuMapper.cpp"), HasSubstr("kAsid = TEGRA_SMMU_ASID_XUSB_HOST"));
}

// switch_usb_hold_test's Switch layer is SwitchXHCI's: USBSTS reads and writes, USBCMD writes and interrupter 0's
// ERSTBA (both widths) go through xusb_hold, and SC7 marks the held state lost.
TEST(SwitchUsbHostSource, RegistersGoThroughTheHold)
{
    const std::string xhci = SourceText("../SwitchXHCI.cpp");
    EXPECT_THAT(Body(xhci, "uint32_t SwitchXHCI::regRead32("), HasSubstr("xusb_hold_usbsts(&hold, value)"));
    const std::string held = Body(xhci, "uint32_t SwitchXHCI::heldWrite32(");
    EXPECT_THAT(held, HasSubstr("xusb_hold_usbsts_write(&hold, value)"));
    EXPECT_THAT(held, HasSubstr("xusb_hold_usbcmd(&hold, value) | XHCI_USBCMD_EU3S"));
    const std::string keeps = Body(xhci, "bool SwitchXHCI::keepsEventRing(");
    EXPECT_THAT(keeps, HasSubstr("runtimeOffset + XHCI_ERSTBA0"));
    EXPECT_LT(at(keeps, "!xusb_hold_retained(&hold)"), at(keeps, "AppleUSBXHCI::regRead64("));
    EXPECT_LT(at(keeps, "!xusb_hold_retained(&hold)"), at(keeps, "AppleUSBXHCI::regRead32("));
    const std::string write32 = Body(xhci, "void SwitchXHCI::regWrite32(");
    EXPECT_LT(at(write32, "heldWrite32(address, value)"), at(write32, "AppleUSBXHCI::regWrite32("));
    EXPECT_LT(at(write32, "keepsEventRing(address, value, 4)"), at(write32, "AppleUSBXHCI::regWrite32("));
    const std::string write64 = Body(xhci, "void SwitchXHCI::regWrite64(");
    EXPECT_LT(at(write64, "keepsEventRing(base + offset, value, 8)"), at(write64, "AppleUSBXHCI::regWrite64("));
    const std::string sleep = Body(SourceText("../SwitchXHCIEvents.cpp"), "IOReturn SwitchXHCI::systemSleep(");
    EXPECT_LT(at(sleep, "gHost.lostPower()"), at(sleep, "xusb_hold_lost(&hold)"));
}

// A saved state SC7 took is reported as lost power when the system wakes (AppleUSBXHCIARM's hibernate power loss), on
// AppleUSBXHCI's work loop, and only then: from Off, or after a controller reset, there is nothing to report.
TEST(SwitchUsbHostSource, Sc7ReportsLostPower)
{
    const std::string events = SourceText("../SwitchXHCIEvents.cpp");
    const std::string sleep = Body(events, "IOReturn SwitchXHCI::systemSleep(");
    EXPECT_THAT(sleep, HasSubstr("hold.held && getPowerState() == kSavedPowerState"));
    EXPECT_LT(at(sleep, "&stateLost"), at(sleep, "xusb_hold_lost(&hold)"));
    const std::string wake = Body(events, "IOReturn SwitchXHCI::powerStateWillChangeTo(");
    EXPECT_THAT(wake, HasSubstr("whatDevice == getPMRootDomain()"));
    EXPECT_THAT(wake, HasSubstr("!(capabilities & kIOPMSleepCapability)"));
    EXPECT_LT(at(wake, "__atomic_exchange_n(&stateLost, false"), at(wake, "hardwareException(kLostPower)"));
    EXPECT_THAT(wake, HasSubstr("getWorkLoop()->runAction(lostPower, this)"));
    const std::string xhci = SourceText("../SwitchXHCI.cpp");
    const std::string held = Body(xhci, "uint32_t SwitchXHCI::heldWrite32(");
    EXPECT_LT(at(held, "XHCI_USBCMD_HCRST"), at(held, "__atomic_store_n(&stateLost, false"));
    EXPECT_THAT(Body(xhci, "bool SwitchXHCI::start("), HasSubstr("getPMRootDomain()->registerInterestedDriver(this)"));
    EXPECT_THAT(Body(xhci, "void SwitchXHCI::stop("), HasSubstr("getPMRootDomain()->deRegisterInterestedDriver(this)"));
}

// Development builds' lines for a device boot, which tools/devicerun keys on (Apple's IOPort lines are debug level and
// a successful power change logs nothing): the IOPort found, each transport added or removed with its role and reason,
// each root port created, each controller power state change, the SuperSpeed reset requested, deferred and taken.
// Release builds carry none of them.
TEST(SwitchUsbHostSource, DeviceBootLogLines)
{
    const std::vector<std::pair<const char *, std::vector<const char *>>> lines = {
        {"../SwitchUSBPort.cpp",
         {"\"SwitchUSB: USB-C port %s@%s found (port type %d, port number %d)\"",
          "\"SwitchUSB: USB-C port transport %s: USB %u, index %u, data role %s (%s)\""}},
        {"../SwitchXHCI.cpp", {"\"SwitchUSB: root port %u created as %s (%s, %s)\""}},
        {"../SwitchXHCIEvents.cpp",
         {"\"SwitchUSB: controller power state %lu -> %lu\"",
          "\"SwitchUSB: SuperSpeed reset requested on port %u (PORTSC %#x)\"",
          "\"SwitchUSB: SuperSpeed reset on port %u deferred: controller halted or reset (PORTSC %#x -> %#x)\"",
          "\"SwitchUSB: SuperSpeed reset taken on port %u (PORTSC %#x -> %#x, RESET_SSPI %s)\""}},
    };
    for (const auto &[file, formats] : lines) {
        const std::string source = SourceText(file);
        EXPECT_THAT(Configured(source, "DEVELOPMENT || DEBUG", false), Not(HasSubstr("os_log("))) << file;
        for (const char *format : formats)
            EXPECT_THAT(Configured(source, "DEVELOPMENT || DEBUG", true), HasSubstr(format)) << file;
    }
}

TEST(SwitchUsbHostSource, OneInterrupterAsTheFalconReportsIt)
{
    const std::string xhci = SourceText("../SwitchXHCI.cpp");
    EXPECT_THAT(Body(xhci, "uint32_t SwitchXHCI::regRead32("), Not(HasSubstr("0x7ff")));
    const std::string header = SourceText("../SwitchXHCI.h");
    EXPECT_THAT(Configured(header, "XNU_12377_121_6", true), Not(HasSubstr("uint32_t shadow[")));
    EXPECT_THAT(Configured(header, "XNU_12377_121_6", false),
                HasSubstr("uint32_t shadow[xhost::kAppleInterrupters][8];"));
    EXPECT_THAT(Configured(header, "XNU_12377_121_6", false), HasSubstr("kAppleInterrupters = 3"));
    const std::string shadow27 = Body(Configured(xhci, "XNU_12377_121_6", true), "uint32_t *SwitchXHCI::shadowFor(");
    EXPECT_THAT(shadow27, HasSubstr("return nullptr;"));
    EXPECT_THAT(shadow27, Not(HasSubstr("shadow[")));
}

// The USB-C port never presents as the donor iPad's Lightning port: the controller's provider carries no "port-type"
// or "port-number", and the only pair SwitchXHCI writes is the USB-C port's (2, 1), the IOPort adt_edit.py publishes.
TEST(SwitchUsbHostSource, OnlyTheUsbCPortIdentity)
{
    EXPECT_THAT(SourceText("../SwitchXHCIServices.cpp"), Not(HasSubstr("port-type")));
    const std::string xhci = SourceText("../SwitchXHCI.cpp");
    EXPECT_EQ(Count(xhci, "\"port-type\""), 1u);
    EXPECT_THAT(Body(xhci, "void SwitchXHCI::nameUSBCPort("), HasSubstr("uint32_t type = kSwitchUSBCPortType"));
    const std::string port = SourceText("../SwitchUSBPort.h");
    EXPECT_THAT(port, HasSubstr("kSwitchUSBCPortType = 2"));
    EXPECT_THAT(port, HasSubstr("kSwitchUSBCPortNumber = 1"));
}

// Every pad controller register that two of the XUDC, the host, pad 0's bring-up and the SuperSpeed lane write is
// read-modified-written under the pad controller lock (XusbPadctl.h): SS_PORT_MAP, USB2_PAD_MUX, USB2_PORT_CAP,
// USB2_VBUS_ID, ELPG_PROGRAM_1 and pad 0's controls; the PMC's USB_AO through the kit's locked read-modify-write.
TEST(SwitchUsbHostSource, SharedPadRegistersTakeThePadLock)
{
    const std::string xudc = SourceText("../XudcHardware.cpp");
    EXPECT_EQ(Count(xudc, "switch_mmio_write32(padctl"), 0u);
    EXPECT_EQ(Count(xudc, "switch_mmio_rmw32(padctl"), 0u);
    EXPECT_THAT(xudc, HasSubstr("switch_pmc_rmw(USB_AO, 0xcu, 0)"));
    const std::string host = SourceText("../XusbHostPower.cpp");
    const std::string power = Body(host, "bool Controller::powerOn(");
    EXPECT_EQ(Count(power, "switch_mmio_rmw32(padctl"), 0u);
    EXPECT_EQ(Count(power, "switch_mmio_write32(padctl"), 0u);
    EXPECT_THAT(power, HasSubstr("switch_pmc_rmw(USB_AO, 0xcu, 0)"));
    const std::string role = Body(host, "void Controller::setHostRole(");
    EXPECT_LT(at(role, "const SwitchPadctlLocked locked;"), at(role, "switch_mmio_rmw32(padctl"));
    const std::string pad = Body(SourceText("../XusbUtmiPad.cpp"), "void powerOn(");
    EXPECT_LT(at(pad, "const SwitchPadctlLocked locked;"), at(pad, "switch_mmio_rmw32(padctl"));
}

// The B01 lane register-file data goes in on a T210B01 only, as L4T's tegra210_pex_uphy_enable gates
// tegra210_xusb_lane_defaults.
TEST(SwitchUsbHostSource, B01LaneDataOnB01Only)
{
    const std::string up = Body(SourceText("../XusbSuperSpeed.cpp"), "void laneUp(");
    ASSERT_FALSE(up.empty());
    EXPECT_LT(at(up, "switch_car_soc_t210b01()"), at(up, "xusb_b01_usb3_lane_data"));
}

// PCIe's lane 0 IDDQ write is followed by 1 us before the AUX clamps are released (Tegra X1 TRM 22.8.4 step 8), all
// under the pad controller lock.
TEST(SwitchUsbHostSource, PcieWaitsAfterIddqBeforeAuxClamps)
{
    const std::string uphy = Body(SourceText("../../SwitchWLAN/TegraPCIe.cpp"), "bool TegraPcie::uphy(");
    const size_t iddq = at(uphy, "p.modify(PADCTL_USB3_PAD_MUX");
    const size_t wait = uphy.find("IODelay(1);", iddq);
    ASSERT_NE(wait, std::string::npos);
    EXPECT_LT(at(uphy, "const SwitchPadctlLocked locked;"), iddq);
    EXPECT_LT(wait, at(uphy, "p.modify(PADCTL_ELPG_PROGRAM1"));
}

// Sleepwalk: the line debounce above 1 (Tegra X1 TRM 12.6.60) and 1 us after the detectors power up (22.8.9.2), not a
// 100 us spin.
TEST(SwitchUsbHostSource, SleepwalkTimesAsTheTrm)
{
    const std::string arm = SourceText("../XusbSleepwalk.cpp");
    EXPECT_THAT(arm, HasSubstr("XUSB_DEBOUNCE_LINE_TWO"));
    EXPECT_THAT(arm, Not(HasSubstr("IODelay(100)")));
    EXPECT_LT(at(arm, "switch_pmc_rmw(XUSB_PMC_USB_AO, XUSB_USB_AO_LINE_PD, 0);"), at(arm, "IODelay(1);"));
}

// AppleUSBHostController's client buffer options, beside the ring options in each kind's table, set write-combined
// before AppleUSBXHCI::start makes the first family buffer.
TEST(SwitchUsbHostSource, FamilyBuffersWriteCombined)
{
    const std::string interface = SourceText("../../../kits/SwitchABI/interface/AppleUSBXHCI.json");
    EXPECT_THAT(Between(interface, "\"clientBufferOptions\": {", "}"), HasSubstr("\"access\": \"get set\""));
    const std::string ios17 = SourceText("../../../kits/SwitchABI/kinds/21H461/AppleUSBXHCI.json");
    EXPECT_THAT(ios17, HasSubstr("\"ringBufferOptions\": \"0x494\""));
    EXPECT_THAT(ios17, HasSubstr("\"clientBufferOptions\": \"0x498\""));
    const std::string ios27 = SourceText("../../../kits/SwitchABI/kinds/24B119/AppleUSBXHCI.json");
    EXPECT_THAT(ios27, HasSubstr("\"ringBufferOptions\": \"0x5ac\""));
    EXPECT_THAT(ios27, HasSubstr("\"clientBufferOptions\": \"0x5b0\""));
    const std::string xhci = SourceText("../SwitchXHCI.cpp");
    EXPECT_THAT(Body(Configured(xhci, "XNU_12377_121_6", true), "bool SwitchXHCI::start("),
                Not(HasSubstr("setClientBufferOptions(")));
    const std::string start = Body(Configured(xhci, "XNU_12377_121_6", false), "bool SwitchXHCI::start(");
    const std::string fromSet = start.substr(
        at(start, "setClientBufferOptions((clientBufferOptions() & ~static_cast<uint32_t>(kIOMapCacheMask))"));
    EXPECT_THAT(fromSet.substr(0, 200), HasSubstr("kIOMapWriteCombineCache"));
    EXPECT_THAT(fromSet, HasSubstr("AppleUSBXHCI::start(provider)"));
}

TEST(SwitchUsbHostSource, PlainClientBuffersCachedOnIos17Only)
{
    const std::string xhci = SourceText("../SwitchXHCI.cpp");
    EXPECT_THAT(Configured(xhci, "XNU_12377_121_6", true), Not(HasSubstr("SwitchXHCI::createIOBuffer(")));
    const std::string allocation =
        Body(Configured(xhci, "XNU_12377_121_6", false), "IOBufferMemoryDescriptor *SwitchXHCI::createIOBuffer(");
    EXPECT_THAT(allocation, HasSubstr("if ((options & kIOMapCacheMask) == 0u)"));
    EXPECT_THAT(allocation, HasSubstr("options |= kIOMapCopybackCache;"));
    EXPECT_THAT(allocation, HasSubstr("return AppleUSBXHCI::createIOBuffer(options, capacity);"));
    EXPECT_THAT(SourceText("../../../kits/SwitchABI/kinds/21H461/AppleUSBXHCI.json"),
                HasSubstr("\"229\": \"createIOBuffer\""));
    EXPECT_THAT(SourceText("../../../kits/SwitchABI/kinds/24B119/AppleUSBXHCI.json"),
                HasSubstr("\"231\": \"createIOBuffer\""));
}

// iOS 17 only: the mapper answers getProtectionGranularity with 1, written as the 32 bits the controller reads back.
TEST(SwitchUsbHostSource, ProtectionGranularityOnIos17Only)
{
    const std::string mapper = SourceText("../UsbDmaMapper.cpp");
    EXPECT_THAT(Configured(mapper, "XNU_10063_141_1", false), Not(HasSubstr("getProtectionGranularity")));
    const std::string ios17 = Configured(mapper, "XNU_10063_141_1", true);
    const std::string answer = Body(ios17, "IOReturn SwitchDMAMapper::callPlatformFunction(");
    EXPECT_LT(at(answer, "isEqualTo(\"getProtectionGranularity\")"),
              at(answer, "*static_cast<uint32_t *>(param1) = 1;"));
    EXPECT_THAT(answer, HasSubstr("return IOMapper::callPlatformFunction(functionName, waitForFunction, param1"));
}

// Nothing reads a mapping's data before its unmap's maintenance: the probe's complete lines come after it, and the
// only bytes read before a bounce's copy back are the pool page's.
TEST(SwitchUsbHostSource, NoProbeReadBeforeTheUnmapMaintenance)
{
    const std::string unmap = Body(SourceText("../UsbDmaMapper.cpp"), "IOReturn SwitchDMAMapper::iovmUnmapMemory(");
    EXPECT_LT(at(unmap, "maintain(memory, translatedOffset, translatedLength, plan.atUnmap);"),
              at(unmap, "usb_dma_probe_unmap(memory, translatedOffset"));
    EXPECT_LT(at(unmap, "switch_dcache_phys_lines(SWITCH_DCACHE_CLEAN_INVALIDATE, mapAddress, mapLength);"),
              at(unmap, "usb_dma_probe_physical_after("));
    EXPECT_THAT(
        unmap,
        HasSubstr("\"bounced\",\n                        poolVA + static_cast<uint64_t>(done.firstPage) * kPage"));
    EXPECT_THAT(SourceText("../UsbDmaProbe.h"), Not(HasSubstr("usb_dma_probe_before")));
    const std::string held = SourceText("../UsbDmaProbeHeld.cpp");
    EXPECT_THAT(held, Not(HasSubstr("readCached")));
    EXPECT_THAT(held, Not(HasSubstr("readUncached")));
    EXPECT_THAT(held, Not(HasSubstr("switch_dcache")));
}

// usb_static_map_test's high-address window cases drive the composition the kernel uses: the pieces checked against
// 34-bit RAM, each window page's entry written by the domain.
TEST(SwitchUsbHostSource, WindowMapIsTheTestedComposition)
{
    const std::string map = Body(SourceText("../UsbSmmuMapper.cpp"), "bool switch_usb_smmu_map(");
    EXPECT_THAT(map, HasSubstr("usb_iova_page_aligned(segments(memory, offset), length, TegraSMMUDomain::kLimit * 4"));
    EXPECT_THAT(map, HasSubstr("usb_iova_map_pages(segments(memory, offset), length, pages, mapPage, &mapped)"));
    EXPECT_THAT(SourceText("../UsbSmmuSpace.h"),
                HasSubstr("domain_.map(base + static_cast<uint64_t>(i) * UsbIovaWindow::kPage, &pa, 1, true)"));
}

TEST(SwitchUsbHostSource, BouncedMappingOwnsACompleteSnapshotBeforePublication)
{
    const std::string mapper = SourceText("../UsbDmaMapper.cpp");
    const std::string bounce = Body(mapper, "IOReturn SwitchDMAMapper::bounce(");
    EXPECT_LT(at(bounce, "length > kPoolBytes"), at(bounce, "allocatePages(pages, alignment)"));
    EXPECT_LT(at(bounce, "if (memory->readBytes(offset, bounce, length) != length)"),
              at(bounce, "releasePages(static_cast<uint32_t>(first), pages);"));
    EXPECT_LT(at(bounce, "return kIOReturnUnderrun;"), at(bounce, "*mapAddress = address;"));
    EXPECT_THAT(bounce, Not(HasSubstr("bzero(bounce")));
    const std::string unmap = Body(mapper, "IOReturn SwitchDMAMapper::iovmUnmapMemory(");
    EXPECT_LT(at(unmap, "memory->writeBytes(done.offset"), at(unmap, "releasePages(done.firstPage, done.pages)"));
    EXPECT_LT(at(unmap, "releasePages(done.firstPage, done.pages)"), at(unmap, "return kIOReturnUnderrun;"));
}

// The mapper takes no reference on what it maps: IODMACommand holds the descriptor from prepare to complete, and Apple's
// family buffers start the teardown that unmaps them only once nothing but their own command holds them.
TEST(SwitchUsbHostSource, MapperHoldsNoDescriptor)
{
    const std::string mapper = SourceText("../UsbDmaMapper.cpp");
    for (const char *function : {"IOReturn SwitchDMAMapper::iovmMapMemory(", "IOReturn SwitchDMAMapper::bounce(",
                                 "IOReturn SwitchDMAMapper::iovmUnmapMemory("}) {
        const std::string body = Body(mapper, function);
        ASSERT_FALSE(body.empty()) << function;
        EXPECT_THAT(body, Not(HasSubstr("->retain()"))) << function;
        EXPECT_THAT(body, Not(HasSubstr("->release()"))) << function;
    }
}
