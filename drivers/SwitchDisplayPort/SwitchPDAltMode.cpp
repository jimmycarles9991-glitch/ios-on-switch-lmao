// SPDX-License-Identifier: GPL-2.0-only
// The BM92T36 alternate-mode engine on SwitchPD's work loop: SwitchPDAltModeSteps.h decides, this file does the I2C,
// the step timer and the hand-offs. DisplayPort's lanes go to SwitchDisplayPort when it is built in; the SuperSpeed
// pair's fate and the dock's hub go to SwitchUSB (XusbLanes.h switch_usb_set_alt_lanes).
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>

#include "../TegraI2C/TegraI2C.h"
#include "SwitchPDAltModeHook.h"
#include "SwitchPDAltModeSteps.h"
#if SWITCH_DISPLAYPORT
#include "SwitchDisplayPort.h"
#endif
#include "../SwitchUSB/XusbLanes.h"

namespace {
constexpr unsigned kBus = 1;
constexpr uint8_t kPD = 0x18;

enum : uint8_t {
    COMMAND = 0x05,
    DP_STATUS = 0x18,
    INCOMING_VDM = 0x50,
    OUTGOING_VDM = 0x60
};

#if SWITCH_DISPLAYPORT
constexpr bool kWantDisplayPort = true;
#else
constexpr bool kWantDisplayPort = false;
#endif

IOTimerEventSource *gTimer;
const void *gPD;
bool (*gAtRest)(const void *);
struct switch_pd_alt gAlt;
uint16_t gStatus1;
enum switch_pd_alt_lanes gToldLanes = SWITCH_PD_ALT_LANES_FREE;
bool gToldHub;
unsigned gToldDisplayPort;

void hand_off()
{
    if (gAlt.lanes != gToldLanes || gAlt.hub_enabled != gToldHub) {
        enum switch_usb3_lanes lanes = SWITCH_USB3_LANES_FREE;
        if (gAlt.lanes == SWITCH_PD_ALT_LANES_PENDING) {
            lanes = SWITCH_USB3_LANES_PENDING;
        } else if (gAlt.lanes == SWITCH_PD_ALT_LANES_TAKEN) {
            lanes = SWITCH_USB3_LANES_TAKEN;
        }
        switch_usb_set_alt_lanes(lanes, gAlt.hub_enabled);
    }
    gToldLanes = gAlt.lanes;
    gToldHub = gAlt.hub_enabled;
#if SWITCH_DISPLAYPORT
    if (gAlt.dp_lanes != gToldDisplayPort) {
        switch_displayport_alt_mode(gAlt.dp_lanes, (gStatus1 & SWITCH_PD_ALT_STATUS1_CSIDE) != 0);
    }
#endif
    gToldDisplayPort = gAlt.dp_lanes;
}

void perform(struct switch_pd_alt_out out)
{
    if (out.act == SWITCH_PD_ALT_READ) {
        uint8_t incoming[SWITCH_PD_ALT_INCOMING_MAX] = {};
        if (tegra_i2c_read(kBus, kPD, INCOMING_VDM, incoming, sizeof(incoming)) != 0) {
            out = switch_pd_alt_finish(&gAlt, SWITCH_PD_ALT_FAILED, "INCOMING_VDM could not be read");
        } else {
            out = switch_pd_alt_reply(&gAlt, incoming, sizeof(incoming));
        }
    }
    if (out.act == SWITCH_PD_ALT_COMMAND) {
        const uint8_t command[2] = {static_cast<uint8_t>(out.command), static_cast<uint8_t>(out.command >> 8)};
        const bool written =
            (out.message_len == 0 || tegra_i2c_write(kBus, kPD, OUTGOING_VDM, out.message, out.message_len) == 0) &&
            tegra_i2c_write(kBus, kPD, COMMAND, command, sizeof(command)) == 0;
        if (!written) {
            out = switch_pd_alt_finish(&gAlt, SWITCH_PD_ALT_FAILED, "the command could not be written");
        }
    }
    if (out.line != nullptr) {
        IOLog("SwitchPD: alternate mode (%s): %s\n", switch_pd_alt_step_name(gAlt.step), out.line);
    }
    if (out.timer_ms != 0) {
        gTimer->setTimeoutMS(out.timer_ms);
    } else {
        gTimer->cancelTimeout();
    }
    hand_off();
}

void timerFired(OSObject *, IOTimerEventSource *)
{
    perform(switch_pd_alt_timer(&gAlt, gAtRest(gPD)));
}
}  // namespace

void switch_pd_alt_alert(IOService *pd, IOWorkLoop *loop, bool (*at_rest_of)(const void *), uint16_t alert,
                         uint16_t status1, bool sink_session, bool dock_power)
{
    if (gTimer == nullptr) {
        switch_pd_alt_init(&gAlt, kWantDisplayPort);
        gTimer = IOTimerEventSource::timerEventSource(pd, &timerFired);
        if (gTimer == nullptr || loop->addEventSource(gTimer) != kIOReturnSuccess) {
            panic("SwitchPD: cannot add the alternate-mode timer to the PD work loop");
        }
        gPD = pd;
        gAtRest = at_rest_of;
    }
    const bool at_rest = at_rest_of(pd);
    gStatus1 = status1;
    uint16_t dp_status = 0;
    if (gAlt.step == SWITCH_PD_ALT_IDLE && sink_session && at_rest) {
        uint8_t raw[2] = {};
        (void)tegra_i2c_read(kBus, kPD, DP_STATUS, raw, sizeof(raw));
        dp_status = static_cast<uint16_t>(raw[0] | static_cast<unsigned>(raw[1]) << 8U);
    }
    const struct switch_pd_alt_in in = {alert, status1, dp_status, sink_session, at_rest, dock_power};
    const enum switch_pd_alt_step before = gAlt.step;
    const struct switch_pd_alt_out out = switch_pd_alt_event(&gAlt, &in);
    if (out.act == SWITCH_PD_ALT_NONE && out.line == nullptr && gAlt.step == before) {
        hand_off();
        return;
    }
    perform(out);
}
